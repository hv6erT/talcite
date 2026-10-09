#include "proving.hpp"

#include <algorithm>
#include <memory>
#include <string>

#include "talcite/box.hpp"
#include "talcite/dialog.hpp"
#include "talcite/root.hpp"
#include "talcite/modals.hpp"

using namespace talcite;

#include "doing.hpp"

namespace {

// Dziecko o znanym życzeniu, mniejszym albo większym od pokoju — po to, żeby próba mogła pokazać,
// że `Box` go **nie pyta**. Bez takiego dziecka „nie pyta" i „pyta, ale wychodzi to samo" byłyby
// nierozróżnialne.
class Wanting : public ui::Node {
public:
    explicit Wanting(ui::Size given) : wants_(given) {}

    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

private:
    ui::Size wants_;
};

}

// --- Rozmiar ------------------------------------------------------------------------------------

// Nieustawiony rozmiar znaczy **maksimum dostępnego**, a nie zero i nie życzenie dziecka. `plenty`
// to ta sama nieskończoność, której używa `List` — nie druga obok niej.
TRIAL("Box: bez rozmiaru prosi o cały pokój") {
    const auto one = std::make_shared<ui::Box>();

    same(one->wants({20, 10}).wide, 20);
    same(one->wants({20, 10}).high, 10);
}

TRIAL("Box: zadany rozmiar jest tym, o co prosi") {
    const auto one = std::make_shared<ui::Box>(ui::Size{12, 4});

    same(one->wants({20, 10}).wide, 12);
    same(one->wants({20, 10}).high, 4);
}

// Przycięcie do pokoju, bo życzenie większe niż miejsce nie jest życzeniem, tylko kłamstwem wobec
// rodzica, który je potem rozdziela.
TRIAL("Box: rozmiar większy od pokoju schodzi do pokoju") {
    const auto one = std::make_shared<ui::Box>(ui::Size{200, 50});

    same(one->wants({20, 10}).wide, 20);
    same(one->wants({20, 10}).high, 10);
}

// Osie są niezależne: `plenty` w jednej znaczy „cały pokój w tej osi", a druga trzyma swoją
// granicę. Tak wymierza się pasek, kolumna i każde okno wysokie na całą wysokość.
TRIAL("Box: plenty w jednej osi nie rusza drugiej") {
    const auto one = std::make_shared<ui::Box>(ui::Size{12, ui::plenty});

    same(one->wants({20, 10}).wide, 12);
    same(one->wants({20, 10}).high, 10);
}

// Pokój bywa ujemny: rodzic odejmuje krawędzie i marginesy od tego, co ma, i przy ciasnym ekranie
// wychodzi poniżej zera. Ujemne życzenie ma się zatrzymać tutaj, a nie wędrować w górę do rachunków
// układu.
TRIAL("Box: ujemny pokój oddaje zero, nie liczbę ujemną") {
    const auto one = std::make_shared<ui::Box>(ui::Size{12, 4});

    same(one->wants({-3, -1}).wide, 0);
    same(one->wants({-3, -1}).high, 0);
}

// --- Dziecko ------------------------------------------------------------------------------------

// **`Box` dziecka nie pyta wcale** — i to jest jego sens, bo inaczej byłby `Frame` bez ramki,
// a zadany rozmiar przestałby być zadany. Próba pokazuje to z obu stron: przy dziecku łakomszym
// i przy dziecku skromniejszym od zadanej liczby.
TRIAL("Box: życzenie dziecka niczego nie zmienia") {
    const auto small = std::make_shared<ui::Box>(ui::Size{12, 4});
    const auto big = std::make_shared<ui::Box>(ui::Size{12, 4});

    small->children().add(std::make_shared<Wanting>(ui::Size{2, 1}));
    big->children().add(std::make_shared<Wanting>(ui::Size{40, 40}));

    same(small->wants({20, 10}).wide, 12);
    same(small->wants({20, 10}).high, 4);

    same(big->wants({20, 10}).wide, 12);
    same(big->wants({20, 10}).high, 4);
}

// Drugiego trybu nie ma, bo robi go **brak** `Box`-a: zwykły `Node` schodzi do życzenia dziecka.
// Próba stoi tu, a nie przy `Node`, bo to jest ta połowa rozstrzygnięcia, przez którą `Box` nie ma
// wyliczenia trybów.
TRIAL("Box: sam Node dalej schodzi do życzenia dziecka") {
    const auto one = std::make_shared<ui::Node>();

    one->children().add(std::make_shared<Wanting>(ui::Size{2, 1}));

    same(one->wants({20, 10}).wide, 2);
    same(one->wants({20, 10}).high, 1);
}

