#include "talcite/row.hpp"

#include <algorithm>

namespace talcite::ui {

Row::Row(int gap) : gap_(std::max(gap, 0)) {}

Row::Row(RowShape shape) : gap_(std::max(shape.gap, 0)), wrapping_(shape.wrapping) {}

void Row::gap(int given) { gap_ = std::max(given, 0); }

int Row::capacity() const { return plenty; }

// **Odstęp należy się między dziećmi, które coś zajmują.** Dziecko o zerowej szerokości — pusty
// napis, wyłączony przycisk, węzeł bez treści — nie otwiera odstępu, bo zostawiłby po sobie dziurę
// w miejscu, w którym nic nie widać. Dlatego pamiętamy nie „które to z kolei dziecko", tylko „czy
// coś już stoi".
//
// To jest sprawa układu, nie treści: rozdzielanie przycisków pustymi węzłami było robotą, którą
// każde okno musiało powtarzać u siebie i przy każdym zdejmowaniu poprawiać.
//
// **Wiersz, który się łamie** (`wrapping`): dziecko, które nie mieści się w tym, co zostało z linii,
// zaczyna następną; każda linia stoi pośrodku. Tak rząd przycisków okna nie gubi ostatnich, gdy jest
// ich więcej, niż zmieści ekran — w 80 kolumnach i w języku z długimi napisami. Dziecko szersze niż
// cały wiersz dostaje linię dla siebie, przycięte.
std::vector<Row::Line> Row::laid(Size room) const {
    std::vector<Line> made(1);

    for (const std::shared_ptr<Node>& one : children()) {
        const Size wants = one->wants({room.wide, room.high});
        const int wide = std::min(wants.wide, room.wide);

        if (wide == 0) continue;

        if (!made.back().placed.empty() && made.back().wide + gap_ + wide > room.wide) made.emplace_back();

        Line& line = made.back();

        line.wide += (line.placed.empty() ? 0 : gap_) + wide;
        line.high = std::max(line.high, wants.high);
        line.placed.emplace_back(one.get(), Size{wide, wants.high});
    }

    return made;
}

Size Row::wants(Size room) const {
    if (wrapping_) {
        Size made;

        for (const Line& line : laid(room)) {
            made.wide = std::max(made.wide, line.wide);
            made.high += line.high;
        }

        return {std::min(made.wide, room.wide), std::min(made.high, room.high)};
    }

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

    made.clear({{0, 0}, seen.size});

    if (wrapping_) {
        const std::vector<Line> lines = laid(given);

        // Dziecko, które nic nie zajmuje, i tak dostaje prostokąt zerowy — jak w wierszu bez łamania.
        for (const std::shared_ptr<Node>& one : children()) {
            const bool placed = std::any_of(lines.begin(), lines.end(), [&one](const Line& line) {
                return std::any_of(line.placed.begin(), line.placed.end(), [&one](const auto& each) { return each.first == one.get(); });
            });

            if (!placed) static_cast<void>(one->render({{0, 0}, {0, 0}}, {{0, 0}, {0, 0}}));
        }

        int y = 0;

        for (const Line& line : lines) {
            int x = std::max((given.wide - line.wide) / 2, 0);

            for (const auto& [one, size] : line.placed) {
                const Area where{{x, y}, {size.wide, line.high}};
                const Area part = Area::crossed(seen, where);

                made.draw(one->render(where, part - where.at), part.at - seen.at);

                x += size.wide + gap_;
            }

            y += line.high;
        }

        return made;
    }

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

        made.draw(one->render(where, part - where.at), part.at - seen.at);

        x = from + gets.wide;
    }

    return made;
}

}
