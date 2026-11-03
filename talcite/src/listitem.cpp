#include "talcite/listitem.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>

#include "eliding.hpp"
#include "talcite/list.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

namespace talcite::ui {

namespace {

// Whether this is the current row of its list. We ask the list, because the list keeps the number;
// the item holds no second flag beside the focus and that number.
//
// This is not a public method and is not meant to be. Whoever asks about the current row asks the
// list: `current()`. The item is not needed for that, and a second path to one truth is always a
// second path to disagreement.
bool current(const ListItem& one) {
    const List* up = dynamic_cast<const List*>(one.parent());

    if (up == nullptr) return false;

    return up->current() == &one;
}

}

// A list row with the given text, in the role "talcite.list".
ListItem::ListItem(std::string text) : text_(std::move(text)) { role("talcite.list"); }

// An item has nothing to hold inside, just like a label and a menu item.
int ListItem::capacity() const { return 0; }

// No padding at the sides, unlike a menu item: in a file list the name starts at the first cell
// and there is nothing to set it apart from. The width is the text alone; the list stretches it by
// giving the row its whole rectangle.
Size ListItem::wants(Size room) const {
    return {std::min(measured(text_).wide, room.wide), std::min(1, room.high)};
}

// Picks the item by firing a `PickEvent`, unless it is disabled. The item does not know, and need
// not know, that it lies in something that scrolls. The event bubbles through the list, so the
// application can listen on the list with one handler instead of hanging a callback on each row;
// `Event::target()` says which row it was.
void ListItem::pick() {
    if (!enabled()) return;

    (void)trigger(PickEvent{});
}

// Paints the row. The current item is the one with the focus. There is no separate "highlighted"
// flag, because two sources of the same truth drift apart sooner or later; for exactly this reason
// a menu has no "open" field and asks about the focus instead. The skin thus has a role for "item
// with the focus", not for "item with some flag that happens to mean the same".
//
// The current row without the focus has its own role. The Midnight Commander panel shows no cursor
// when inactive, but a listbox in a dialog shows it in another color, and that is
// `talcite.list.mark`. The built-in skin leaves this role empty, so nothing shows; a skin with
// colors can fill it without any change to the code.
//
// A text longer than the row is cut with an ellipsis on the right, as in a button. A text field
// scrolls because one types in it; a list row has nothing to scroll, so cutting is more honest.
void ListItem::paint(Canvas& onto) const {
    const Size given = onto.size();

    const Skin& how = skin();

    const Style look = how.style(!enabled()           ? role() + ".disabled"
                                      : focused()      ? role() + ".focus"
                                      : current(*this) ? role() + ".mark"
                                                       : role());

    onto.fill({{0, 0}, given}, look);
    onto.put({0, 0}, elided(text_, given.wide, Elide::right, how.glyph("talcite.elide")), look);
}

// Enter is the key of the item itself, because the item has the focus, unlike in a menu, where
// the menu takes Enter because its items have no focus. Everything else is passed on: the arrows
// and the page keys belong to the list, letters and Tab to the window.
//
// `handle` runs before the key handlers attached with `on` but does not hide anything from them:
// there are two channels, and true from `handle` closes only the widget path. An attached handler
// therefore sees every key, Enter included, and Enter is additionally announced as a `PickEvent`.
bool ListItem::handle(KeyEvent& what) {
    if (!enabled()) return false;

    if (what.mods().ctrl || what.mods().alt) return false;

    if (what.key().code() != Code::enter) return false;

    pick();

    return true;
}

// A click makes the item current and takes the focus. In a list the list does it (`List::going`),
// so that it announces the move as made by hand, the same as its arrows; the row number, pulling
// the view along and notifying the application are the list's. An item outside a list only takes
// the focus.
bool ListItem::handle(ClickEvent&) {
    // A disabled item lets the click pass up instead of swallowing it: since it does not answer
    // to its key, there is no reason for it to answer to the mouse, and the list underneath may do
    // with the click whatever it sees fit.
    if (!enabled()) return false;

    chosen();

    return true;
}

// Makes the item current by hand: through its list when it stands in one, otherwise by the focus
// alone. Nothing happens outside a tree, where there is no focus to take.
void ListItem::chosen() {
    if (root() == nullptr) return;

    auto* list = dynamic_cast<List*>(parent());
    const std::optional<std::size_t> at = list != nullptr ? list->children().find(*this) : std::nullopt;

    if (at.has_value()) (void)list->going(static_cast<int>(*at), Moved::by_hand);
    else focus();
}

// A double click on a row does what Enter does, and also puts the focus on it on the way, because
// the double click may land on a row that is not current yet.
bool ListItem::handle(DoubleClickEvent&) {
    if (!enabled()) return false;

    chosen();

    pick();

    return true;
}

}
