#include "proving.hpp"

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>

#include "talcite/encoding.hpp"
#include "talcite/gauge.hpp"
#include "talcite/loop.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"
#include "talcite/time.hpp"

using namespace talcite;

namespace {

std::string read(const View& one) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, 0}).letter;

        if (letter != 0) talcite::utf8::encode(letter, made);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

std::string drawn(ui::Gauge& one, int wide) { return read(one.render({{0, 0}, {wide, 1}})); }

}

TRIAL("Gauge: chce tyle, ile mu kazano, i zawsze jeden wiersz") {
    ui::Gauge one{20};

    same(one.wants({40, 3}), (ui::Size{20, 3 > 1 ? 1 : 3}));
    same(one.wants({12, 3}), (ui::Size{12, 1}));
    same(one.wants({40, 0}), (ui::Size{20, 0}));
    same(one.capacity(), 0);
}

// Ułamek przycinamy w jednym miejscu — tam, gdzie wchodzi. Porównanie odwrotne łapie przy okazji
// NaN, który inaczej przeszedłby przez każdy zwykły warunek.
TRIAL("Gauge: ułamek przycina się do zera i jedynki") {
    ui::Gauge one{20};

    one.part(-3.0);
    same(one.part(), 0.0);

    one.part(7.5);
    same(one.part(), 1.0);

    one.part(std::nan(""));
    same(one.part(), 0.0);

    one.part(0.25);
    same(one.part(), 0.25);
}

TRIAL("Gauge: pasek, klamry i procent z prawej") {
    ui::Gauge one{20};

    same(drawn(one, 20), std::string{"[░░░░░░░░░░░░░]   0%"});

    one.part(0.5);

    same(drawn(one, 20), std::string{"[███████░░░░░░]  50%"});

    one.part(1.0);

    same(drawn(one, 20), std::string{"[█████████████] 100%"});
}

// **Pełne dopiero po skończeniu.** Obie liczby idą tą samą regułą, więc pasek i procent nigdy nie
// mówią czegoś innego — a `100%` znaczy „zrobione", a nie „prawie".
TRIAL("Gauge: sto procent tylko przy dokładnej jedynce") {
    ui::Gauge one{20};

    one.part(0.999);

    same(drawn(one, 20), std::string{"[████████████░]  99%"});

    one.part(1.0);

    same(drawn(one, 20), std::string{"[█████████████] 100%"});
}

TRIAL("Gauge: ciasno odpada najpierw procent, potem klamry") {
    ui::Gauge one{20};

    one.part(0.5);

    same(drawn(one, 12), std::string{"[███░░]  50%"});
    same(drawn(one, 8), std::string{"[░]  50%"});

    // Poniżej ośmiu kratek procent nie ma się gdzie zmieścić.
    same(drawn(one, 7), std::string{"[███░░]"});
    same(drawn(one, 3), std::string{"[░]"});

    // Poniżej trzech nie ma miejsca na klamry, ale pasek dalej coś mówi.
    same(drawn(one, 2), std::string{"█░"});
    same(drawn(one, 1), std::string{"░"});
    same(drawn(one, 0), std::string{});
}

TRIAL("Gauge: bez procentu cała szerokość idzie na pasek") {
    ui::Gauge one{20};

    one.percent(false);
    one.part(0.5);

    same(drawn(one, 20), std::string{"[█████████░░░░░░░░░]"});
}

// Nieokreślony: kostka jedzie tam i z powrotem, a procentu nie ma — bo nie ma czego pokazać.
// Ruch napędza `step()`, więc próba nie musi czekać na zegar.
TRIAL("Gauge: nieokreślony jedzie i odbija się od krawędzi") {
    ui::Gauge one{20};

    one.part(0.5);
    one.busy(true);

    ought(one.busy());

    same(drawn(one, 20), std::string{"[██░░░░░░░░░░░]"});

    one.step();

    same(drawn(one, 20), std::string{"[░██░░░░░░░░░░]"});

    // Jedenaście kroków dowozi kostkę do prawej krawędzi.
    for (int which = 1; which < 11; ++which) one.step();

    same(drawn(one, 20), std::string{"[░░░░░░░░░░░██]"});

    one.step();

    same(drawn(one, 20), std::string{"[░░░░░░░░░░██░]"});
}

TRIAL("Gauge: powrót z nieokreślonego pokazuje zapamiętany ułamek") {
    ui::Gauge one{20};

    one.part(0.5);
    one.busy(true);
    one.step();
    one.step();

    one.busy(false);

    ought(!one.busy());
    same(one.part(), 0.5);
    same(drawn(one, 20), std::string{"[███████░░░░░░]  50%"});

    // Ponowne włączenie zaczyna od lewej, a nie od miejsca, w którym stanęło.
    one.busy(true);

    same(drawn(one, 20), std::string{"[██░░░░░░░░░░░]"});
}

// Pasek bez właściciela i bez pętli po prostu nie chodzi sam — i nie wywraca się przy tym.
TRIAL("Gauge: nieokreślony bez pętli nie rusza zegara i nie psuje się") {
    ui::Gauge one{20};

    one.busy(true);
    one.busy(false);
    one.busy(true);

    same(drawn(one, 20), std::string{"[██░░░░░░░░░░░]"});
}

// **Pasek kręci się sam.** `Time::later` odmierza, `here()` przenosi ciąg dalszy na wątek pętli,
// a `Loop::turn()` go wykonuje — tu w próbie ręcznie, w programie robi to `Term`.
TRIAL("Gauge: nieokreślony kręci się sam zegarem biblioteki") {
    Loop loop;

    const auto one = std::make_shared<ui::Gauge>(20);

    one->busy(true);

    const std::string was = drawn(*one, 20);

    Stopwatch began;

    while (began.spent() < std::chrono::seconds{3}) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});

        (void)loop.turn();

        if (drawn(*one, 20) != was) break;
    }

    ought(drawn(*one, 20) != was);

    // Zatrzymany przestaje tykać: po zgaszeniu pętla nie dostaje już nic do zrobienia.
    one->busy(false);

    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    (void)loop.turn();

    std::this_thread::sleep_for(std::chrono::milliseconds{300});

    same(loop.turn(), std::size_t{0});
}

// Wskaźnik miał sześć kluczy skórki wpisanych na sztywno — najwięcej w całej bibliotece. Teraz
// wszystkie wyrastają z jednego rdzenia, jak wszędzie indziej.
TRIAL("Gauge: rdzeń roli można podmienić") {
    ui::Root top;

    const auto one = std::make_shared<ui::Gauge>(8);

    same(one->role(), std::string{"talcite.gauge"});

    one->part(0.0);

    top.children().add(one);

    ui::Styles these;

    Style look{};

    look.foreground = Color{false, 4, 4, 4};

    these["moj.wskaznik"] = look;

    top.skin(ui::Skin{these});
    one->role("moj.wskaznik");

    const View made = top.render({{0, 0}, {8, 1}});

    same(made.at({1, 0}).style.foreground, (Color{false, 4, 4, 4}));
}
