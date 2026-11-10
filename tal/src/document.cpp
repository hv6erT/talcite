#include "tal/document.hpp"

#include <algorithm>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "damage.hpp"

namespace tal {

namespace {

constexpr std::size_t most_damaged = 64;

constexpr std::chrono::milliseconds double_click{400};

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
// The clock serves the timers that come with events.
// Its own default actions: Tab and Shift+Tab move the focus, a press of a pointing button gives it
// to the deepest node on the way that can take it.
Document::Document(Port<Clock> time) : time_(std::move(time)) {
    document_ = this;

    own_.add(otherwise<KeyEvent>([this](KeyEvent& one) {
        const Mods mods = one.key.mods();

        if (one.key.code() != Code::tab || mods.ctrl || mods.alt) return;

        Node* next = following(focus_, mods.shift, nullptr);

        if (next == nullptr) return;

        one.take();
        moved(next);
    }));

    own_.add(otherwise<MouseEvent>([this](MouseEvent& one) {
        if (one.press != Press::down || one.button == Button::back || one.button == Button::forward) return;

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
    damaged(Area{{0, 0}, screen_});
}

// Says what the document is shown on: the size of the screen in cells, its color level and
// whether it shows Unicode. A new size is a new surface, laid out and painted whole; a new level
// styles every node again; Unicode changes the glyphs, so everything is painted again.
void Document::display(Size screen, Colors colors, bool unicode) {
    if (screen == screen_ && colors == colors_ && unicode == unicode_) return;

    changing();

    if (screen != screen_) {
        screen_ = screen;
        surface_ = Surface{screen};

        relayout();
    }

    if (colors != colors_) {
        colors_ = colors;

        restyle();
    }

    unicode_ = unicode;

    damaged(Area{{0, 0}, screen_});
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
        (void)measured(Limits{screen_, screen_});
        placed(Area{{0, 0}, screen_});
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
    const Area screen{{0, 0}, screen_};

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
        const Style made = sheet_.style(Selection{classes, one.states_, {}}, colors_);

        ++tally_.styled;

        if (made.border != one.style_.border || made.padding != one.style_.padding || made.margin != one.style_.margin) {
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
// everything under it, since children show only inside their parent.
void Document::painted(Node& one, Spot origin, Area clip, Area part) {
    if (!one.placed_) return;

    const Area box{{origin.x + one.area_.at.x, origin.y + one.area_.at.y}, one.area_.size};
    const Area seen = box.common(clip).common(part);

    if (seen.size.wide <= 0 || seen.size.high <= 0) return;

    Canvas whole = surface_.canvas(seen).within(Area{{box.at.x - seen.at.x, box.at.y - seen.at.y}, box.size});
    const Style& style = one.style_;
    const Area all{{0, 0}, box.size};
    const int line = style.border == Border::none ? 0 : 1;

    whole.fill(all, style.look);

    if (line != 0) whole.frame(all, sheet_.lines(style.border, unicode_), style.look);

    Canvas inside = whole.within(all.inner(Sides{style.padding.top + line, style.padding.right + line,
                                                 style.padding.bottom + line, style.padding.left + line}));

    one.paint(inside);

    ++tally_.painted;

    for (Node& child : one.children_) painted(child, box.at, box.common(clip), part);
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

// Delivers the key `one` to the node with the focus, or to the document when none has it.
void Document::key(Key one) {
    changing();

    KeyEvent made{one};

    delivered(made, focus_ != nullptr ? *focus_ : *this);
}

// Delivers what the mouse did at `screen`: to the node that captured it, or else to the deepest
// active node under the pointer, which with the nodes above it is hovered. A press ends any
// capture before it is aimed and a release after it is delivered. A release where the press was,
// with the same button, adds a click, or a double click when it is the second one there within
// 400 ms; the back and forward buttons make no clicks.
void Document::mouse(Press press, Button button, Spot screen, Mods mods, int steps) {
    changing();

    if (press == Press::down) captured_ = nullptr;

    Node& below = under(screen);

    hovered(&below);

    Node& aimed = captured_ != nullptr ? *captured_ : below;
    MouseEvent made{press, button, mods, screen, steps};

    if (const std::optional<Area> where = aimed.absolute()) made.at = Spot{screen.x - where->at.x, screen.y - where->at.y};

    delivered(made, aimed);

    if (press == Press::up) captured_ = nullptr;

    if (press == Press::down || press == Press::up) clicked(press, button, screen, mods);
}

// Delivers pasted text to the node with the focus, or to the document.
void Document::paste(std::string text) {
    changing();

    PasteEvent made{std::move(text)};

    delivered(made, focus_ != nullptr ? *focus_ : *this);
}

// When the next timer rings, or nothing when none waits.
std::optional<std::chrono::steady_clock::time_point> Document::alarm() const {
    std::optional<std::chrono::steady_clock::time_point> made;

    for (const std::weak_ptr<Timer::State>& held : timers_)
        if (const std::shared_ptr<Timer::State> one = held.lock(); one && one->waiting && one->handle != nullptr)
            if (!made || one->when < *made) made = one->when;

    return made;
}

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

        if (one->focusable() && (skipped == nullptr || !skipped->holds(*one))) return one;
    }

    return nullptr;
}

// Lets go of `gone`, which leaves the tree: a focus inside moves on as Tab moves it, a capture
// inside ends, and the nodes inside are no longer hovered.
void Document::forgotten(Node& gone) {
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

// Makes `now` and the nodes above it, short of the document, the hovered ones: the state `hover`
// goes from the nodes that leave the set and comes to those that join it.
void Document::hovered(Node* now) {
    std::vector<Node*> made;

    for (Node* at = now; at != nullptr && at != this; at = at->parent_) made.push_back(at);

    for (Node* one : hover_)
        if (std::find(made.begin(), made.end(), one) == made.end()) one->state(State::hover, false);

    for (Node* one : made)
        if (std::find(hover_.begin(), hover_.end(), one) == hover_.end()) one->state(State::hover, true);

    hover_ = std::move(made);
}

// Turns a press and a release in the same cell with the same button into a click for the node
// under the pointer, or a double click when a click with that button in that cell came no more
// than 400 ms before by the document's clock. A release elsewhere is a drag, not a click.
void Document::clicked(Press press, Button button, Spot screen, Mods mods) {
    if (button == Button::back || button == Button::forward) return;

    if (press == Press::down) {
        began_ = screen;
        pressing_ = button;

        return;
    }

    const Button which = pressing_;

    pressing_ = Button::none;

    if (which == Button::none || which != button || began_ != screen) return;

    const auto now = time_->now();
    const bool twice = first_click_ && clicked_ == button && waiting_ == screen && now - *first_click_ <= double_click;

    if (twice) {
        first_click_.reset();
    } else {
        first_click_ = now;
        waiting_ = screen;
        clicked_ = button;
    }

    Node& aimed = under(screen);
    MouseEvent made{twice ? Press::double_click : Press::click, button, mods, screen, 0};

    if (const std::optional<Area> where = aimed.absolute()) made.at = Spot{screen.x - where->at.x, screen.y - where->at.y};

    delivered(made, aimed);
}

}
