#include "talcite/list.hpp"

#include <algorithm>
#include <climits>

#include "talcite/skin.hpp"
#include "talcite/listitem.hpp"
#include "talcite/root.hpp"

#include "scrollbar.hpp"

namespace talcite::ui {

namespace {

// The position of `one` among `all`, or -1, looked for from `near` outward, both ways at once. The
// row a focus or a program goes to is almost always the current one or its neighbor, so an arrow
// in a list of a hundred thousand rows looks at a couple of them, not at all the rows above.
int index_near(const Children& all, const Node* one, int near) {
    const int much = static_cast<int>(all.many());

    if (much == 0) return -1;

    near = std::clamp(near, 0, much - 1);

    for (int step = 0; near - step >= 0 || near + step < much; ++step) {
        if (near - step >= 0 && all[static_cast<std::size_t>(near - step)].get() == one) return near - step;

        if (near + step < much && all[static_cast<std::size_t>(near + step)].get() == one) return near + step;
    }

    return -1;
}

// The bar along the right edge of a list of size `given` showing `much` rows from `shift` on, the
// same for the layout, which hands it to the grip, and for the painting.
Track tracked(Size given, int shift, int much) {
    return Track{Scrollbar::edge(given, {{0, 0}, given}, true), true, given.high, static_cast<std::size_t>(shift),
                 static_cast<std::size_t>(much), static_cast<std::size_t>(given.high)};
}

}

// A list that asks for `wide` by `high` cells, with the scrollbar its `ScrollShape` asks for.
List::List(int wide, int high, ScrollShape shape)
    : shape_(shape), grip_(std::make_shared<Grip>()), wide_(wide), high_(high) {}

// A list holds its items as children, just like a menu, so it accepts any number of them.
int List::capacity() const { return plenty; }

// A list is made of rows only. `which()` and `current()` count positions among the children and
// treat them as items, so a foreign child would put the numbering out of step with what is shown.
bool List::accepts(const Node& one) const { return dynamic_cast<const ListItem*>(&one) != nullptr; }

// A list is always focusable, even when empty. An empty file panel must be reachable with Tab;
// otherwise an empty directory would push the focus aside. Focusability is also the boundary of
// the traversal here: a focusable widget is a leaf for Tab, so Tab stops on the list and not on
// each of its rows.
//
// There is deliberately no way to disable the whole list. A disabled list would not be focusable,
// and then Tab would descend into it and stop on a row, because the row would still be focusable.
// Single items are disabled instead, the same notion as in a menu.
bool List::focusable() const { return true; }

// The size given at construction, cut to the room offered.
Size List::wants(Size room) const {
    return {std::min(wide_, room.wide), std::min(high_, room.high)};
}

// The item at the given position, or `nullptr` when the position is outside the list.
ListItem* List::item(int which) const {
    if (which < 0 || which >= static_cast<int>(children().many())) return nullptr;

    return dynamic_cast<ListItem*>(children()[static_cast<std::size_t>(which)].get());
}

// The nearest enabled item in the given direction, or -1, without wrapping around. A menu wraps
// because it is short and fully visible; a list of ten thousand files that jumps from the last
// row to the first loses the user. The Midnight Commander panel decides it the same way.
int List::nearest(int from, int step) const {
    const int much = static_cast<int>(children().many());

    if (much == 0 || step == 0) return -1;

    for (int at = from + step; at >= 0 && at < much; at += step) {
        const ListItem* one = item(at);

        if (one != nullptr && one->enabled()) return at;
    }

    return -1;
}

// The focus goes to an item, not to the list, and everything else follows from that: a key starts
// at the row, so a handler attached to the row sees it first, and the list learns the number of
// the current row by bubbling instead of keeping its own bookkeeping beside the focus.
//
// When there is nothing to stand on (the list is empty or all items are disabled), the list takes
// the focus itself.
void List::focus() {
    ListItem* one = item(which_);

    if (one == nullptr || !one->enabled()) one = item(nearest(-1, 1));

    if (one != nullptr) {
        one->focus();

        return;
    }

    Node::focus();
}

// The current row has two faces under two names, as in every widget with items: `which()` is its
// number, `current()` the row itself. Whoever wants to do something with the row (read its text,
// press it, compare identity) takes `current()`; whoever needs a number for their own array
// alongside takes `which()`. Emptiness is `nullopt` and `nullptr`, with no sentinel number.
// Inside, the number is kept as an `int` with -1, because the step arithmetic in `nearest` works
// on it.
std::optional<std::size_t> List::which() const {
    if (item(which_) == nullptr) return std::nullopt;

    return static_cast<std::size_t>(which_);
}

// The current row itself, or `nullptr` when there is none.
ListItem* List::current() const { return item(which_); }

// Sets the current row, clamped to the list. When the list holds the focus, the row is set
// through the focus; otherwise the number is remembered and used the next time the list is
// entered. That way leaving with Tab for a button and coming back returns to the same row.
void List::aim(int given) {
    const int much = static_cast<int>(children().many());

    if (much == 0) {
        which_ = -1;

        changed();

        return;
    }

    which_ = std::clamp(given, 0, much - 1);

    changed();
    follow_ = true;

    const Root* top = root();

    if (top == nullptr) return;

    if (const Node* had = top->focused_node(); had != nullptr && had->within(*this))
        if (ListItem* one = item(which_); one != nullptr && one->enabled()) one->focus();
}

// Sets the current row by number, for building a list when the caller has the number of an
// option but not the row yet. A number past the end is cut to the last row.
void List::which(std::size_t given) {
    aim(static_cast<int>(std::min<std::size_t>(given, static_cast<std::size_t>(INT_MAX))));
}

// Makes the given row current. A row that is not in the list changes nothing.
void List::current(ListItem& one) {
    if (const int at = index_near(children(), &one, which_); at >= 0) which(static_cast<std::size_t>(at));
}

// The number of the first visible row.
std::size_t List::first() const { return static_cast<std::size_t>(shift_); }

// Picks the current row, as Enter on it would.
void List::pick() {
    if (ListItem* one = item(which_)) one->pick();
}

// Scrolls the view without moving the current row. The upper bound is not checked here, because
// it depends on a height not known at this moment; `layout` cuts it.
//
// The change is announced with an event, not with a callback of its own: one thing has one path,
// and events go through `on` and the hook it gives. The event carries no row number, because
// whoever asks has it at hand in `which()`, and the view in `first()`.
void List::first(std::size_t given, Moved how) {
    const int was = shift_;

    shift_ = static_cast<int>(std::min<std::size_t>(given, static_cast<std::size_t>(INT_MAX)));

    changed();

    if (shift_ != was) (void)trigger(ScrollEvent{how});
}

// Makes the given row current and moves the focus to it; a negative number means there is nowhere
// to go. The key is consumed either way. The list's own keys and a click on an item come this way,
// and the list announces the move here, saying how it came (`ScrollEvent::by_hand`), before the
// focus follows; the focus arriving at the row then finds it current already. A focus put on an
// item from elsewhere (Tab, the program) still meets the list in `handle(FocusEvent&)`.
bool List::going(int to, Moved how) {
    if (to < 0) return true;

    ListItem* one = item(to);

    if (one == nullptr) return true;

    follow_ = true;

    if (which_ != to) {
        which_ = to;

        changed();

        (void)trigger(ScrollEvent{how});
    }

    one->focus();

    return true;
}

// Steps the given number of rows, item by item, so that disabled items do not count as passed and
// the movement stops at the end instead of going past it.
bool List::walking(int step, int times, Moved how) {
    int at = which_;

    for (int which = 0; which < times; ++which) {
        const int to = nearest(at, step);

        if (to < 0) break;

        at = to;
    }

    return going(at, how);
}

// The arrows and the page keys belong to the list; Enter has already been taken by the item,
// because the item has the focus. Letters, Tab and Esc are left to the window, unlike in a text
// field, which has to swallow letters because one types in it. Here a letter has to reach a button
// in the dialog, or in the file manager the quick search that the panel adds itself.
//
// At its ends the list keeps the arrows, page keys, Home and End, even when they move nothing: in a
// dialog an arrow pressed once too often must not carry the focus off to a button, where the Enter
// or Space that follows would press it instead of acting on the row. A group of options, a few
// rows at most, and a history field pass them on instead (`Modal`).
//
// The page height comes from the rectangle of the previous frame: only it knows the width and
// height the list really got. Before the first paint a page is one row.
bool List::handle(KeyEvent& what) {
    if (what.mods().ctrl || what.mods().alt) return false;

    const int page = std::max(area().size.high, 1);

    switch (what.key().code()) {
        case Code::up: return walking(-1, 1, Moved::by_hand);

        case Code::down: return walking(1, 1, Moved::by_hand);

        case Code::pageup: return walking(-1, page, Moved::by_hand);

        case Code::pagedown: return walking(1, page, Moved::by_hand);

        case Code::home: return going(nearest(-1, 1), Moved::by_hand);

        case Code::end: return going(nearest(static_cast<int>(children().many()), -1), Moved::by_hand);

        default: return false;
    }
}

// The wheel moves only the view and leaves the current row where it is, like the scroller and like
// the Midnight Commander panel. If it moved the row, every notch would change the current row, and
// in the file manager the preview refresh hangs on that.
//
// At the end of the scroll range we return false, so the movement goes up and a scroller above
// the list gets it; so does the tilted wheel, since the list does not scroll sideways.
bool List::handle(WheelEvent& what) {
    if (what.axis() != Axis::vertical) return false;

    const int much = static_cast<int>(children().many());
    const int window = std::max(area().size.high, 0);
    const int was = shift_;

    shift_ = std::clamp(shift_ - wheeled(what.steps()), 0, std::max(much - window, 0));

    changed();

    if (shift_ == was) return false;

    (void)trigger(ScrollEvent{Moved::by_hand});

    return true;
}

// Moves the view to a new place, cut to the content; it announces only an actual change, as with
// the wheel.
void List::slid(int to, Moved how) {
    const int much = static_cast<int>(children().many());
    const int window = std::max(area().size.high, 0);
    const int was = shift_;

    shift_ = std::clamp(to, 0, std::max(much - window, 0));

    changed();

    if (shift_ != was) (void)trigger(ScrollEvent{how});
}

// The mouse on the scrollbar: the track moves one page, the thumb is dragged.
// Like the wheel, it moves the view but not the current row.
bool List::handle(MouseEvent& what) {
    const Asked asked = grip_->mouse(what, local(what.where()), *this);
    const int window = std::max(area().size.high, 1);

    switch (asked.part) {
        case Part::none: return false;
        case Part::before: slid(shift_ - window, Moved::by_hand); break;
        case Part::after: slid(shift_ + window, Moved::by_hand); break;
        case Part::thumb:
            if (asked.to.has_value()) slid(static_cast<int>(*asked.to), Moved::by_hand);
            break;
    }

    return true;
}

// All the bookkeeping of the current row is here. A focus placed on an item, by a key, a click,
// Tab or from the program, bubbles from there to the list, so all these paths meet in one place
// instead of each setting the number its own way.
bool List::handle(FocusEvent& what) {
    Node* one = dynamic_cast<Node*>(what.target());

    if (one == nullptr || one == this) return false;

    const int at = index_near(children(), one, which_);

    if (at >= 0) {
        follow_ = true;

        if (which_ != at) {
            which_ = at;

            changed();

            (void)trigger(ScrollEvent{});
        }
    }

    return false;
}

// Lays the visible rows out and settles the scroll position, because only here is the real height
// known; the scrollbar's track goes to the grip.
void List::layout(Size given) {
    const int much = static_cast<int>(children().many());

    // Items may have disappeared behind the list's back: `Children::remove` is not virtual, so the row
    // number does not fix itself. It is cut here, because everything is recomputed here anyway.
    if (which_ >= much) which_ = much - 1;

    // A scrollbar shown "when needed" (`needed`) appears only for a list longer than the window.
    const int aside = shape_.scrollbar && given.wide > 0 && (!shape_.needed || much > given.high) ? 1 : 0;
    const int wide = given.wide - aside;

    const int was = shift_;

    // The scroll position is computed here, because only here is the height the list really got
    // known. The view catches up with the current row only when the row moved; without this
    // condition the wheel would be undone in the same frame in which it was turned.
    if (follow_ && which_ >= 0 && given.high > 0) {
        if (which_ < shift_) shift_ = which_;

        if (which_ >= shift_ + given.high) shift_ = which_ - given.high + 1;
    }

    follow_ = false;

    shift_ = std::clamp(shift_, 0, std::max(much - given.high, 0));

    // The bar shows the offset: one the layout moved is a change of the list, not only of its rows.
    if (shift_ != was) changed();

    // The rows in view are laid out, and their list is kept for painting and for the next frame.
    const int from = shift_;
    const int upto = std::clamp(shift_ + std::max(given.high, 0), from, much);

    std::vector<std::weak_ptr<Node>> laid;
    std::vector<const Node*> now;

    laid.reserve(static_cast<std::size_t>(upto - from));
    now.reserve(static_cast<std::size_t>(upto - from));

    for (int at = from; at < upto; ++at) {
        const std::shared_ptr<Node>& row = children()[static_cast<std::size_t>(at)];

        row->arrange({{0, at - shift_}, {wide, 1}});

        laid.push_back(row);
        now.push_back(row.get());
    }

    // A row out of view gets a rectangle of zero height, so its `area()` tells the truth about this
    // frame; otherwise a row that just scrolled away would catch the mouse where it stood before.
    // Only a row that still has a rectangle needs that, and such a row was laid in the previous
    // frame, so only those are looked at: a list of a hundred thousand rows costs its window, not
    // all of them. A row laid then may since have been removed or moved under another parent;
    // its rectangle there is not ours to touch.
    //
    // The exception is a change in the set of rows. A row brought from another parent carries the
    // rectangle it had there, and this list never laid it, so after every change all rows are
    // looked at once; that frame follows someone adding or removing rows, which costs as much.
    const auto hidden = [&](Node& row) {
        if (row.area().size.high == 0 && row.area().at == Spot{0, 0}) return;

        row.arrange({{0, 0}, {wide, 0}});
    };

    if (children().changes() != swept_) {
        for (int at = 0; at < much; ++at)
            if (at < from || at >= upto) hidden(*children()[static_cast<std::size_t>(at)]);

        swept_ = children().changes();
    } else {
        std::sort(now.begin(), now.end());

        for (const std::weak_ptr<Node>& one : laid_) {
            const std::shared_ptr<Node> row = one.lock();

            if (!row || row->parent() != this) continue;

            if (!std::binary_search(now.begin(), now.end(), row.get())) hidden(*row);
        }
    }

    laid_ = std::move(laid);

    if (aside != 0) {
        grip_->laid(tracked(given, shift_, much));
    } else {
        grip_->laid(std::nullopt);
    }
}

// Paints the rows laid out in view and, when there is one, the scrollbar.
void List::paint(Canvas& onto) const {
    for (const std::weak_ptr<Node>& one : laid_)
        if (const std::shared_ptr<Node> row = one.lock(); row && row->parent() == this) row->render(onto);

    const Size given = onto.size();
    const int much = static_cast<int>(children().many());

    if (!shape_.scrollbar || given.wide <= 0 || (shape_.needed && much <= given.high)) return;

    const Scrollbar bar{.track = tracked(given, shift_, much),
                        .focused = Scrollbar::focused_in(*this),
                        .skin = skin(),
                        .role = scrollbar_role_};

    bar.draw(onto);
}

}
