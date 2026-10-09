#include "talcite/line.hpp"

#include <algorithm>
#include <utility>

#include "talcite/skin.hpp"
#include "lining.hpp"
#include "titling.hpp"

namespace talcite::ui {

// Rola jak u ramki, bo rozdzielnik jest kawałkiem ramki: kreska w barwie `role()`, napis
// w `role() + ".title"`. Domyślnie `talcite.frame`, więc bez nadanej roli wygląda jak dotąd;
// z rolą nadaną — jak jego ramka, np. kreska nad wierszem stanu w panelu czynnym i biernym.
Line::Line(std::string text, Border border) : text_(std::move(text)), border_(border) {
    role("talcite.frame");
}

int Line::capacity() const { return 0; }

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

    made.clear({{0, 0}, seen.size});

    const Skin& how = skin();

    const Style look = how.style(role());

    if (border_ != Border::none) {
        const Lines glyphs = how.lines(border_);

        across(made, {-seen.at.x, -seen.at.y}, given.wide, glyphs.across, look);
    }

    // Napis rysuje ta sama funkcja, którą ramka stawia tytuł w krawędzi — bo to jest ta sama rzecz.
    along(made, {-seen.at.x, -seen.at.y}, given.wide, text_, justify_,
          how.style(role() + ".title"), how.glyph("talcite.elide"));

    return made;
}

}
