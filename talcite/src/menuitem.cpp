#include "talcite/menuitem.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/menu.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "marking.hpp"

namespace talcite::ui {

// A menu item with the given text, in the role "talcite.menu".
MenuItem::MenuItem(std::string text) : text_(std::move(text)) { role("talcite.menu"); }

// A menu item with the given text and a hint shown on the right, usually the shortcut.
MenuItem::MenuItem(std::string text, std::string hint)
    : text_(std::move(text)), hint_(std::move(hint)) {
    role("talcite.menu");
}

// An item has nothing to hold inside, just like a label.
int MenuItem::capacity() const { return 0; }

// The column where the text starts: one cell from the left, or three in a menu that holds items
// with a check mark (`checked`), whose marks stand in the first column. Every item of such a menu
// leaves the column of the marks, so that the texts stand one under the other, as in the menus of
// desktops; an item that can be checked leaves it also outside a menu.
int MenuItem::lead() const {
    const auto* menu = dynamic_cast<const Menu*>(parent());

    return checked_.has_value() || (menu != nullptr && menu->checks()) ? 3 : 1;
}

// One space on each side and two between the text and the shortcut hint, so that "Copy" does not
// stick to "F5", and the column of the marks in a menu that has them (`lead`). The height is
// always one row: a menu item that wraps is not an item.
Size MenuItem::wants(Size room) const {
    int wide = lead() + 1 + measured(text_).wide;

    if (!hint_.empty()) wide += 2 + measured(hint_).wide;

    return {std::min(wide, room.wide), std::min(1, room.high)};
}

// Paints the item. The highlighted item is the one with the focus. There is no separate flag,
// because two sources of the same truth drift apart sooner or later; for exactly this reason a
// menu has no "open" field. A separator never takes the focus, so nothing here needs to know about
// it.
//
// The highlight is reverse video, not color: it works on every terminal and does not require
// inventing colors nobody asked for. A disabled item is dimmed.
//
// The role is built from a stem and a suffix, as in a button: whoever assembles the menu sets the
// stem, and the item adds the suffix, because only it knows what states it has. A checked item
// shows its mark (`talcite.menu.check`) in the column of the marks.
void MenuItem::paint(Canvas& onto) const {
    const Size given = onto.size();

    const Skin& how = skin();

    const Style look = how.style(!enabled()      ? role() + ".disabled"
                                      : focused() ? role() + ".focus"
                                                  : role());

    const int from = lead();

    onto.fill({{0, 0}, given}, look);

    if (checked_.value_or(false)) onto.put({1, 0}, how.glyph("talcite.menu.check"), look);

    onto.put({from, 0}, text_, look);

    if (!hint_.empty()) {
        const int wide = measured(hint_).wide;

        onto.put({given.wide - 1 - wide, 0}, hint_, look);
    }

    // The hotkey letter has its own role, laid over the item's look, as in a button and a check
    // box. The letter is shifted as the text is (`lead`). A disabled item marks no letter, since
    // its key does nothing.
    if (enabled())
        talcite::ui::marking(onto, {from, 0}, text_, shortcut_column(text_),
                          look, how.style(role() + ".key"));
}

// The hotkey arrives from the base class. The item does the same as on a click: it first becomes
// current, then the pick follows, because otherwise the menu would pick the item the arrows last
// visited.
void MenuItem::activate() {
    if (Menu* one = dynamic_cast<Menu*>(parent())) {
        one->current(*this);
        one->pick();

        return;
    }

    pick();
}

// Picks the item by firing a `PickEvent`, unless it is disabled. Closing the menu is the job of
// `Menu`, because the item does not know that it lies in something that opens. A submenu picks
// differently: it opens its list (`Submenu::pick`).
void MenuItem::pick() {
    if (!enabled()) return;

    (void)trigger(PickEvent{});
}

// A click picks the item through its menu. A disabled item swallows the click.
bool MenuItem::handle(ClickEvent&) {
    if (!enabled()) return true;

    // The clicked item first becomes highlighted. Otherwise the menu would pick the item the
    // arrows last visited, not the one under the cursor.
    if (Menu* one = dynamic_cast<Menu*>(parent())) {
        one->current(*this);
        one->pick();

        return true;
    }

    pick();

    return true;
}

// Hovering moves the highlight, as in Midnight Commander. The event is passed on, because mouse
// movement belongs to nobody exclusively.
bool MenuItem::handle(MouseEvent& what) {
    if (what.press() != Press::move || !enabled()) return false;

    if (Menu* one = dynamic_cast<Menu*>(parent())) one->current(*this);

    return false;
}

}
