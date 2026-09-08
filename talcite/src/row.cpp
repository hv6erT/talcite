#include "talcite/row.hpp"

#include <algorithm>

namespace talcite::ui {

Row::Row(int gap) : gap_(std::max(gap, 0)) {}

void Row::gap(int given) { gap_ = std::max(given, 0); }

int Row::takes() const { return plenty; }

// **Odstęp należy się między dziećmi, które coś zajmują.** Dziecko o zerowej szerokości — pusty
// napis, wyłączony przycisk, węzeł bez treści — nie otwiera odstępu, bo zostawiłby po sobie dziurę
// w miejscu, w którym nic nie widać. Dlatego pamiętamy nie „które to z kolei dziecko", tylko „czy
// coś już stoi".
//
// To jest sprawa układu, nie treści: rozdzielanie przycisków pustymi węzłami było robotą, którą
// każde okno musiało powtarzać u siebie i przy każdym zdejmowaniu poprawiać.
Size Row::wants(Size room) const {
    Size made;

    for (const std::shared_ptr<Node>& one : children()) {
        const int apart = made.wide > 0 ? gap_ : 0;
        const Size wants =
            one->wants({std::max(room.wide - made.wide - apart, 0), room.high});

        if (wants.wide == 0) continue;

        made.wide += apart + wants.wide;
        made.high = std::max(made.high, wants.high);
    }

    return {std::min(made.wide, room.wide), std::min(made.high, room.high)};
}

View Row::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — reguła opisana przy `Node::paint`.

    made.bare({{0, 0}, seen.size});

    int x = 0;

    for (const std::shared_ptr<Node>& one : children()) {
        const int apart = x > 0 ? gap_ : 0;
        const int left = std::max(given.wide - x - apart, 0);
        const Size wants = one->wants({left, given.high});
        const Size gets{std::min(wants.wide, left), given.high};

        // Nikt nie wypada z pętli, nawet gdy miejsce się skończyło: węzeł, którego nie widać,
        // dostaje prostokąt o zerowej szerokości i dzięki temu jego area() mówi prawdę o tej
        // klatce. Ale odstępu przed sobą nie otwiera.
        const int from = gets.wide > 0 ? x + apart : x;

        const Area where{{from, 0}, gets};
        const Area part = Area::crossed(seen, where);

        made.within(one->render(where, part - where.at), part.at - seen.at);

        x = from + gets.wide;
    }

    return made;
}

}