// --- Życzenie jako funkcja -----------------------------------------------------------------------

// **Po to życzeniem jest funkcja, a nie liczba.** „Najwyżej osiemdziesiąt kolumn, ale zostaw po
// osiem z każdej strony" to zwyczajny sposób wymierzania okna — i liczbą się go nie zapisze, bo
// druga połowa zależy od tego, ile miejsca w ogóle jest. Rachunek z `help.c` Midnight Commandera,
// przeniesiony wprost.
TRIAL("Box: życzenie liczone z pokoju") {
    const auto one = std::make_shared<ui::Box>(
        ui::Box::Wish{[](ui::Size room) { return ui::Size{std::min(80, room.wide - 16), 20}; }});

    // Szeroki pokój: górna granica trzyma.
    same(one->wants({200, 40}).wide, 80);

    // Ciasny: zostaje odstęp od krawędzi, którego stała liczba nie umiałaby zachować.
    same(one->wants({60, 40}).wide, 44);
    same(one->wants({30, 40}).wide, 14);
}

// Stała jest podprzypadkiem funkcji i dlatego oba konstruktory dają **jedno** pole — `wants` nie
// ma czego rozgałęziać. Próba pilnuje, że zapisy są wymienne.
TRIAL("Box: stała i funkcja oddająca tę samą stałą są nie do odróżnienia") {
    const auto flat = std::make_shared<ui::Box>(ui::Size{12, 4});
    const auto deep = std::make_shared<ui::Box>(
        ui::Box::Wish{[](ui::Size) { return ui::Size{12, 4}; }});

    for (const ui::Size room : {ui::Size{20, 10}, ui::Size{6, 2}, ui::Size{-3, -1}}) {
        same(flat->wants(room).wide, deep->wants(room).wide);
        same(flat->wants(room).high, deep->wants(room).high);
    }
}

// Przycięcie zostaje w `Box`, nie w funkcji — bo inaczej każdy, kto ją pisze, musiałby o nim
// pamiętać, a zapomnienie oddałoby rodzicowi życzenie większe niż miejsce.
TRIAL("Box: funkcja łakoma i tak nie wyjdzie poza pokój") {
    const auto one = std::make_shared<ui::Box>(
        ui::Box::Wish{[](ui::Size) { return ui::Size{1000, 1000}; }});

    same(one->wants({20, 10}).wide, 20);
    same(one->wants({20, 10}).high, 10);
}

// Pusta funkcja rzuciłaby `bad_function_call` przy pierwszej klatce, z wnętrza układu i w miejscu,
// które o niczym nie mówi. Odrzucamy ją tam, gdzie powstała.
TRIAL("Box: pustego życzenia nie przyjmuje") {
    threw((void)std::make_shared<ui::Box>(ui::Box::Wish{}));
}

// --- W drzewie ----------------------------------------------------------------------------------

// **Po to on jest.** Zasłona liczy pudełko z życzenia swojego jedynego dziecka i stawia je na
// środku, więc treść łakoma — a taka jest każda strona, tabela i panel — rozdymała okno na cały
// ekran. Próba pokazuje obie strony tego samego: **bez** `Box`-a okno zajmuje wszystko, **z** nim
// stoi na zadanej liczbie i zasłona ma co wyśrodkować.
TRIAL("Box: w zasłonie zatrzymuje łakomą treść na zadanym rozmiarze") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Dialog>();
    const auto box = std::make_shared<ui::Box>(ui::Size{10, 4});

    box->children().add(std::make_shared<Wanting>(ui::Size{ui::plenty, ui::plenty}));
    one->children().add(box);

    top.children().add(window);
    window->open(one);

    (void)top.render({{0, 0}, {30, 12}});

    // Miejsce na cień — dwie kolumny i wiersz — odejmuje się przed środkowaniem, więc pudełko
    // środkuje się w 28 na 11: (28−10)/2 = 9 i (11−4)/2 = 3.
    same(one->children().front()->area(), (ui::Area{{9, 3}, {10, 4}}));
}

TRIAL("Box: bez niego ta sama treść rozdyma okno na cały ekran") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Dialog>();

    one->children().add(std::make_shared<Wanting>(ui::Size{ui::plenty, ui::plenty}));

    top.children().add(window);
    window->open(one);

    (void)top.render({{0, 0}, {30, 12}});

    same(one->children().front()->area(), (ui::Area{{0, 0}, {28, 11}}));
}
