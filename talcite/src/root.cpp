#include "talcite/root.hpp"

#include "talcite/float.hpp"
#include "talcite/modal.hpp"
#include "talcite/modals.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

#include "talcite/canvas.hpp"
#include "talcite/encoding.hpp"

namespace talcite::ui {

namespace {

// Whether every frame is painted whole as well and compared with what painting only the changes
// gave (`frames_checked`).
std::atomic<bool> checking{false};

// The most passes of the layout in one frame (`Root::steadied`): a change announced in a pass is
// taken in by the next, and a chain of more than a few is a node that never settles.
constexpr int settling_passes = 8;

// The rows [from, upto) cut to [0, high), sorted, with overlapping and adjoining ones merged.
std::vector<std::pair<int, int>> merged(std::vector<std::pair<int, int>> given, int high) {
    for (auto& [from, upto] : given) {
        from = std::clamp(from, 0, high);
        upto = std::clamp(upto, 0, high);
    }

    std::erase_if(given, [](const std::pair<int, int>& one) { return one.second <= one.first; });
    std::sort(given.begin(), given.end());

    std::vector<std::pair<int, int>> made;

    for (const auto& one : given) {
        if (!made.empty() && one.first <= made.back().second) made.back().second = std::max(made.back().second, one.second);
        else made.push_back(one);
    }

    return made;
}

// A cell as a trial reads it in a report: its letter and how it is shown.
std::string told(const Cell& one) {
    std::string made = "'" + (one.letter == 0 ? std::string{"(half)"} : spelled(one.letter)) + "'";

    made += one.shown == Shown::none ? " not drawn" : one.shown == Shown::shade ? " shade" : one.shown == Shown::foreground ? " foreground" : "";

    return made;
}

}

// Turns the check of every frame on or off: each frame is painted whole as well, on a sheet of its
// own, and a cell that differs from what painting only the changes left on the kept sheet throws
// `std::logic_error`, the place and the node under it named. So a change nobody announced
// (`Node::changed`) fails a trial instead of leaving a stale screen. The trials of talcite and twa
// turn it on; a program does not, since it costs a second frame.
void frames_checked(bool on) { checking = on; }

// The root reads the time for its schedule (`schedule`) from `time`: the real clock in a program,
// a manual one in a trial.
Root::Root(talcite::Clock& time) : schedule_(time) {}

Root::~Root() = default;

// Replaces the skin. Replacing the whole palette is the whole of changing the look on the fly. A
// skin cannot be changed after it is built, so there is no state in which half the roles come from
// the new one and half from the old; the next frame paints the whole screen anew. There is no walk
// over the tree. A skin comes with variants for all levels and at once
// adopts the root's facts: the level the root already knows passes to the new skin, not the other
// way around. Thanks to that, replacing the skin (also as a trial, in a settings window) does not
// require repeating the facts about the terminal.
void Root::skin(Skin given) {
    given.colors(skin_.colors());
    given.unicode(skin_.unicode());

    skin_ = std::move(given);

    repaint();
}

// Sets the terminal's color level. Colors and unicode are facts about the terminal, like `blocks`
// and `cell` below, set by whoever drives the tree. The root does not keep them separately: they
// live in the skin, because the skin uses them at every lookup, and a second place would sooner or
// later drift apart.
void Root::colors(Colors given) {
    skin_.colors(given);

    repaint();
}

// Sets whether the terminal shows unicode; like the colors, the fact lives in the skin.
void Root::unicode(bool given) {
    skin_.unicode(given);

    repaint();
}

// Sets which block characters the terminal can draw. The facts about the cell are set here by
// whoever drives the tree, usually `Term`, in `show()` and at every `reconfigure`. The tree does
// not know where they came from, so a test or a second rendering layer can set them just as well.
// Every widget may read them while painting, so the next frame paints the whole screen.
void Root::blocks(Blocks given) {
    blocks_ = given;

    repaint();
}

// Sets the size of one cell in pixels; zero means the terminal did not say.
void Root::cell(Size given) {
    cell_ = given;

    repaint();
}

// Sets which graphics protocol the terminal supports.
void Root::graphics(Graphics given) {
    graphics_ = given;

    repaint();
}

// The frame of the given size on the sheet the root keeps between frames. The whole tree is laid
// out every time: there a node finds what changed in what it reads (another node, the program's
// data, the pool's progress, the time) and says so, and a node that moved adds the places it left
// and took. Then only the rows that changed are cleared and painted, the tree cut to them: a node
// outside them paints nothing, and a frame in which nothing changed paints nothing at all. Rows
// rather than rectangles, because a rectangle could cut a double-width character in half, and a
// frame cut at the edges of the screen alone is the same as a whole one. A new size, or something
// every node reads (`repaint`), paints everything.
const View& Root::frame(Size size) {
    if (sheet_.size() != size) {
        sheet_ = View{size};
        whole_ = true;
    }

    steadied(size);

    if (whole_) damaged_.assign(1, {0, size.high});

    whole_ = false;

    if (!damaged_.empty()) {
        Canvas whole{sheet_};

        for (const auto& [from, upto] : merged(std::move(damaged_), size.high)) {
            const Area rows{{0, from}, {size.wide, upto - from}};

            sheet_.clear(rows);

            Canvas cut = whole.clipped(rows);

            paint(cut);
        }

        damaged_.clear();

        sheet_.forget();
    }

    if (checking) checked(size);

    return sheet_;
}

// The frame as a copy, for the trials, which compare frames as sheets: the root draws as `Term`
// draws it, painting only the changes, so the check of every frame (`frames_checked`) covers them.
View Root::render(const Area& where) { return frame(where.size); }

// Everything changed: something every node reads (the skin, the terminal's colors, Unicode, blocks,
// cell size or graphics), so the next frame paints the whole screen.
void Root::repaint() { whole_ = true; }

// Rows of the screen to paint at the next frame: those `screen` covers.
void Root::damage(const Area& screen) {
    if (screen.size.wide <= 0 || screen.size.high <= 0) return;

    damaged_.emplace_back(screen.at.y, screen.at.y + screen.size.high);

    if (damaged_.size() > 256) damaged_ = merged(std::move(damaged_), sheet_.size().high);
}

// A change a node announces (`Node::changed`). One announced in the middle of the layout may change
// what another node has already laid out from it: a status line composed from the position of the
// content laid out after it, a figure built from data that arrived meanwhile. So the layout goes on
// (`steadied`).
void Root::announced(const Node& who, const Area& screen) {
    damage(screen);

    if (arranging_) unsettled_ = &who;
}

// The tree laid out until a pass announces nothing (`announced`): every node then stands where
// what the others hold puts it, and paints what it laid out. A node announces a change in its
// layout only when something it shows is different from the last pass, so the second pass usually
// is the last. A tree that never settles is a node announcing a change every time, which the check
// of frames (`frames_checked`) reports; the program shows the last pass.
void Root::steadied(Size size) {
    for (int pass = 0;; ++pass) {
        unsettled_ = nullptr;
        arranging_ = true;

        try {
            arrange({{0, 0}, size});
        } catch (...) {
            arranging_ = false;

            throw;
        }

        arranging_ = false;

        if (unsettled_ == nullptr) return;

        if (pass + 1 < settling_passes) continue;

        if (checking) throw std::logic_error(std::string{"talcite::ui::Root: the layout does not settle, "} + typeid(*unsettled_).name() + " announces a change (Node::changed) in every pass");

        return;
    }
}

// The check of a frame (`frames_checked`): the tree as this frame laid it out, painted whole on a
// fresh sheet, must give what the kept sheet holds, cell by cell. The check does not lay the tree
// out again: what a layout reads may move meanwhile by itself (the pool's progress, the time), and
// the frame's layout has taken in whatever changed there. What painting alone reads must have been
// announced, and that is what the check finds.
void Root::checked(Size size) {
    View whole{size};

    whole.clear({{0, 0}, size});

    Canvas onto{whole};

    paint(onto);

    for (int y = 0; y < size.high; ++y)
        for (int x = 0; x < size.wide; ++x) {
            const Cell kept = sheet_.at({x, y});
            const Cell fresh = whole.at({x, y});

            if (kept == fresh) continue;

            std::string under_it;

            for (const Node* at = under({x, y}); at != nullptr && at != this; at = at->parent())
                under_it += (under_it.empty() ? "" : " in ") + std::string{typeid(*at).name()};

            throw std::logic_error("talcite::ui::Root: the cell (" + std::to_string(x) + ", " + std::to_string(y) +
                                   ") is " + told(kept) + " but " + told(fresh) + " when painted whole, under " +
                                   under_it + ": a change nobody announced (Node::changed)");
        }
}

// Moves the focus to `one` (null clears it) and sends `UnfocusEvent` and `FocusEvent`; throws
// `std::logic_error` for a node from another tree. The field is changed before both events are
// sent, not between them: a handler that asks focused() in reaction to losing the focus has to get
// the new answer, not one from the middle of the operation.
void Root::move_focus(Node* one) {
    if (one != nullptr && one->root() != this)
        throw std::logic_error("talcite::ui::Root: the node does not belong to this tree");

    // A veil covers the focus too. A request from a node under an open veil is not granted at
    // once: the veil records it as where the focus should return after it closes (`withheld`). We
    // go up from the node and ask the first veil window that covers it.
    if (one != nullptr)
        for (Node* each = one; each != nullptr; each = each->parent())
            if (auto* modals = dynamic_cast<Modals*>(each->parent()); modals != nullptr && modals->withheld(*one))
                return;

    // A call on a node that does not take the focus does nothing, and that is not an oversight. The
    // focus is also handed back to whoever had it before: that is how both the menu bar and a
    // dialog return to the application. That node may have been disabled or have left the tree in
    // the meantime, and an exception on this path would bring the program down for someone else's
    // state.
    if (one != nullptr && !one->focusable()) return;

    if (one == focus_) return;

    Node* was = focus_;

    focus_ = one;

    // What shows the focus looks different now: the node that lost it and the one that got it, and
    // every node between them and the closest ancestor both share, since a frame or a scroll bar
    // shows whether the focus is anywhere inside it. Below the shared ancestor that is the topmost
    // node of each side, whose place holds the rest; the shared ones above it saw the focus inside
    // before and still do.
    std::vector<Node*> before;
    std::vector<Node*> after;

    for (Node* at = was; at != nullptr; at = at->parent()) before.push_back(at);
    for (Node* at = one; at != nullptr; at = at->parent()) after.push_back(at);

    auto old_side = before.rbegin();
    auto new_side = after.rbegin();

    while (old_side != before.rend() && new_side != after.rend() && *old_side == *new_side) {
        ++old_side;
        ++new_side;
    }

    if (old_side != before.rend()) (*old_side)->changed();
    if (new_side != after.rend()) (*new_side)->changed();

    if (was != nullptr) {
        UnfocusEvent what{one};

        (void)sent(*was, what);
    }

    if (one != nullptr) {
        FocusEvent what{was};

        (void)sent(*one, what);
    }
}

// Delivers a key. A key and a paste go to the focus, and when there is no focus, to the root.
// Otherwise a shortcut handled by the application would stay silent until the user pointed at
// something.
//
// The root sees the key first if it wants to (`foresee`): a key after a prefix (Ctrl+X in
// Midnight Commander) belongs to the prefix, not to the focus; the letter after Ctrl+X must not
// reach the panel's search or a field. By default the root wants nothing and the key goes the
// ordinary way, from the focus up. `foresee` is called under the same guard as `sent`, because it
// may close a node or move the focus.
bool Root::dispatch(KeyEvent& what) {
    ++busy_;

    bool foreseen = false;

    try {
        foreseen = foresee(what);
    } catch (...) {
        released();

        throw;
    }

    released();

    if (foreseen) return true;

    return sent(focus_ != nullptr ? *focus_ : *this, what);
}

// Lets the root take a key before the focus does; by default it takes none.
bool Root::foresee(KeyEvent&) { return false; }

// Delivers a paste to the focus, or to the root when there is no focus.
bool Root::dispatch(PasteEvent& what) { return sent(focus_ != nullptr ? *focus_ : *this, what); }

// Delivers a terminal resize to the root.
bool Root::dispatch(SizeEvent& what) { return sent(*this, what); }

// Delivers a raw mouse event and then derives clicks and double clicks from it. The mouse aims at
// the deepest node under the cursor, not at the focus: one clicks what one sees. The raw event
// goes first and always, because whoever selects by dragging cannot wait for the verdict on a
// click.
//
// The exception is a capture (`capture`): movement and release then go to the node that asked for
// them, wherever the cursor is; otherwise a window dragged by its edge would lose the mouse at the
// first quicker move. A press starts anew and always goes under the cursor, and a release removes
// the capture, so a lost release does not leave the mouse tied forever. A captured node that has
// left the tree gets nothing more.
bool Root::dispatch(MouseEvent& what) {
    if (what.press() == Press::down) held_.reset();

    const std::shared_ptr<Node> kept = held_.lock();

    Node* aim = kept && kept->root() == this ? kept.get() : under(what.where());

    const bool made = sent(*aim, what);

    if (what.press() == Press::up) held_.reset();

    minded(what);

    return made;
}

// Captures the mouse for `one` until the button is released. The node that got the press asks for
// it, on request and not for everyone, because moving the mouse over menu items still has to go
// under the cursor. It is held weakly: the captured node may leave the tree during the drag.
void Root::capture(Node& one) { held_ = one.weak_from_this(); }

// Delivers a wheel step to the node under the cursor.
bool Root::dispatch(WheelEvent& what) { return sent(*under(what.where()), what); }

// Delivers a click to the node under the cursor.
bool Root::dispatch(ClickEvent& what) { return sent(*under(what.where()), what); }

// Delivers a double click to the node under the cursor.
bool Root::dispatch(DoubleClickEvent& what) { return sent(*under(what.where()), what); }

// Turns presses and releases into clicks and double clicks. A click goes at once, and a double
// click is an additional event; that is what Windows does (WM_LBUTTONDOWN at once, WM_LBUTTONDBLCLK
// instead of the second press), and X11 and browsers too. A single click does not wait for the
// double-click window to pass, so buttons, menu titles and key bar items react without delay;
// the receivers of a double click tolerate the click before it (a list sets the focus, then
// chooses; an editor places the cursor, then selects a word).
//
// A second release in the same cell, with the same button and no later than `double_click()` after
// the first gives a `DoubleClickEvent` alone, without a second `ClickEvent`, as in Windows. The
// window is measured with the moments the events themselves carry (`MouseEvent::when`), so the root
// needs no clock, and nobody outside has to tell it that the time is up: an expired first click
// simply does not match the second.
//
// Each root waits for its own: a click on one screen and a click on another do not combine into a
// double click, because those are two different roots.
//
// The back and forward buttons make no clicks. They are commands, not pointing: a dialog button
// under the pointer must not be pressed by "back", and a left click must not be broken by a back
// press in its middle. Whoever wants them takes their presses as `MouseEvent`.
void Root::minded(const MouseEvent& what) {
    if (what.button() == Mouse::back || what.button() == Mouse::forward) return;

    if (what.press() == Press::down) {
        began_ = what.where();
        pressing_ = what.button();

        return;
    }

    if (what.press() != Press::up) return;

    const Mouse which = pressing_;

    pressing_ = Mouse::none;

    // A release in a different cell than the press is a drag, not a click. A click waiting for its
    // second is left alone; time will close it.
    if (which == Mouse::none || which != what.button() || !(began_ == what.where())) return;

    if (first_click_.has_value() && clicked_ == what.button() && waiting_ == what.where() &&
        what.when() - *first_click_ <= double_click_) {
        first_click_.reset();

        DoubleClickEvent one{what.where(), what.button(), what.mods()};

        (void)dispatch(one);

        return;
    }

    first_click_ = what.when();
    waiting_ = what.where();
    clicked_ = what.button();

    ClickEvent one{what.where(), what.button(), what.mods()};

    (void)dispatch(one);
}

// The deepest visible node under the spot `where`. We descend floor by floor, subtracting the
// offset of each step, because a node's rectangle is expressed relative to its parent; that is how
// rendering records it. Among siblings the last matching one wins, because composing goes in order
// and the last one lies on top. A node with nothing under the spot is the target; until someone
// has laid things out, all rectangles are zero and the root stays the target.
//
// "On top" has one source: the order of children. Every container attaches its overlay after the
// content, so a veil or an open menu is on top because it comes later.
//
// A float occupies only what it holds. Its rectangle is the room its parent had to spare, and the
// margins in it belong to nobody: a click there is not a click on the float, so the float does not
// match and the search goes on among the siblings below it, as if it were not there. Otherwise a
// float over the whole room of a stack would take the mouse from every layer under it.
Node* Root::under(Spot where) {
    // Whether `one` covers the spot `at` given in its parent's coordinates, with a visible cell.
    // Only what is visible: a scroller's child stands in a rectangle larger than the window: a row
    // scrolled under the bar or past the bottom edge has its rectangle there, although it has not
    // drawn a single cell. Without this check, a click on the scroller's bar would hit the row
    // under it.
    const auto covers = [](const Node& one, Spot at) {
        const Area& box = one.area();

        if (at.x < box.at.x || at.x >= box.at.x + box.size.wide || at.y < box.at.y || at.y >= box.at.y + box.size.high)
            return false;

        const Area& seen = one.seen_;
        const Spot inner{at.x - box.at.x, at.y - box.at.y};

        return inner.x >= seen.at.x && inner.x < seen.at.x + seen.size.wide && inner.y >= seen.at.y &&
               inner.y < seen.at.y + seen.size.high;
    };

    // Whether a float holds the spot `at`, given in the float's own coordinates. A float occupies
    // only what it holds, so it is hit only where its child is, and through a chain of floats
    // (`Top{Right{icons}}`) only where the first node that is not a float is; a click in the empty
    // margin of any of them goes to what lies below.
    const auto holds = [&covers](const auto& self, const Node& one, Spot at) -> bool {
        if (one.children().empty()) return false;

        const Node& inside = *one.children().front();

        if (!covers(inside, at)) return false;

        if (dynamic_cast<const Float*>(&inside) == nullptr) return true;

        return self(self, inside, {at.x - inside.area().at.x, at.y - inside.area().at.y});
    };

    Node* at = this;

    for (;;) {
        Node* into = nullptr;

        for (const std::shared_ptr<Node>& one : at->children()) {
            if (!covers(*one, where)) continue;

            if (dynamic_cast<const Float*>(one.get()) != nullptr &&
                !holds(holds, *one, {where.x - one->area().at.x, where.y - one->area().at.y}))
                continue;

            into = one.get();
        }

        if (into == nullptr) return at;

        where.x -= into->area().at.x;
        where.y -= into->area().at.y;

        at = into;
    }
}

// Ends one delivery and, when it was the last, releases the nodes removed in the meantime.
// Deliveries are counted, because no node may be released during one: a removed node waits in
// `dying_` until the last delivery ends. A counter, not a flag, because a handler may deliver
// another event in the middle of the first. The `sent` template in the header raises it for the
// root's own deliveries, `Handler::delivering` for every event started in the tree (a button
// pressed by the program), and this function lowers it, also when a handler throws.
void Root::released() {
    if (--busy_ > 0) return;

    // Releasing may trigger further removals in destructors, so the list has to be detached first
    // and only then emptied.
    const std::vector<std::shared_ptr<Node>> taken = std::move(dying_);

    dying_.clear();
}

// Keeps a removed node alive until the current delivery ends; outside a delivery it does nothing.
void Root::linger(std::shared_ptr<Node> one) {
    if (busy_ > 0) dying_.push_back(std::move(one));
}

// Calls what the schedule has due by now (`Schedule::elapse`), inside a delivery like an event: a
// node that a call takes off the tree lives until the calls are over. Says whether anything was
// called; the loop then draws a frame, since a call changes what is on the screen, or asks for a
// frame for a node that computes its picture from the time.
bool Root::elapse() {
    ++busy_;

    bool made = false;

    try {
        made = schedule_.elapse();
    } catch (...) {
        released();

        throw;
    }

    released();

    return made;
}

// The cursor of the focused node on the screen, or nothing. The cursor is assembled by walking up
// over `area_`, the same way the mouse hits, so scrolling and nesting count by themselves and
// there is nothing to remember separately.
//
// On the way, membership is checked on every floor before the coordinates are carried up. A node
// answers even when it is not visible, so we ask whether its answer fits in what it got, and since
// every ancestor is checked separately, a field scrolled past the edge of a scrolled window drops
// out where it should, not only at the edge of the screen.
//
// Call it after the frame is composed: `area_` and the scrolling within a field are set only
// while painting.
std::optional<Spot> Root::caret() const {
    if (focus_ == nullptr) return {};

    std::optional<Spot> at = focus_->caret();

    if (!at.has_value()) return {};

    for (const Node* one = focus_; one != nullptr; one = one->parent()) {
        const Area& where = one->area();

        if (at->x < 0 || at->y < 0 || at->x >= where.size.wide || at->y >= where.size.high)
            return {};

        at->x += where.at.x;
        at->y += where.at.y;
    }

    return at;
}

// The shape of the cursor is that of the focus, the node the cursor belongs to; without a focus,
// the terminal's usual one.
CaretShape Root::caret_shape() const { return focus_ == nullptr ? CaretShape{} : focus_->caret_shape(); }

// Clears the focus when it is on the node being removed or anywhere under it. Otherwise it would
// point at a dead node, and that would come out only at the next key.
void Root::losing(const Node* one) {
    for (const Node* at = focus_; at != nullptr; at = at->parent())
        if (at == one) {
            focus_ = nullptr;

            return;
        }
}

}
