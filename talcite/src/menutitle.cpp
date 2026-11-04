#include "menutitle.hpp"

#include "talcite/menu.hpp"
#include "talcite/menubar.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "marking.hpp"

namespace talcite::ui {

namespace {

// Whether the title is highlighted: it has the focus (the bar is active but folded), or its menu
// is open.
bool lit(const MenuTitle& one) { return one.focused() || one.menu().opened(); }

}

// The title in the bar is a node separate from the list it opens. The title stands in the row of
// titles, and the list is a child of the bar's stack: two rectangles in two places of the tree,
// each knowing where it is.
//
// The title has no keys of its own: it asks the menu's `keys()`, so a hotkey letter added to the
// menu works without changes on the caller's side. The focus on the title means the bar is active
// but folded.
MenuTitle::MenuTitle(Menu& menu) : menu_(menu) {}

// The menu's title with one cell of space on each side, in a single row.
Size MenuTitle::wants(Size room) const {
    return {std::min(2 + measured(menu_.title()).wide, room.wide), std::min(1, room.high)};
}

// The title takes the focus, because an active bar has to put it somewhere, and a folded menu is
// outside the tree and cannot take it.
bool MenuTitle::focusable() const { return true; }

// Paints the title, highlighted when lit, with its hotkey letter marked.
void MenuTitle::paint(Canvas& onto) const {
    const Size given = onto.size();

    const Skin& how = skin();

    // The roles come from the menu, not from the title itself: for the skin the title and the list
    // are one menu, and `talcite.menubar` is the title's role.
    const Style look = how.style(lit(*this) ? menu_.role() + ".focus" : menu_.role());

    onto.fill({{0, 0}, {given.wide, 1}}, look);
    onto.put({1, 0}, menu_.title(), look);

    // The hotkey letter uses the same machinery as a menu item and a button. The title stands one
    // cell from the left edge of its strip, so the letter is shifted by that cell too.
    talcite::ui::marking(onto, {1, 0}, menu_.title(),
                         talcite::ui::shortcut_column(menu_.keys(), menu_.title()),
                         look, how.style(menu_.role() + ".key"));
}

// A folded title with the focus opens with the down arrow and Enter, as in Midnight Commander; the
// rest goes up to the bar: the side arrows, Esc and title letters. For a title with no items below
// it, Enter performs the title's action, because there is nothing to open.
bool MenuTitle::handle(KeyEvent& what) {
    if (!focused()) return false;

    const Mods mods = what.mods();

    if (mods.ctrl || mods.alt) return false;

    const Code code = what.key().code();

    if (code == Code::enter) {
        if (menu_.children().empty()) menu_.pick();
        else menu_.open();

        return true;
    }

    if (code == Code::down && !menu_.children().empty()) {
        menu_.open();

        return true;
    }

    return false;
}

// The letter of a title does what a click on it does: a menu without items is picked, any other is
// opened. Only the toggle is the mouse's: a letter pressed again does not close the menu. The keys
// live in the menu, not here, so that adding keys to the menu is all a caller has to do.
bool MenuTitle::handle(ShortcutEvent& what) {
    if (!menu_.keys().contains(what.key())) return false;

    menu_.activate();

    return true;
}

// A click on the title toggles, as in Midnight Commander. A click opens the menu, not only
// activates the bar: with the mouse a person aims at a particular menu and wants its contents.
// With the keyboard it is different: F9 takes the bar, and opening is chosen separately.
bool MenuTitle::handle(ClickEvent&) {
    if (menu_.children().empty()) {
        menu_.pick();

        return true;
    }

    if (menu_.opened()) {
        if (menu_.bar_ != nullptr) menu_.bar_->leave();

        return true;
    }

    if (root() != nullptr) menu_.open();

    return true;
}

}
