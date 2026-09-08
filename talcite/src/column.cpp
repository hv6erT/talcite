#include "talcite/column.hpp"

#include <algorithm>

namespace talcite::ui {

Column::Column(int gap) : gap_(std::max(gap, 0)) {}

void Column::gap(int given) { gap_ = std::max(given, 0); }

int Column::takes() const { return plenty; }

// Wysokości dzieci sumujemy, szerokość bierzemy z najszerszego — ale oddajemy **co najwyżej
// tyle, ile dostaliśmy**. Kolumna nie ma prawa powiedzieć rodzicowi, że potrzebuje stu wierszy,
// bo rodzic i tak nie ma ich skąd wziąć; od pokazywania tego, co się nie mieści, będzie VScroll.
//
// Odstęp należy się między dziećmi, które coś zajmują — reguła i powód opisane przy wierszu.
Size Column::wants(Size room) const {
    Size made;

    for (const std::shared_ptr<Node>& one : children()) {
        const int apart = made.high > 0 ? gap_ : 0;
        const Size wants =
            one->wants({room.wide, std::max(room.high - made.high - apart, 0)});

        if (wants.high == 0) continue;

        made.wide = std::max(made.wide, wants.wide);
        made.high += apart + wants.high;
    }

    return {std::min(made.wide, room.wide), std::min(made.high, room.high)};
}

// Nikt nie wypada z pętli, nawet gdy miejsce się skończyło: węzeł, którego nie widać, dostaje
// prostokąt o zerowej wysokości i dzięki temu jego area() mówi prawdę o tej klatce, zamiast
// pamiętać poprzednią. Odstępu przed sobą taki węzeł nie otwiera.
View Column::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — reguła opisana przy `Node::paint`.

    made.bare({{0, 0}, seen.size});

    int y = 0;

    for (const std::shared_ptr<Node>& one : children()) {
        const int apart = y > 0 ? gap_ : 0;
        const int left = std::max(given.high - y - apart, 0);
        const Size wants = one->wants({given.wide, left});
        const Size gets{given.wide, std::min(wants.high, left)};

        const int from = gets.high > 0 ? y + apart : y;

        const Area where{{0, from}, gets};
        const Area part = Area::crossed(seen, where);

        made.within(one->render(where, part - where.at), part.at - seen.at);

        y = from + gets.high;
    }

    return made;
}

}
