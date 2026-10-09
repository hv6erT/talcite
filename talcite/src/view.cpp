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

    if (!over.foreground.transparent) made.foreground = over.foreground;

    if (!over.background.transparent) made.background = over.background;

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

    while (at < what.size()) made.wide += width(utf8::decode(what, at));

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

    if (width(one.letter) == 2 && holds({where.x + 1, where.y})) {
        Cell& after = cell({where.x + 1, where.y});

        if (after.letter == 0) after.letter = U' ';
    }
}

// Każdy znak idzie przez `shown` — komórka nigdy nie niesie znaku sterującego do terminala.
void View::put(ui::Spot at, char32_t what, const Style& how) {
    if (what == 0 || !holds(at)) return;

    what = shown(what);

    const int much = width(what);

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

void View::put(ui::Spot at, std::string_view what, const Style& how) {
    std::size_t which = 0;

    while (which < what.size() && at.x < size_.wide) {
        const char32_t one = ::talcite::utf8::decode(what, which);

        put(at, one, how);

        at.x += width(one);
    }
}

void View::fill(const ui::Area& where, const Style& how) {
    for (int y = where.at.y; y < where.at.y + where.size.high; ++y)
        for (int x = where.at.x; x < where.at.x + where.size.wide; ++x) put({x, y}, U' ', how);
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

// **Sam pierwszy plan.** Kratka niesie znak i jego barwę, a tło — gdy go nie podano — bierze przy
// wklejaniu z tego, co leży pod spodem: kratka brzegu obrazu stoi w oknie, w podglądzie, na panelu,
// a tylko spód wie, jakie tam jest tło. Zwykły napis tak nie robi i robić nie ma — jego rola podaje
// własne tło (próba ramki w `framing.cpp`); kratka pierwszego planu to osobna, jawna prośba. Podane
// tło wygrywa: kratka z tłem jest zwykłą kratką.
void View::foreground(ui::Spot at, char32_t what, const Style& how) {
    put(at, what, how);

    if (!holds(at) || what == 0 || !how.background.transparent) return;

    cell(at).shown = Shown::foreground;
}

// Kratka, której węzeł nie rysuje, a która mimo to leży w jego prostokącie. Zdarza się to
// zawsze, gdy kształt nie jest prostokątem: menu zajmuje pas tytułu i pudełko pod nim, a rogi
// między nimi należą do sąsiadów. Bez tego węzeł zamalowałby je spacjami.
void View::clear(const ui::Area& where) {
    for (int y = where.at.y; y < where.at.y + where.size.high; ++y)
        for (int x = where.at.x; x < where.at.x + where.size.wide; ++x) {
            if (!holds({x, y})) continue;

            cell({x, y}) = Cell{U' ', Style{}, Shown::none};
        }
}

void View::draw(const View& one, ui::Spot at) {
    // Obrazy idą w górę razem z kratkami. Gdzie stoją, mówią kratki, nie spis — spis mówi tylko,
    // czym je narysować — więc przesuwać ani przycinać w nim nie ma czego.
    for (const Picture& one_picture : one.pictures_)
        if (std::none_of(pictures_.begin(), pictures_.end(),
                         [&one_picture](const Picture& mine) { return mine.mark == one_picture.mark; }))
            pictures_.push_back(one_picture);

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

                // Pierwszy plan nad pustką zostaje sobą i czeka piętro wyżej, jak cień; nad czymkolwiek
                // bierze jego tło i staje się zwykłą kratką.
                if (said.shown == Shown::foreground) {
                    Cell& under = cell({x, y});

                    if (under.shown == Shown::none) {
                        under = said;

                        continue;
                    }

                    const Color paper = under.style.background;

                    under = said;
                    under.style.background = paper;
                    under.shown = Shown::own;

                    continue;
                }

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

                    // Obraz w pikselach nie umie przygasnąć, więc cień go zasłania: kratka przestaje
                    // należeć do obrazu, a obraz rysuje się znakami, póki cień nie zejdzie.
                    under.picture = 0;

                    continue;
                }

                cell({x, y}) = said;
            }

            if (cell({from_x, y}).letter == 0) cell({from_x, y}).letter = U' ';

            if (width(cell({upto_x - 1, y}).letter) == 2) cell({upto_x - 1, y}).letter = U' ';
        }
}

