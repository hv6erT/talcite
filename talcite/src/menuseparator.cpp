#include "talcite/menuseparator.hpp"

#include <algorithm>

#include "talcite/line.hpp"
#include "talcite/menu.hpp"
#include "talcite/skin.hpp"

#include "lining.hpp"

namespace talcite::ui {

// **Kreska w menu to zwykły węzeł, a nie pozycja.** Dawniej była wyłączoną pozycją menu (MenuSeparator)
// — z tekstem, podpowiedzią, klawiszami i `pick`, a `enabled(true)` robiło z niej pozycję do
// wybrania. Menu i tak omija przy strzałkach wszystko, co nie jest dostępną pozycją (`item()`
// oddaje dla kreski pustkę), więc nie trzeba było jej udawać. Rdzeń roli ma ten sam co pozycje,
// żeby podmiana rdzenia menu obejmowała i kreskę.
MenuSeparator::MenuSeparator() { role("talcite.menu"); }

// Kreska nie ma czego trzymać w środku.
int MenuSeparator::capacity() const { return 0; }

// Kreska nie rozpycha menu — jej szerokość bierze się z pozostałych pozycji.
Size MenuSeparator::wants(Size room) const { return {0, std::min(1, room.high)}; }

// Znak bierzemy z menu, w którym leżymy, żeby kreska nie była cienka w grubej ramce.
View MenuSeparator::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — reguła opisana przy `Node::paint`.

    made.clear({{0, 0}, seen.size});

    Border how = Border::thin;

    if (const Menu* one = dynamic_cast<const Menu*>(parent())) how = one->border();

    if (how == Border::none) return made;

    const Skin& told = skin();

    const Lines lines = told.lines(how);
    const Style edge = told.style(role() + ".frame");

    across(made, {-seen.at.x, -seen.at.y}, given.wide, lines.across, edge);

    return made;
}

}
