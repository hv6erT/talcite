#include "talcite/submenu.hpp"

#include <algorithm>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/skin.hpp"

#include "marking.hpp"

// A submenu is a menu item that opens a list of its own instead of being picked: the cascading
// list known from Turbo Vision, which Midnight Commander does not have. It is meant to be used
// sparingly, where a group of commands would otherwise make a menu too tall for the screen.
//
// The list is an ordinary `Menu`, built with items and separators like any other. When it opens,
// the bar attaches it on top of the open menu, beside the item (`Menu::placement`), so a key from
// one of its items bubbles item → list → bar, the same way as in a menu of the bar. The right
// arrow, Enter, the item's letter or a click open the list; the left arrow or Esc close it and
// give the focus back to the item; the right arrow inside the list goes on to the next menu of
// the bar, as everywhere else. Moving to another item of the menu closes the list, and closing the
// menu closes it first (`Menu::shut`).
//
// The item holds its list, and the list knows the item by a raw pointer (`Menu::owner_`), the same
// way a menu of the bar knows its title. The item lives in the menu that holds it, while an open
// list lives in the bar, so the two can part: the item that goes away first unhooks itself from its
// list and leaves detaching it to the bar (`Menubar::fold`), which then closes everything left
// open.
namespace talcite::ui {

// A submenu with the given text and an optional hint, shown on the right before the mark.
Submenu::Submenu(std::string text, std::string hint)
    : MenuItem(std::move(text), std::move(hint)), menu_(std::make_shared<Menu>(std::string{})) {}

// The item that goes away unhooks itself from its list and does no more: it may be torn down
// together with the bar, and touching the tree from here would reach into nodes that are already
// half gone.
Submenu::~Submenu() {
    menu_->owner_ = nullptr;
    menu_->head_ = nullptr;
}

// Opens the list beside the item and puts the focus on it. Only an item of an open menu of a bar
// can open its list, because the list stands on the bar, beside the item. The list takes the bar
// and the shape of the menu it comes from, so that its border and shadow look the same. Another
// list open in the same menu closes first; an open list only takes the focus again.
void Submenu::pick() {
    if (!enabled()) return;

    Menu* up = dynamic_cast<Menu*>(parent());

    if (up == nullptr || up->bar_ == nullptr || !up->opened()) return;

    up->shut(this);

    menu_->bar_ = up->bar_;
    menu_->head_ = this;
    menu_->owner_ = this;
    menu_->shape_ = up->shape_;

    menu_->open();
}

// The width of an item and two cells more for the mark and the space before it: " Text  Hint ▸ ".
Size Submenu::wants(Size room) const {
    int wide = 4 + measured(text()).wide;

    if (!hint().empty()) wide += 2 + measured(hint()).wide;

    return {std::min(wide, room.wide), std::min(1, room.high)};
}

// Paints the item as `MenuItem` does, with the mark of a submenu on the right edge
// (`talcite.menu.more`) and the hint before it. While the list is open, the item stays highlighted,
// though the focus has gone down into the list: the person should see which item the list comes
// from.
View Submenu::paint(Size given, Area seen) {
    View made{seen.size};

    const Skin& how = skin();

    const bool lit = focused() || menu_->opened();

    const Style look = how.style(!enabled() ? role() + ".disabled" : lit ? role() + ".focus" : role());

    made.fill({{-seen.at.x, -seen.at.y}, given}, look);
    made.put({1 - seen.at.x, -seen.at.y}, text(), look);
    made.put({given.wide - 2 - seen.at.x, -seen.at.y}, how.glyph("talcite.menu.more"), look);

    if (!hint().empty())
        made.put({given.wide - 3 - measured(hint()).wide - seen.at.x, -seen.at.y}, hint(), look);

    if (enabled())
        talcite::ui::marking(made, {1 - seen.at.x, -seen.at.y}, text(), shortcut_column(text()), look,
                             how.style(role() + ".key"));

    return made;
}

// The right arrow opens the list, as the cascading menus of Turbo Vision and of desktops do. On an
// ordinary item the same arrow moves to the next menu of the bar. Nothing is caught with Ctrl or
// Alt, as in a menu.
bool Submenu::handle(KeyEvent& what) {
    if (what.mods().ctrl || what.mods().alt || what.key().code() != Code::right || !enabled()) return false;

    pick();

    return true;
}

}
