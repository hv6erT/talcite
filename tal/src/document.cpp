#include "tal/document.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "tal/editing.hpp"
#include "tal/fs.hpp"
#include "tal/pages.hpp"
#include "tal/window.hpp"

#include "damage.hpp"
#include "locating.hpp"

namespace tal {

namespace {

using namespace std::chrono_literals;

constexpr std::size_t most_damaged = 64;
constexpr std::chrono::milliseconds checked_within = 1500ms;

// Whether every one of the paths is there, as the files of the defaults see it.
Task<bool> present(std::vector<Path> paths) {
    for (const Path& one : paths) {
        try {
            (void)co_await fs::look(one);
        } catch (const Trouble&) {
            co_return false;
        }
    }

    co_return true;
}

}

// A timer: what it does, when, whether it still waits, the handle that owns it and its place among
// timers due at the same moment.
struct Timer::State {
    Fn<Timer> what;
    std::shared_ptr<Clock> clock;
    std::chrono::steady_clock::time_point when;
    bool waiting = true;
    Timer* handle = nullptr;
    std::uint64_t order = 0;
};

// The handle of the timer `state`.
Timer::Timer(std::shared_ptr<State> state) : state_(std::move(state)) { state_->handle = this; }

// The timer goes with its handle and never rings after.
Timer::~Timer() {
    if (state_ != nullptr) state_->handle = nullptr;
}

// Takes over the timer of `other`, which is left empty.
Timer::Timer(Timer&& other) noexcept : state_(std::move(other.state_)) {
    other.state_.reset();

    if (state_ != nullptr) state_->handle = this;
}

// Lets go of the timer held and takes over the one of `other`.
Timer& Timer::operator=(Timer&& other) noexcept {
    if (this == &other) return *this;

    if (state_ != nullptr) state_->handle = nullptr;

    state_ = std::move(other.state_);
    other.state_.reset();

    if (state_ != nullptr) state_->handle = this;

    return *this;
}

// Sets the timer to ring `wait` from now by its document's clock, whether or not it rang.
void Timer::again(std::chrono::milliseconds wait) {
    if (state_ == nullptr) return;

    state_->when = state_->clock->now() + wait;
    state_->waiting = true;
}

// Whether the timer is set and has not rung yet.
bool Timer::waiting() const { return state_ != nullptr && state_->waiting; }

// The root of a tree of nodes, with the built-in sheet and a screen of no size until `display`.
// The clock serves the timers that come with events, the processes open the addresses followed.
// Its own default actions: Tab and Shift+Tab move the focus, a press of a pointing button gives it
// to the deepest node on the way that can take it. In a debug build Ctrl+F12 opens the view of the
// tree (`inspected`) before anything else hears it.
Document::Document(Port<Clock> time, Port<Processes> processes) : time_(std::move(time)), processes_(std::move(processes)) {
    document_ = this;

#ifndef NDEBUG
    own_.add(on<KeyEvent>(
        [this](KeyEvent& one) {
            const Mods mods = one.key.mods();

            if (one.key.code() != Code::f12 || !mods.ctrl || mods.alt || mods.shift) return;

            one.take();
            inspected();
        },
        Phase::capture));
#endif

    own_.add(otherwise<KeyEvent>([this](KeyEvent& one) {
        const Mods mods = one.key.mods();

        if (one.key.code() != Code::tab || mods.ctrl || mods.alt) return;

        Node* next = following(focus_, mods.shift, nullptr);

        if (next == nullptr) return;

        one.take();
        moved(next);
    }));

    own_.add(otherwise<MouseEvent>([this](MouseEvent& one) {
        if (one.press != Press::down || one.button == MouseButton::back || one.button == MouseButton::forward) return;

        for (Node* at = &one.target(); at != nullptr; at = at->parent_)
            if (at->focusable()) {
                moved(at);

                return;
            }
    }));
}

// Takes another sheet: every node is styled again and the whole screen painted again, since the
// glyphs may differ too.
void Document::sheet(StyleSheet given) {
    changing();

    sheet_ = std::move(given);

    restyle();
    damaged(Area{{0, 0}, display_.screen});
}

// Says what the document is shown on: the size of the screen in cells, its color level, whether
// it shows Unicode, how it renders clusters (whether it joins emoji sequences and presents
// pictographs as emoji), and how it shows pictures: graphics, block characters and the size of a
// cell in pixels. A new size is a new surface, laid out and painted whole; a new rendering changes
// the width of text, and new pictures change what a picture asks for, so every node is measured
// again; a new level styles every node again; Unicode changes the glyphs, so everything is painted
// again.
void Document::display(Display shown) {
    if (shown == display_) return;

    changing();

    const Display was = display_;

    display_ = shown;

    if (shown.screen != was.screen || shown.rendering != was.rendering) {
        surface_ = Surface{shown.screen, shown.rendering};

        relayout();
    }

    if (shown.rendering != was.rendering || shown.graphics != was.graphics || shown.blocks != was.blocks || shown.cell != was.cell) {
        std::vector<Node*> waiting{this};

        while (!waiting.empty()) {
            Node* one = waiting.back();

            waiting.pop_back();

            one->sizing_ = true;
            one->arranging_ = true;

            for (const std::shared_ptr<Node>& child : one->children_.held_) waiting.push_back(child.get());
        }
    }

    if (shown.colors != was.colors) restyle();

    damaged(Area{{0, 0}, display_.screen});
}

// Whether a frame would change anything: a node to style, measure or arrange, or a place to paint.
bool Document::due() const { return restyling_ || sizing_ || arranging_ || !damage_.empty(); }

// Makes a frame: styles the nodes that wait for it, measures and arranges from the root, skipping
// what has not changed, and paints the damaged places only, each node that meets one of them. The
// surface's damage is what this frame changed.
const Surface& Document::frame() {
    struct Ending {
        Document& one;

        ~Ending() { one.stage_ = Stage::idle; }
    } ending{*this};

    ++frames_;
    tally_ = Tally{};
    surface_.forget();

    stage_ = Stage::styling;

    if (restyling_) {
        restyling_ = false;
        styled(*this);
    }

    stage_ = Stage::laying;

    if (sizing_ || arranging_) {
        (void)measured(Limits{display_.screen, display_.screen});
        placed(Area{{0, 0}, display_.screen});
    }

    stage_ = Stage::settling;

    if (arranged_in_ == frames_) {
        Arranged one;

        notified(one);
    }

    const std::vector<std::shared_ptr<Node>> settled = std::move(arranged_);

    arranged_.clear();

    for (const std::shared_ptr<Node>& one : settled) {
        if (one->document_ != this) continue;

        Arranged told;

        one->notified(told);
    }

    stage_ = Stage::painting;

    const std::vector<Area> parts = std::move(damage_);
    const Area screen{{0, 0}, display_.screen};

    damage_.clear();

    for (const Area& part : parts) painted(*this, Spot{0, 0}, screen, part.common(screen));

    return surface_;
}

// Notes that `part` of the screen must be painted again.
void Document::damaged(Area part) { damage::added(damage_, part, most_damaged); }

// Styles `one` and everything under it that waits for a style. A new box lays the node out again,
// a new look only paints it again.
void Document::styled(Node& one) {
    if (!one.styled_) {
        const std::vector<std::vector<std::string>> classes = one.lineage();
        const Style made = sheet_.style(Selection{classes, one.states_, {}}, display_.colors);

        ++tally_.styled;

        if (made.border != one.style_.border || made.padding != one.style_.padding || made.margin != one.style_.margin ||
            made.shadow != one.style_.shadow) {
            // The shadow lies outside the box, so where it fell before is painted again too.
            if (made.shadow != one.style_.shadow) one.repaint();

            one.style_ = made;
            one.relayout();
        } else if (made.look != one.style_.look) {
            one.style_ = made;
            one.repaint();
        }

        one.styled_ = true;
    }

    for (Node& child : one.children_) styled(child);
}

// Paints `one`, whose parent stands at `origin` on the screen, where it meets `clip` (what its
// parent shows) and `part` (the damage being painted): its background and border, its content,
// then its children in order, the later on top. A node that does not meet them is skipped with
// everything under it, since children show only inside their parent. What `outline` draws goes on
// the box after the border. A node whose style casts a shadow casts it last, over what lies before
// it and inside its parent, also where the damage meets only the shadow.
void Document::painted(Node& one, Spot origin, Area clip, Area part) {
    if (!one.placed_) return;

    const Area box{{origin.x + one.area_.at.x, origin.y + one.area_.at.y}, one.area_.size};
    const Area seen = box.common(clip).common(part);

    if (seen.size.wide > 0 && seen.size.high > 0) drawn(one, box, seen, clip, part);

    if (!one.style_.shadow) return;

    const Area shown = clip.common(part);
    Canvas over = surface_.canvas(shown);
    const std::vector<std::vector<std::string>> shade{{"shade"}};
    const Look dark = sheet_.style(Selection{shade, {}, {}}, display_.colors).look;

    for (const Area cast : damage::shadowed(box)) over.shade(Area{{cast.at.x - shown.at.x, cast.at.y - shown.at.y}, cast.size}, dark);
}

// Paints `one`, standing at `box` on the screen, where it shows (`seen`): its background, border
// and outline, its content, then its children.
void Document::drawn(Node& one, Area box, Area seen, Area clip, Area part) {
    Canvas whole = surface_.canvas(seen).within(Area{{box.at.x - seen.at.x, box.at.y - seen.at.y}, box.size});
    const Style& style = one.style_;
    const Area all{{0, 0}, box.size};
    const int line = style.border == Border::none ? 0 : 1;

    whole.fill(all, style.look);

    if (line != 0) whole.frame(all, sheet_.lines(style.border, display_.unicode), style.look);

    one.outline(whole);

    Canvas inside = whole.within(all.inner(Sides{style.padding.top + line, style.padding.right + line,
                                                 style.padding.bottom + line, style.padding.left + line}));

    one.paint(inside);

    ++tally_.painted;

    for (Node& child : one.children_) painted(child, box.at, box.common(clip), part);
}

// The caret of the node with the focus, on the screen: where the node says it stands inside its
// content, as long as that lies inside the content and on the screen. Nothing without a focus, a
// placed focus or a caret, nor for a focus the confinement leaves outside; the document itself has
// no caret of its own.
std::optional<Caret> Document::caret() const {
    if (focus_ == nullptr || focus_ == this || (confined_ != nullptr && !confined_->holds(*focus_))) return std::nullopt;

    std::optional<Caret> made = focus_->caret();
    const std::optional<Area> where = focus_->absolute();

    if (!made || !where) return std::nullopt;

    const Style& style = focus_->style_;
    const int line = style.border == Border::none ? 0 : 1;
    const Area content = Area{where->at, where->size}.inner(Sides{style.padding.top + line, style.padding.right + line,
                                                                   style.padding.bottom + line, style.padding.left + line});
    const Spot at{content.at.x + made->at.x, content.at.y + made->at.y};

    if (!content.contains(at) || !Area{{0, 0}, display_.screen}.contains(at)) return std::nullopt;

    made->at = at;

    return made;
}

// Starts the timer `what`, to ring once `wait` from now by the document's clock (`ring`). The
// handle it returns owns it: gone, it never rings. `what` gets the handle and may set it `again`.
Timer Document::after(std::chrono::milliseconds wait, Fn<Timer> what) {
    auto made = std::make_shared<Timer::State>();

    made->what = std::move(what);
    made->clock = time_.shared();
    made->when = time_->now() + wait;
    made->order = ++timed_;

    timers_.push_back(made);

    return Timer{std::move(made)};
}

// Delivers the key `one` to the node with the focus, or to the document when none has it; while a
// drop is checked it waits for it (`waited`).
void Document::key(Key one) {
    if (waited([this, one] { key(one); })) return;

    changing();

    KeyEvent made{one};

    delivered(made, keyed());
}

// Delivers what the mouse did at `screen` (for the wheel, `steps` along `axis`): to the node that
// captured it, or else to the deepest active node under the pointer, which with the nodes above it
// is hovered. A press ends any capture before it is aimed and a release after it is delivered. A
// release where the press was, with the same button, adds a click, or a double click when it is the
// second one there within the double-click time; the back and forward buttons make no clicks.
// While a drop is checked it waits for it (`waited`), and comes after it at the moment it came.
void Document::mouse(Press press, MouseButton button, Spot screen, Mods mods, int steps, Axis axis) {
    const auto now = time_->now();

    if (waited([=, this] { moused(press, button, screen, mods, steps, axis, now); })) return;

    moused(press, button, screen, mods, steps, axis, now);
}

// What the mouse did at `now` by the document's clock (see `mouse`).
void Document::moused(Press press, MouseButton button, Spot screen, Mods mods, int steps, Axis axis, std::chrono::steady_clock::time_point now) {
    changing();

    if (press == Press::down) captured_ = nullptr;

    Node& found = under(screen);

    // While the focus is confined, the mouse outside goes to the confinement itself, as to a
    // curtain over everything else: nothing outside is hovered, clicked or captured. A capture lies
    // inside, since confining ends one outside. A press on the curtain that takes the curtain away
    // (a menu closing) makes no click: the click would land on what was beneath.
    Node& below = confined_ != nullptr && !confined_->holds(found) ? *confined_ : found;
    const std::shared_ptr<Node> curtain = press == Press::down && &below != &found ? below.shared_from_this() : nullptr;

    hovered(&below);

    Node& aimed = captured_ != nullptr ? *captured_ : below;
    MouseEvent made{press, button, mods, screen, steps, axis};

    if (const std::optional<Area> where = aimed.absolute()) made.at = Spot{screen.x - where->at.x, screen.y - where->at.y};

    delivered(made, aimed);

    if (press == Press::up) captured_ = nullptr;

    if (press == Press::down || press == Press::up) clicked(press, button, screen, mods, now);

    if (curtain != nullptr && confined_ != curtain.get()) pressing_ = MouseButton::none;
}

// Delivers pasted text to the node with the focus, or to the document; text that names files
// (`locating::dropped`, what a terminal pastes when files are dropped on it) is a drop when they
// are all there. Whether they are is asked of the files on the loop of the application that shows
// the document, or on the program's loop, and what comes meanwhile waits for the answer
// (`waited`). Without a loop to ask on, the text is a paste.
void Document::paste(std::string text) {
    if (waited([this, text]() mutable { paste(std::move(text)); })) return;

    std::optional<std::vector<Path>> paths = locating::dropped(text);
    Executor* home = home_;

    if (paths && home == nullptr) {
        try {
            home = &loop();
        } catch (const Trouble&) {
            paths.reset();
        }
    }

    if (!paths) {
        pasted(std::move(text));

        return;
    }

    checking_ = true;
    dropped_ = text;
    ticket_ = std::make_shared<int>(0);
    drops_.emplace(*home);
    drops_->run(dropping(std::move(*paths), std::move(text), ticket_));
}

// Keeps `again` to come after the drop being checked, and after what waits already, and says
// whether it kept it. With no check running, what waits goes first, and `again` with it; while
// that goes (`releasing_`) and no check runs, each comes at once.
bool Document::waited(Work again) {
    if (!checking_ && (releasing_ || held_.empty())) return false;

    held_.push_back(std::move(again));

    if (!checking_) released();

    return true;
}

// Delivers the text as a paste, to the node with the focus, or to the document.
void Document::pasted(std::string text) {
    changing();

    PasteEvent made{std::move(text)};

    delivered(made, keyed());
}

// Asks whether every one of `paths` is there, for at most `checked_within`, and hands the answer
// back on the loop it was started on as work of its own (`landed`), so that no handler runs inside
// this task and a handler may even destroy the document; the work does nothing when the document
// is gone or gave the check up (`ticket`). Any failure of the files is an answer: no.
Task<void> Document::dropping(std::vector<Path> paths, std::string text, std::weak_ptr<int> ticket) {
    Executor& back = co_await detail::Here{};
    bool there = false;

    try {
        there = co_await within(checked_within, present(paths));
    } catch (const Cancelled&) {
        throw;
    } catch (...) {
        there = false;
    }

    co_await to(back);

    back.post([this, ticket = std::move(ticket), there, paths = std::move(paths), text = std::move(text)]() mutable {
        if (ticket.expired()) return;

        landed(there, std::move(paths), std::move(text));
    });
}

// Delivers the drop to the node with the focus when the files are `there`, or the text as a paste
// when one is missing, the files did not answer in time, or no node took the drop; then lets what
// waited go. The delivery goes first among what waited, so that what its handlers do comes at once.
void Document::landed(bool there, std::vector<Path> paths, std::string text) {
    checking_ = false;
    dropped_.clear();
    ticket_.reset();

    held_.emplace_front([this, there, paths = std::move(paths), text = std::move(text)]() mutable {
        if (there) {
            changing();

            DropEvent made{std::move(paths), text};

            delivered(made, keyed());

            if (made.taken()) return;
        }

        pasted(std::move(text));
    });

    released();
}

// Does what waited for a drop, in its order, until it is done or one of it starts another check,
// after which the rest waits again. A handler that throws leaves the rest waiting for the next
// input, which lets it go first.
void Document::released() {
    // The mark of the release, taken away however it ends.
    struct Releasing {
        bool& on;

        // Marks the release.
        explicit Releasing(bool& given) : on(given) { on = true; }

        // Ends it.
        ~Releasing() { on = false; }

        Releasing(const Releasing&) = delete;
        Releasing& operator=(const Releasing&) = delete;
    };

    if (releasing_) return;

    const Releasing marked{releasing_};

    while (!checking_ && !held_.empty()) {
        Work next = std::move(held_.front());

        held_.pop_front();
        next();
    }
}

// Takes `home` as the loop the document's drops are checked on: the application's that shows it,
// or none. A check running on another loop is given up, and its text waits to come first as a
// paste, so that a loop that goes leaves no check behind.
void Document::homed(Executor* home) {
    if (home == home_) return;

    home_ = home;

    if (!checking_) return;

    drops_.reset();
    ticket_.reset();
    checking_ = false;
    held_.emplace_front([this, text = std::exchange(dropped_, {})]() mutable { pasted(std::move(text)); });
}

// Tells the document that the terminal's window gained the focus or lost it (`WindowFocused`,
// `WindowBlurred`), at the document itself: the window is the whole document's, not a node's.
// While a drop is checked it waits for it (`waited`).
void Document::window(bool focused) {
    if (waited([this, focused] { window(focused); })) return;

    if (focused) {
        WindowFocused made;

        delivered(made, *this);
    } else {
        WindowBlurred made;

        delivered(made, *this);
    }
}

// Opens `address` with the program the system has for it, through the document's port of processes,
// in the document's scope on the program's loop; a failure, no loop running among them, tells
// `Unfollowed` at the document. A node tells `Followed` first and comes here when nobody took it.
void Document::follow(std::string address) {
    if (address.empty()) return;

    if (!scope_) {
        try {
            scope_.emplace();
        } catch (const Trouble& why) {
            Unfollowed one{address, why};

            notified(one);

            return;
        }
    }

    scope_->run(opened(std::move(address)));
}

// The opening itself, and what failed of it.
Task<void> Document::opened(std::string address) {
    Command what;

    what.arguments = {address};
    what.mode = Mode::opened;

    std::optional<Trouble> failed;

    try {
        (void)co_await processes_->start(std::move(what));
    } catch (const Trouble& why) {
        failed = why;
    } catch (const std::exception& why) {
        failed = Trouble{Kind::other, why.what(), address};
    }

    if (!failed) co_return;

    Unfollowed one{address, *failed};

    notified(one);
}

// The view of the tree: a window over the uppermost layers (the last children are looked at first,
// as they lie on top), its text the node with the focus on top and then the whole tree
// (`described`), modal, so that the person scrolls it and leaves it with Escape. Without layers
// there is nowhere to open it.
void Document::inspected() {
    Layers* layers = nullptr;
    std::vector<Node*> waiting{this};

    while (layers == nullptr && !waiting.empty()) {
        Node* one = waiting.back();

        waiting.pop_back();
        layers = dynamic_cast<Layers*>(one);

        for (Node& child : one->children()) waiting.push_back(&child);
    }

    if (layers == nullptr) return;

    std::string said = focus_ != nullptr ? focus_->described() : std::string{};

    said = said.substr(0, said.find('\n')) + "\n\n" + described();

    auto window = std::make_shared<Window>(std::string{}, Modal::yes);

    window->id("inspector");
    window->full = true;
    window->children().add<Pager>(std::make_shared<Text>(said));
    (void)layers->open(window);
}

// When the next timer rings, or nothing when none waits.
std::optional<std::chrono::steady_clock::time_point> Document::alarm() const {
    std::optional<std::chrono::steady_clock::time_point> made;

    for (const std::weak_ptr<Timer::State>& held : timers_)
        if (const std::shared_ptr<Timer::State> one = held.lock(); one && one->waiting && one->handle != nullptr)
            if (!made || one->when < *made) made = one->when;

    return made;
}

// Sets the double-click time: the longest wait between two clicks that still make a double click,
// 400 ms until set; a negative time counts as none.
void Document::double_click(std::chrono::milliseconds most) { double_click_ = std::max(most, std::chrono::milliseconds{0}); }

// Rings the timers that are due by the clock, the earliest first and those due together in the
// order they were made. A timer set again while ringing waits for the next call.
void Document::ring() {
    changing();

    const auto now = time_->now();
    std::vector<std::shared_ptr<Timer::State>> due;

    std::erase_if(timers_, [](const std::weak_ptr<Timer::State>& one) { return one.expired(); });

    for (const std::weak_ptr<Timer::State>& held : timers_)
        if (std::shared_ptr<Timer::State> one = held.lock(); one && one->waiting && one->handle != nullptr && one->when <= now)
            due.push_back(std::move(one));

    std::sort(due.begin(), due.end(), [](const auto& one, const auto& two) {
        return std::tie(one->when, one->order) < std::tie(two->when, two->order);
    });

    for (const std::shared_ptr<Timer::State>& one : due) {
        if (!one->waiting || one->handle == nullptr) continue;

        one->waiting = false;
        one->what(*one->handle);
    }
}

// Delivers `one` to `target`: down from the root through capturing handlers, then up from the
// target through bubbling ones, then, when nobody took it, the default actions from the target
// up. Taking the event stops it wherever it is. The way is fixed before the first handler runs and
// its nodes are kept alive, so handlers may change the tree meanwhile.
void Document::delivered(Event& one, Node& target) {
    std::vector<Node*> way;
    std::vector<std::shared_ptr<Node>> kept;

    for (Node* at = &target; at != nullptr; at = at->parent_) {
        way.push_back(at);

        if (at != this) kept.push_back(at->shared_from_this());
    }

    one.target_ = &target;

    for (auto at = way.rbegin(); at != way.rend(); ++at)
        if ((*at)->called(one, false, Phase::capture)) return;

    for (Node* at : way)
        if (at->called(one, false, Phase::bubble)) return;

    for (Node* at : way)
        if (at->called(one, true, Phase::bubble)) return;
}

// The deepest node under `screen` that may take events: down from the root into the last placed
// child whose border holds the spot, since the later children lie on top and a child shows only
// inside its parent; then out of any inactive part (`reachable`).
Node& Document::under(Spot screen) {
    Node* at = this;
    Spot origin{area_.at};

    while (true) {
        Node* next = nullptr;

        for (auto child = at->children_.held_.rbegin(); child != at->children_.held_.rend(); ++child) {
            Node& one = **child;
            const Area box{{origin.x + one.area_.at.x, origin.y + one.area_.at.y}, one.area_.size};

            if (one.placed_ && box.contains(screen)) {
                next = &one;
                origin = box.at;

                break;
            }
        }

        if (next == nullptr) return reachable(*at);

        at = next;
    }
}

// `one`, or when it or a node above it is inactive, the parent of the topmost inactive node.
Node& Document::reachable(Node& one) {
    Node* made = &one;

    for (Node* at = &one; at != nullptr; at = at->parent_)
        if (!at->enabled_ && at->parent_ != nullptr) made = at->parent_;

    return *made;
}

// Confines the focus and the mouse to `within` and what lies under it, or frees them (nothing): Tab
// goes round inside, the mouse outside goes to `within` itself, as to a curtain, and keys go to
// `within` while nothing in it has the focus. The focus itself does not move here; whoever confines
// it decides where it goes. A node outside that asks for the focus meanwhile waits (`Node::focus`)
// and gets it once the confinement lets it in, the last one to ask winning. A node outside the
// document throws std::logic_error.
void Document::confine(Node* within) {
    changing();

    if (within != nullptr && within->document_ != this) throw std::logic_error{"tal::Document::confine: the node is not in this document"};

    confined_ = within;

    if (confined_ != nullptr && captured_ != nullptr && !confined_->holds(*captured_)) captured_ = nullptr;

    const std::shared_ptr<Node> waiting = deferred_.lock();

    if (waiting == nullptr || waiting->document_ != this) {
        deferred_.reset();

        return;
    }

    if (confined_ != nullptr && !confined_->holds(*waiting)) return;

    deferred_.reset();

    if (waiting->focusable()) moved(waiting.get());
}

// Where keys and pastes go: the node with the focus, unless it lies outside the confinement; then
// the node the focus is confined to; without either, the document.
Node& Document::keyed() {
    if (focus_ != nullptr && (confined_ == nullptr || confined_->holds(*focus_))) return *focus_;

    return confined_ != nullptr ? *confined_ : *this;
}

// Moves the focus to `to` (nothing for none): `Blurred` to the node that had it, `Focused` to the
// new one, and the state `focus` with it.
void Document::moved(Node* to) {
    if (to == focus_) return;

    Node* was = focus_;

    focus_ = to;

    if (was != nullptr) {
        was->state(State::focus, false);

        Blurred told;

        was->notified(told);
    }

    if (to != nullptr) {
        to->state(State::focus, true);

        Focused told;

        to->notified(told);
    }
}

// The node that takes the focus after `from` (before it, when `back`) in the order of the tree,
// round the end: the next that can take it (`focusable`) and does not lie in `skipped`. From
// nowhere it is the first (the last). It may be `from` itself when nothing else can take it, and
// nothing when nothing can.
Node* Document::following(const Node* from, bool back, const Node* skipped) {
    std::vector<Node*> order;
    std::vector<Node*> waiting{this};

    while (!waiting.empty()) {
        Node* one = waiting.back();

        waiting.pop_back();
        order.push_back(one);

        for (auto at = one->children_.held_.rbegin(); at != one->children_.held_.rend(); ++at) waiting.push_back(at->get());
    }

    if (back) std::reverse(order.begin(), order.end());

    const auto start = std::find(order.begin(), order.end(), from);
    const std::size_t first = start == order.end() ? 0 : static_cast<std::size_t>(start - order.begin()) + 1;

    for (std::size_t step = 0; step < order.size(); ++step) {
        Node* one = order[(first + step) % order.size()];

        if (one->focusable() && (skipped == nullptr || !skipped->holds(*one)) && (confined_ == nullptr || confined_->holds(*one)))
            return one;
    }

    return nullptr;
}

// Lets go of `gone`, which leaves the tree: a focus inside moves on as Tab moves it, a capture
// inside ends, and the nodes inside are no longer hovered.
void Document::forgotten(Node& gone) {
    if (confined_ != nullptr && gone.holds(*confined_)) confined_ = nullptr;

    if (focus_ != nullptr && gone.holds(*focus_)) moved(following(focus_, false, &gone));

    if (captured_ != nullptr && gone.holds(*captured_)) captured_ = nullptr;

    std::vector<Node*> kept;

    for (Node* one : hover_) {
        if (gone.holds(*one)) {
            one->state(State::hover, false);
        } else {
            kept.push_back(one);
        }
    }

    hover_ = std::move(kept);
}

// Makes `now` and the nodes above it, short of the document, the hovered ones, all but those that
// take no hover (`Node::hoverable`): the state `hover` goes from the nodes that leave the set and
// comes to those that join it.
void Document::hovered(Node* now) {
    std::vector<Node*> made;

    for (Node* at = now; at != nullptr && at != this; at = at->parent_)
        if (at->hoverable_) made.push_back(at);

    for (Node* one : hover_)
        if (std::find(made.begin(), made.end(), one) == made.end()) one->state(State::hover, false);

    for (Node* one : made)
        if (std::find(hover_.begin(), hover_.end(), one) == hover_.end()) one->state(State::hover, true);

    hover_ = std::move(made);
}

// Turns a press and a release in the same cell with the same button into a click for the node under
// the pointer (the confinement for one outside it, as `mouse` does), or a double click when a click
// with that button in that cell came no longer than the double-click time before by the document's
// clock, the press and the release taken at `now`. A release elsewhere is a drag, not a click.
void Document::clicked(Press press, MouseButton button, Spot screen, Mods mods, std::chrono::steady_clock::time_point now) {
    if (button == MouseButton::back || button == MouseButton::forward) return;

    if (press == Press::down) {
        began_ = screen;
        pressing_ = button;

        return;
    }

    const MouseButton which = pressing_;

    pressing_ = MouseButton::none;

    if (which == MouseButton::none || which != button || began_ != screen) return;

    const bool twice = first_click_ && clicked_ == button && waiting_ == screen && now - *first_click_ <= double_click_;

    if (twice) {
        first_click_.reset();
    } else {
        first_click_ = now;
        waiting_ = screen;
        clicked_ = button;
    }

    Node& found = under(screen);
    Node& aimed = confined_ != nullptr && !confined_->holds(found) ? *confined_ : found;
    MouseEvent made{twice ? Press::double_click : Press::click, button, mods, screen, 0};

    if (const std::optional<Area> where = aimed.absolute()) made.at = Spot{screen.x - where->at.x, screen.y - where->at.y};

    delivered(made, aimed);
}

}
