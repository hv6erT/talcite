#include "talcite/box.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace talcite::ui {

// **Węzeł, który rozmiar zadaje, zamiast go negocjować.** Dotąd robiło to pięć różnych rzeczy:
// `List` dwiema liczbami z `plenty` jako brakiem granicy, `Pane` wartością logiczną `full`,
// `Capped` w `message.cpp` zwężaniem pytania, `Frame` dodawaniem grubości krawędzi — a do tego
// **dziewięć prób** trzymało własną atrapę o tej samej nazwie i tym samym ciele (`menuing`,
// `legending`, `layering`, `layout`, `clicking`, `buttoning`, `rows`, `framing`, `dialoging`).
// Nazwa nie jest więc wymyślona, tylko zastana.
//
// **Życzeniem jest funkcja, nie liczba.** Liczba nie umie powiedzieć „najwyżej osiemdziesiąt
// kolumn, ale zostaw po osiem z każdej strony" — a to jest zwyczajny sposób wymierzania okna,
// nie osobliwość. Funkcja dostaje pokój i liczy, więc stały rozmiar jest jej podprzypadkiem;
// odwrotnie się nie da. Koszt zmierzony, nie oszacowany: `std::function` z funkcją w osobnej
// jednostce przekładu wypada tyle samo, co odczyt pola — różnica ginie w szumie przy dwudziestu
// milionach wywołań, a `wants` woła się rzędu setek razy na klatkę.
//
// **Drugiego trybu nie ma, bo robi go brak `Box`-a.** „Chcę tyle, ile chce dziecko" to domyślne
// `Node::wants`, więc kto tego chce, po prostu nie owija. Dlatego nie ma tu wyliczenia trybów ani
// pytania dziecka: `Box` **nie pyta go wcale**. Gdyby pytał, byłby tym samym, co `Frame` bez ramki,
// a zadany rozmiar przestałby być zadany.
//
// **Pokój to nie ekran.** Funkcja dostaje tyle, ile rodzic jej zostawił — a `Modal::placed` odejmuje
// miejsce na cień, dwie kolumny i wiersz, **zanim** zapyta dziecko o życzenie. „Zostaw szesnaście"
// znaczy więc „szesnaście z tego, co dostałem", nie „z ekranu"; kto przepisuje rachunek liczony od
// szerokości terminala, dokłada sobie te dwie kolumny i ten wiersz we własnej funkcji. Osobnych
// rodzajów pozycjonowania to nie wymaga: różnica jest stała i znana, a pozycjonowanie jest
// odrębnym pytaniem od rozmiaru — tym zajmuje się `Modal::placed`.
// Stała jako podprzypadek funkcji: **drugie wejście, nie drugi stan**. Pole zostaje jedno, więc
// `wants` nie ma czego rozgałęziać, a `Box{{68, 20}}` czyta się lepiej niż domknięcie oddające
// dwie liczby — i to będzie najczęstszy przypadek.
//
// `plenty` nie wymaga tu osobnej gałęzi: życzenie idzie dalej takie, jakie jest, a przycięcie
// w `wants` zamienia je na cały pokój. Rozpoznawanie go w tym miejscu byłoby drugą prawdą o tym,
// co znaczy brak granicy.
Box::Box(Size given) : wish_([given](Size) { return given; }) {}

// Pusta funkcja rzuciłaby `bad_function_call` z wnętrza układu, przy pierwszej klatce i w miejscu,
// które o niczym nie mówi. Odrzucamy ją tam, gdzie powstała.
Box::Box(Wish wish) : wish_(std::move(wish)) {
    if (!wish_) throw std::logic_error("talcite::ui::Box: the wish is empty");
}

// Przycięcie zostaje **tutaj**, choć funkcja mogłaby je zrobić sama — bo inaczej każdy, kto ją
// pisze, musiałby o nim pamiętać, a zapomnienie oddałoby rodzicowi życzenie większe niż miejsce,
// czyli kłamstwo wobec tego, kto je potem rozdziela.
//
// Od dołu do zera nie jest ostrożnością na wyrost: pokój bywa ujemny sam z siebie, bo rodzic
// odejmuje krawędzie i marginesy od tego, co ma. Ujemne życzenie ma się zatrzymać w miejscu,
// w którym powstało, zamiast wędrować w górę i mnożyć się w rachunkach układu.
Size Box::wants(Size room) const {
    const Size wish = wish_(room);

    return {std::max(std::min(wish.wide, room.wide), 0),
            std::max(std::min(wish.high, room.high), 0)};
}

}
