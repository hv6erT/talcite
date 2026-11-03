#include "talcite/menuseparator.hpp"

#include <algorithm>

#include "talcite/line.hpp"
#include "talcite/menu.hpp"
#include "talcite/skin.hpp"

#include "lining.hpp"

namespace talcite::ui {

// A separator in a menu is a plain node, not an item: it has no text, hint, keys or `pick`. The
// menu skips everything that is not an enabled item when moving with the arrows (`item()` gives
// nothing for a separator), so the separator does not need to pretend to be one. Its role stem is
// the same as the items', so that changing the menu's role stem covers the separator too.
MenuSeparator::MenuSeparator() { role("talcite.menu"); }

// A separator has nothing to hold inside.
int MenuSeparator::capacity() const { return 0; }

// A separator does not push the menu wider; its width comes from the other items.
Size MenuSeparator::wants(Size room) const { return {0, std::min(1, room.high)}; }

// Paints the line. The border kind comes from the menu we lie in, so the line is not thin in a
// thick border.
void MenuSeparator::paint(Canvas& onto) const {
    const Size given = onto.size();

    Border how = Border::thin;

    if (const Menu* one = dynamic_cast<const Menu*>(parent())) how = one->border();

    if (how == Border::none) return;

    const Skin& told = skin();

    const Lines lines = told.lines(how);
    const Style edge = told.style(role() + ".frame");

    across(onto, {0, 0}, given.wide, lines.across, edge);
}

}
