#include "tal/webview.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "tal/controls.hpp"
#include "tal/fs.hpp"
#include "tal/key.hpp"
#include "tal/leaves.hpp"
#include "tal/text.hpp"
#include "tal/trouble.hpp"

#include "locating.hpp"

// A page of the web in the tree (E6e): a view of a browser (an engine in a process of its own)
// shown as an `Image` child of its pictures, a pixel of the view for a pixel of a cell, as `Leaves`
// shows pages. A click in a cell clicks the middle of its pixels, the wheel scrolls three rows of
// cells a step, and keys go to the page while the node has the focus: letters and pasted text as
// typed, the named keys as keys, Alt with Left and Right back and forward; what holds Ctrl is the
// program's. What the view tells of itself comes as `Visited` (a new address or title) and
// `Refused` (something it would not do, as a link to the web with the network off).
//
// A new picture is asked after every input, every change the view tells, every new size (on the
// document's clock at once, outside layout), every 250 ms while the page loads and every second
// while its scripts run; one at a time, those asked meanwhile taken as one. Where the terminal
// shows no pixels, or no engine opens the page, the node shows the text version of a local page
// (E6f) under the reason; a page on the web then shows the reason alone. A trouble of the engine
// shows in place of the page, and the next input opens the page anew.
namespace tal {

namespace {

constexpr std::array<double, 6> scales{1.0, 1.25, 1.5, 2.0, 3.0, 4.0};
constexpr int wheeled = 3;
constexpr std::chrono::milliseconds loading_every{250};
constexpr std::chrono::milliseconds scripts_every{1000};
constexpr std::uint64_t most_page = std::uint64_t{16} << 20;

// Whether the terminal shows pixels: sixel or kitty, with the size of a cell known.
bool pictures(const Display& shown) { return shown.graphics != Graphics::none && shown.cell.wide > 0 && shown.cell.high > 0; }

// The local file a file address names (on Windows file:///C:/x is C:/x), or none.
std::optional<Path> local_file(std::string_view address) {
    std::optional<Path> made = locating::unlocated(address);

#ifdef _WIN32
    const std::string_view stored = made ? made->stored() : std::string_view{};

    if (stored.size() >= 3 && stored[0] == '/' && stored[2] == ':') made = Path::restored(stored.substr(1));
#endif

    return made;
}

// The name of the named key `code` that a page takes, or none.
bool page_key(Code code) {
    switch (code) {
        case Code::enter:
        case Code::tab:
        case Code::backspace:
        case Code::remove:
        case Code::escape:
        case Code::up:
        case Code::down:
        case Code::left:
        case Code::right:
        case Code::home:
        case Code::end:
        case Code::pageup:
        case Code::pagedown: return true;
        default: return false;
    }
}

}

// The page at `address` of `browser`, with scripts and the network as `how` allows, in the class
// "webview". Throws `std::invalid_argument` for no browser.
Webview::Webview(std::shared_ptr<docs::Browser> browser, std::string address, docs::Browsing how)
    : browser_(std::move(browser)), address_(std::move(address)), how_(how) {
    if (browser_ == nullptr) throw std::invalid_argument{"tal::Webview: no browser"};

    classes({"webview"});
    focusable(true);

    hooks_.add(on<Changed>([this](Changed& what) {
        if (!what.of(zoom)) return;

        if (*zoom >= scales.size()) {
            zoom = scales.size() - 1;

            return;
        }

        if (view_) act(acted(view_, view_->zoomed(scales[*zoom])));
    }));

    hooks_.add(otherwise<KeyEvent>([this](KeyEvent& what) {
        const Key key = what.key;
        const Mods mods = key.mods();

        if (mods.ctrl) return;

        if (mods.alt && (key.code() == Code::left || key.code() == Code::right)) {
            step(key.code() == Code::left ? docs::Step::back : docs::Step::forward);
        } else if (mods.alt || (key.letter() == 0 && !page_key(key.code()))) {
            return;
        } else if (key.letter() != 0) {
            std::string typed;

            text::encoded(typed, key.letter());

            if (view_) act(acted(view_, view_->typed(std::move(typed))));
            else act(opened());
        } else {
            if (view_) act(acted(view_, view_->keyed(key)));
            else act(opened());
        }

        what.take();
    }));

    hooks_.add(otherwise<PasteEvent>([this](PasteEvent& what) {
        if (mode_ != Mode::pictures) return;

        if (view_) act(acted(view_, view_->typed(text::repaired(what.text))));
        else act(opened());

        what.take();
    }));

    hooks_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        if (mode_ != Mode::pictures) return;

        const Size cell = display().cell;
        const Spot at{what.at.x - inside_.at.x, what.at.y - inside_.at.y};

        if (at.x < 0 || at.y < 0 || at.x >= inside_.size.wide || at.y >= inside_.size.high) return;

        if (what.press == Press::click && what.button == MouseButton::left) {
            focus();

            if (view_) act(acted(view_, view_->clicked((at.x + 0.5) * cell.wide, (at.y + 0.5) * cell.high)));
            else act(opened());
        } else if (what.press == Press::wheel) {
            const double by = -static_cast<double>(wheeled) * what.steps * cell.high;

            if (view_) act(acted(view_, what.axis == Axis::vertical ? view_->wheeled(0, by) : view_->wheeled(by, 0)));
        } else {
            return;
        }

        what.take();
    }));
}

