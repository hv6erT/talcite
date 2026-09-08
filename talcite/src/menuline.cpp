#include "talcite/menuline.hpp"

#include <algorithm>

#include "talcite/line.hpp"
#include "talcite/menu.hpp"
#include "talcite/skin.hpp"

#include "lining.hpp"

namespace talcite::ui {

// Kreska jest pozycją, po której nie da się chodzić — i to jedyne, czym różni się od zwykłej.
// Dzięki temu Menu nie zna żadnego trzeciego rodzaju dziecka: przy strzałkach po prostu omija
// wszystko, co niedostępne, a to samo załatwia wyszarzone pozycje.
MenuLine::MenuLine() : MenuItem(std::string{}) { able(false); }

// Kreska nie rozpycha menu — jej szerokość bierze się z pozostałych pozycji.
Size MenuLine::wants(Size room) const { return {0, std::min(1, room.high)}; }

// Znak bierzemy z menu, w którym leżymy, żeby kreska nie była cienka w grubej ramce.
View MenuLine::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — reguła opisana przy `Node::paint`.

    made.bare({{0, 0}, seen.size});

    LineStyle how = LineStyle::thin;

    if (const Menu* one = dynamic_cast<const Menu*>(parent())) how = one->border();

    if (how == LineStyle::none) return made;

    const Skin& told = skin();

    const Lines lines = told.get<Lines>(named(how));
    const Style edge = told.get<Style>(role() + ".frame");

    across(made, {-seen.at.x, -seen.at.y}, given.wide, lines.across, edge);

    return made;
}

}
