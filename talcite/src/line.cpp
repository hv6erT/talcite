#include "talcite/line.hpp"

#include <algorithm>
#include <utility>

#include "talcite/skin.hpp"
#include "lining.hpp"
#include "titling.hpp"

namespace talcite::ui {

Line::Line(std::string text, LineStyle lines) : text_(std::move(text)), lines_(lines) {}

int Line::takes() const { return 0; }

// **Rozdzielnik nie rozpycha rodzica.** Prosi o szerokość samego napisu, a bez napisu o zero —
// tak samo i z tego samego powodu, co kreska w menu: to nie linia ma decydować, jak szerokie jest
// okno, w którym stoi. Rozciąga ją kolumna, bo każdemu dziecku daje **całą swoją szerokość**,
// a nie tylko tyle, ile chciało. Wysokość zawsze jeden wiersz: rozdzielnik, który się zawija,
// to nie rozdzielnik.
Size Line::wants(Size room) const {
    const int wide = text_.empty() ? 0 : 2 + measured(text_).wide;

    return {std::min(wide, room.wide), std::min(1, room.high)};
}

View Line::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — reguła opisana przy `Node::paint`.

    made.bare({{0, 0}, seen.size});

    const Skin& how = skin();

    const Style look = how.get<Style>("talcite.frame");

    if (lines_ != LineStyle::none) {
        const Lines glyphs = how.get<Lines>(named(lines_));

        across(made, {-seen.at.x, -seen.at.y}, given.wide, glyphs.across, look);
    }

    // Napis rysuje ta sama funkcja, którą ramka stawia tytuł w krawędzi — bo to jest ta sama rzecz.
    along(made, {-seen.at.x, -seen.at.y}, given.wide, text_, justify_,
          how.get<Style>("talcite.title"), how.get<char32_t>("talcite.elide"));

    return made;
}

}