// Its work ends with it: the scope cancels what it waits for, and the view is closed.
Webview::~Webview() = default;

// How many steps of zoom there are: 1, 1.25, 1.5, 2, 3 and 4 times.
std::size_t Webview::zooms() { return scales.size(); }

// Whether anything asked of the view is being done (its changes, always awaited, aside).
bool Webview::busy() const { return scope_.has_value() && scope_->busy(); }

// The view taken to `address`; the page opened anew there when it has no view.
void Webview::go(std::string address) {
    if (view_) {
        act(acted(view_, view_->went(std::move(address))));

        return;
    }

    address_ = std::move(address);
    act(opened());
}

// The view taken back, forward, loaded again or stopped.
void Webview::step(docs::Step step) {
    if (view_) act(acted(view_, view_->stepped(step)));
}

// The whole room it is given.
Size Webview::measure(Limits room) const { return room.least; }

// The child laid out in the room; the view brought to the room's pixels, or the text version
// taken where the terminal shows none, after the layout.
void Webview::arrange(Area inside) {
    inside_ = inside;

    if (shown_) shown_->placed(inside);

    const Mode wanted = pictures(display()) ? Mode::pictures : Mode::text;

    if (wanted != mode_ || (wanted == Mode::pictures && pixels() != size_)) scheduled();
}

// The pixels of the room: its cells times the pixels of a cell.
media::Size Webview::pixels() const {
    const Size cell = display().cell;

    return media::Size{static_cast<std::uint32_t>(std::max(inside_.size.wide, 0) * std::max(cell.wide, 0)),
                       static_cast<std::uint32_t>(std::max(inside_.size.high, 0) * std::max(cell.high, 0))};
}

// A refresh appointed at once on the document's clock, unless one is appointed already.
void Webview::scheduled() {
    if (refresh_.waiting() || document() == nullptr) return;

    refresh_ = document()->after(std::chrono::milliseconds{0}, [this](Timer&) { refreshed(); });
}

// What the node shows brought to the terminal and the room: the text version where there are no
// pixels; else the page opened, or its view brought to the room's size.
void Webview::refreshed() {
    const Mode wanted = pictures(display()) ? Mode::pictures : Mode::text;

    if (wanted != mode_) {
        mode_ = wanted;

        if (mode_ == Mode::text) {
            view_.reset();
            act(read_text("The terminal shows no pictures."));

            return;
        }

        broken_ = false;
    }

    if (mode_ != Mode::pictures || broken_) return;

    const media::Size wanted_size = pixels();

    if (wanted_size.width == 0 || wanted_size.height == 0) return;

    if (!view_) {
        act(opened());
    } else if (wanted_size != size_) {
        act(resized(view_, wanted_size));
    }
}

// `child` shown in place of what was, filling the room.
void Webview::shown(std::shared_ptr<Node> child) {
    if (shown_ == child) return;

    if (shown_) (void)children().remove(*shown_);

    shown_ = std::move(child);

    if (shown_) children().add(shown_);

    relayout();
}

// `work` run in the node's scope, on the program's loop.
void Webview::act(Task<void> work) {
    if (!scope_) scope_.emplace();

    scope_->run(std::move(work));
}

// A picture of the view asked for, or once more after the one being taken.
void Webview::shoot() {
    if (!view_) return;

    if (shooting_) {
        again_ = true;

        return;
    }

    shooting_ = true;
    act(shot(view_));
}

// The next picture appointed while the page loads (every 250 ms) or its scripts run (every second).
void Webview::ticking() {
    if (tick_.waiting() || document() == nullptr || !view_) return;

    if (!loading_ && !how_.scripts) return;

    tick_ = document()->after(loading_ ? loading_every : scripts_every, [this](Timer&) {
        shoot();
        ticking();
    });
}

// The page opened in a view of the room's size, its changes watched and its first picture asked.
// When no engine opens it (`Trouble` unsupported), the text version of a local page under the
// reason, or the reason alone; any other trouble in place of the page.
Task<void> Webview::opened() {
    if (opening_ || mode_ != Mode::pictures) co_return;

    const media::Size size = pixels();

    if (size.width == 0 || size.height == 0) co_return;

    const std::weak_ptr<Node> alive = weak_from_this();
    Executor& here = co_await detail::Here{};

    opening_ = true;
    broken_ = false;

    std::shared_ptr<docs::Browsed> made;
    std::optional<Trouble> refused;

    try {
        made = co_await browser_->opened(address_, how_, size);
    } catch (const Trouble& why) {
        refused = why;
    }

    co_await to(here);

    if (alive.expired()) co_return;

    opening_ = false;

    if (refused && refused->kind() == Kind::unsupported) {
        co_await read_text(refused->what());
        co_return;
    }

    if (refused) {
        failed(refused->what());
        co_return;
    }

    view_ = made;
    size_ = size;
    seen_ = 0;
    skipped_ = 0;

    if (!image_) image_ = std::make_shared<Image>();

    shown(image_);

    if (!watching_) watching_.emplace();

    watching_->run(watched(made));

    if (*zoom > 0) act(acted(made, made->zoomed(scales[*zoom])));

    shoot();
    ticking();
}

