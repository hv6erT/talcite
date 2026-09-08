#include "talcite/view.hpp"

#include <algorithm>
#include <utility>

#include "talcite/encoding.hpp"

namespace talcite {

// Dwa style w jeden: **wierzchni dokłada, ale nie odbiera**. Kolor przezroczysty przepuszcza to,
// co pod spodem, a ustawiony zasłania; znaczniki się sumują.
//
// Po to jest, żeby widżet spoza biblioteki mógł mieć własny wygląd zapasowy: `merge(mój, ze skórki)`
// daje mój wygląd, gdy skórka o tej roli nie słyszała, i wygląd ze skórki tam, gdzie coś powiedziała.
// Sumowanie znaczników, a nie zastępowanie, bierze się wprost z tego zastosowania — rola nieustawiona
// oddaje styl pusty, a zastępowanie wymazywałoby nim to, co dał wołający.
//
// **Ceną jest to, że wierzchni nie zgasi znacznika spodniego.** `Style` nie ma stanu „nie ustawiono"
// na znacznikach — ma go tylko kolor — więc odróżnić „nie mam zdania" od „ma być wyłączone" nie da
// się bez dokładania tego stanu do typu, przez który przechodzi całe rysowanie.
Style Style::merge(Style base, Style over) {
    Style made = base;

    if (!over.ink.transparent) made.ink = over.ink;

    if (!over.paper.transparent) made.paper = over.paper;

    made.bold = base.bold || over.bold;
    made.faint = base.faint || over.faint;
    made.italic = base.italic || over.italic;
    made.under = base.under || over.under;
    made.reverse = base.reverse || over.reverse;
    made.crossed = base.crossed || over.crossed;

    return made;
}

// Napis mierzymy w kratkach, a nie w znakach ani w bajtach: ogonek liczy się za jeden, znak
// pisma wschodniego za dwa.
ui::Size measured(std::string_view what) {
    ui::Size made{0, what.empty() ? 0 : 1};

    std::size_t at = 0;

    while (at < what.size()) made.wide += widely(letter(what, at));

    return made;
}

View::View(ui::Size given) {
    // Ujemny wymiar bierze się z ciasnego okna, nie z błędu wołającego — „zostało minus dwa"
    // ma dać pustą kartkę, a nie wywrócić program.
    size_.wide = std::max(given.wide, 0);
    size_.high = std::max(given.high, 0);

    cells_.assign(static_cast<std::size_t>(size_.wide) * static_cast<std::size_t>(size_.high),
                  Cell{});
}

bool View::holds(ui::Spot where) const {
    return where.x >= 0 && where.y >= 0 && where.x < size_.wide && where.y < size_.high;
}

Cell& View::cell(ui::Spot where) {
    return cells_[static_cast<std::size_t>(where.y) * static_cast<std::size_t>(size_.wide) +
                  static_cast<std::size_t>(where.x)];
}

// Znak podwójnej szerokości stoi w dwóch komórkach i pisanie w jedną z nich osieroca drugą.
// Zamieniamy tę drugą na spację, zanim cokolwiek nadpiszemy — inaczej na ekranie zostaje połowa
// znaku, która przy porównywaniu klatek zaczyna mrugać.
void View::broken(ui::Spot where) {
    if (!holds(where)) return;

    const Cell one = cell(where);

    if (one.letter == 0 && where.x > 0) cell({where.x - 1, where.y}).letter = U' ';

    if (widely(one.letter) == 2 && holds({where.x + 1, where.y})) {
        Cell& after = cell({where.x + 1, where.y});

        if (after.letter == 0) after.letter = U' ';
    }
}

void View::letter(ui::Spot at, char32_t what, const Style& how) {
    if (what == 0 || !holds(at)) return;

    const int much = widely(what);

    // Połowa znaku podwójnego to śmieć, więc na ostatniej kolumnie zostawiamy spację.
    if (much == 2 && !holds({at.x + 1, at.y})) {
        broken(at);

        cell(at) = Cell{U' ', how};

        return;
    }

    broken(at);

    if (much == 2) broken({at.x + 1, at.y});

    cell(at) = Cell{what, how};

    if (much == 2) cell({at.x + 1, at.y}) = Cell{0, how};
}

void View::string(ui::Spot at, std::string_view what, const Style& how) {
    std::size_t which = 0;

    while (which < what.size() && at.x < size_.wide) {
        const char32_t one = ::talcite::letter(what, which);

        letter(at, one, how);

        at.x += widely(one);
    }
}

void View::fill(const ui::Area& where, const Style& how) {
    for (int y = where.at.y; y < where.at.y + where.size.high; ++y)
        for (int x = where.at.x; x < where.at.x + where.size.wide; ++x) letter({x, y}, U' ', how);
}

// Cień nie ma własnej litery. Komórka zapamiętuje styl i to, że litery nie wnosi — rozstrzyga
// się to dopiero przy wklejaniu, gdy widać, co leży pod spodem. Liter zmieszać się nie da, więc
// przezroczystość znaku jest czymś innym niż przezroczystość koloru: znak albo jest, albo go nie
// ma. To dlatego cień nie potrzebuje alfy.
void View::shade(const ui::Area& where, const Style& how) {
    for (int y = where.at.y; y < where.at.y + where.size.high; ++y)
        for (int x = where.at.x; x < where.at.x + where.size.wide; ++x) {
            if (!holds({x, y})) continue;

            Cell& one = cell({x, y});

            one.style = how;
            one.shown = Shown::shade;
        }
}

// Kratka, której węzeł nie rysuje, a która mimo to leży w jego prostokącie. Zdarza się to
// zawsze, gdy kształt nie jest prostokątem: menu zajmuje pas tytułu i pudełko pod nim, a rogi
// między nimi należą do sąsiadów. Bez tego węzeł zamalowałby je spacjami.
void View::bare(const ui::Area& where) {
    for (int y = where.at.y; y < where.at.y + where.size.high; ++y)
        for (int x = where.at.x; x < where.at.x + where.size.wide; ++x) {
            if (!holds({x, y})) continue;

            cell({x, y}) = Cell{U' ', Style{}, Shown::none};
        }
}

void View::within(const View& one, ui::Spot at) {
    const int from_x = std::max(at.x, 0);
    const int from_y = std::max(at.y, 0);
    const int upto_x = std::min(at.x + one.size_.wide, size_.wide);
    const int upto_y = std::min(at.y + one.size_.high, size_.high);

    if (upto_x > from_x)
        for (int y = from_y; y < upto_y; ++y) {
            // Para znaków przeciętych brzegiem wklejenia — po obu stronach.
            broken({from_x, y});
            broken({upto_x - 1, y});

            for (int x = from_x; x < upto_x; ++x) {
                const Cell& said = one.cells_[static_cast<std::size_t>(y - at.y) *
                                                  static_cast<std::size_t>(one.size_.wide) +
                                              static_cast<std::size_t>(x - at.x)];

                // Komórka bez własnej litery zmienia tylko styl tego, co zastała — tak wygląda
                // cień: tekst spod spodu zostaje widoczny, tylko przygasa. Komórka, której węzeł
                // w ogóle nie rysował, nie zmienia niczego.
                if (said.shown == Shown::none) continue;

                if (said.shown == Shown::shade) {
                    Cell& under = cell({x, y});

                    under.style = said.style;

                    // **Cień nad pustką zostaje cieniem**, zamiast zniknąć. Kratka, na której nikt
                    // jeszcze nic nie narysował, wciąż mówi „mnie tu nie ma" — a wklejenie takie
                    // pomija, więc przyciemnienie przepadało piętro wyżej i cień pod dialogiem
                    // urywał się w połowie. Złapał to odcisk klatki przy przejściu pojemników na
                    // przezroczystość: wcześniej pustki nie było, bo każdy węzeł zamalowywał swój
                    // prostokąt, i wada nie miała jak wyjść.
                    if (under.shown == Shown::none) under.shown = Shown::shade;

                    continue;
                }

                cell({x, y}) = said;
            }

            if (cell({from_x, y}).letter == 0) cell({from_x, y}).letter = U' ';

            if (widely(cell({upto_x - 1, y}).letter) == 2) cell({upto_x - 1, y}).letter = U' ';
        }
}

Cell View::at(ui::Spot where) const {
    if (!holds(where)) return Cell{};

    return cells_[static_cast<std::size_t>(where.y) * static_cast<std::size_t>(size_.wide) +
                  static_cast<std::size_t>(where.x)];
}

}