// **Obraz w pikselach** (G1). Kratki obrazu niosą jego znak i zostają tym, czym były — znakami
// obrazu, rysowanymi jak dotąd — a spis pamięta bajty. Złożenie przenosi znak razem z kratką, więc
// to, co obraz zasłoni (okno, menu, cień, brzeg przewijania), po prostu znaku nie ma. Kratki
// przezroczyste stają się pustym pierwszym planem: tło biorą ze spodu jak dotąd, a znak nieść muszą,
// bo inaczej obraz z przezroczystym brzegiem nigdy nie byłby widoczny w całości.
void View::picture(const ui::Area& where, Picture what) {
    for (int y = where.at.y; y < where.at.y + where.size.high; ++y)
        for (int x = where.at.x; x < where.at.x + where.size.wide; ++x) {
            if (!holds({x, y})) continue;

            Cell& one = cell({x, y});

            if (one.shown == Shown::none) one = Cell{U' ', Style{}, Shown::foreground};

            one.picture = what.mark;
        }

    std::erase_if(pictures_, [&what](const Picture& one) { return one.mark == what.mark; });

    pictures_.push_back(std::move(what));
}

// Obraz widoczny **w całości** — każda kratka jego prostokąta niesie jego znak i prostokąt leży
// w `room` — dostaje piksele: jego kratki stają się puste, z tłem, które miały, i ze znakiem obrazu,
// żeby nigdy nie równały się zwykłej kratce. Dzięki temu porównanie klatek widzi każde przejście
// między obrazem a znakami i przepisuje kratki, które obraz pokrywał. Obraz zasłonięty choćby
// w jednej kratce zostaje cały w znakach, a jego kratki tracą znak — z tego samego powodu.
std::vector<Placement> View::bared(const ui::Area& room) {
    std::vector<Placement> made;

    for (const Picture& one : pictures_) {
        if (one.mark == 0 || one.size.wide <= 0 || one.size.high <= 0) continue;

        int least_x = size_.wide;
        int least_y = size_.high;
        int most_x = -1;
        int most_y = -1;
        long long many = 0;

        for (int y = 0; y < size_.high; ++y)
            for (int x = 0; x < size_.wide; ++x)
                if (cell({x, y}).picture == one.mark) {
                    least_x = std::min(least_x, x);
                    least_y = std::min(least_y, y);
                    most_x = std::max(most_x, x);
                    most_y = std::max(most_y, y);
                    ++many;
                }

        if (many == 0) continue;

        const ui::Area found{{least_x, least_y}, {most_x - least_x + 1, most_y - least_y + 1}};

        const bool whole = found.size == one.size &&
                           many == static_cast<long long>(one.size.wide) * one.size.high &&
                           found.at.x >= room.at.x && found.at.y >= room.at.y &&
                           found.at.x + found.size.wide <= room.at.x + room.size.wide &&
                           found.at.y + found.size.high <= room.at.y + room.size.high;

        for (int y = found.at.y; y < found.at.y + found.size.high; ++y)
            for (int x = found.at.x; x < found.at.x + found.size.wide; ++x) {
                Cell& here = cell({x, y});

                if (here.picture != one.mark) continue;

                if (!whole) {
                    here.picture = 0;

                    continue;
                }

                Style paper;

                paper.background = here.style.background;

                here = Cell{U' ', paper, Shown::own, one.mark};
            }

        if (whole) made.push_back(Placement{found.at, one});
    }

    return made;
}

Cell View::at(ui::Spot where) const {
    if (!holds(where)) return Cell{};

    return cells_[static_cast<std::size_t>(where.y) * static_cast<std::size_t>(size_.wide) +
                  static_cast<std::size_t>(where.x)];
}

}