// The text version of the page under `why`, when it is a local file; else `why` alone.
Task<void> Webview::read_text(std::string why) {
    if (reading_) co_return;

    const std::weak_ptr<Node> alive = weak_from_this();
    Executor& here = co_await detail::Here{};
    const std::optional<Path> path = local_file(address_);

    if (!path) {
        failed(why + " The page is not a local file: " + address_);
        co_return;
    }

    reading_ = true;

    Bytes page;
    std::optional<Trouble> unread;

    try {
        page = co_await fs::read(*path, fs::Range{0, most_page + 1});
    } catch (const Trouble& trouble) {
        unread = trouble;
    }

    co_await to(here);

    if (alive.expired()) co_return;

    reading_ = false;

    if (unread) {
        failed(why + " " + unread->what());
        co_return;
    }

    docs::Plain plain;
    std::optional<Trouble> unwritten;

    try {
        plain = docs::plain(page, address_);
    } catch (const Trouble& trouble) {
        unwritten = trouble;
    }

    if (unwritten) {
        failed(why + " " + unwritten->what());
        co_return;
    }

    if (!plain.title.empty() && plain.title != title_) {
        title_ = plain.title;

        Visited told{address_, title_};

        notified(told);
    }

    shown(transcript(plain, std::move(why)));
}

// The changes of `view` as it tells them: a new address or title told as `Visited`, each refusal
// as `Refused`, a picture asked, the next pictures appointed while the page loads; a view lost
// shown as a trouble, so that the next input opens the page anew. Ends when the view does.
Task<void> Webview::watched(std::shared_ptr<docs::Browsed> view) {
    const std::weak_ptr<Node> alive = weak_from_this();
    Executor& here = co_await detail::Here{};

    for (;;) {
        docs::Change change;
        bool ended = false;

        try {
            change = co_await view->changed(seen_);
        } catch (const Trouble&) {
            ended = true;
        }

        co_await to(here);

        if (alive.expired() || view_ != view) co_return;

        if (ended) {
            failed("The page is gone: its engine ended.");
            co_return;
        }

        seen_ = change.sequence;

        if (change.address != address_ || change.title != title_) {
            address_ = change.address;
            title_ = change.title;

            Visited told{address_, title_};

            notified(told);
        }

        for (docs::Refusal& one : change.refusals) {
            Refused told{std::move(one)};

            notified(told);
        }

        skipped_ = change.skipped;

        if (change.lost) {
            failed("The page was lost: the process that showed it ended.");
            co_return;
        }

        if (loading_ && !change.loading) tick_ = Timer{};

        loading_ = change.loading;
        shoot();
        ticking();
    }
}

// A picture of `view` taken and shown; another after it when one was asked meanwhile. A trouble
// in place of the page.
Task<void> Webview::shot(std::shared_ptr<docs::Browsed> view) {
    const std::weak_ptr<Node> alive = weak_from_this();
    Executor& here = co_await detail::Here{};
    docs::Shot made;
    std::optional<Trouble> refused;

    try {
        made = co_await view->shot();
    } catch (const Trouble& why) {
        refused = why;
    }

    co_await to(here);

    if (alive.expired()) co_return;

    shooting_ = false;

    if (view_ != view) co_return;

    if (refused) {
        failed(refused->what());
        co_return;
    }

    if (made.pixels.size() == size_ && image_) {
        image_->hold(std::move(made.pixels));
        relayout();
    }

    if (std::exchange(again_, false)) shoot();
}

// `view` brought to `size`, then a picture of it asked.
Task<void> Webview::resized(std::shared_ptr<docs::Browsed> view, media::Size size) {
    co_await acted(view, view->resized(size));

    if (view_ == view) {
        size_ = size;
        shoot();
    }
}

// `work` done on `view`, then a picture asked; a trouble in place of the page.
Task<void> Webview::acted(std::shared_ptr<docs::Browsed> view, Task<void> work) {
    const std::weak_ptr<Node> alive = weak_from_this();
    Executor& here = co_await detail::Here{};
    std::optional<Trouble> refused;

    try {
        co_await std::move(work);
    } catch (const Trouble& why) {
        refused = why;
    }

    co_await to(here);

    if (alive.expired() || view_ != view) co_return;

    if (refused && refused->kind() != Kind::refused && refused->kind() != Kind::other) {
        failed(refused->what());
        co_return;
    }

    shoot();
}

// `why` shown in place of the page; the view let go, so that the next input opens the page anew.
void Webview::failed(std::string why) {
    view_.reset();
    broken_ = true;
    loading_ = false;
    tick_ = Timer{};

    auto said = std::make_shared<Label>(std::move(why));

    said->wrap = true;
    said->add_class("trouble");
    shown(said);
}

}
