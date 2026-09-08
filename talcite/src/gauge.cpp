#include "talcite/gauge.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>

#include "talcite/loop.hpp"
#include "talcite/promise.hpp"
#include "talcite/skin.hpp"
#include "talcite/time.hpp"

namespace talcite::ui {

namespace {

// Jedno tyknięcie na dziesiątą sekundy: oko widzi ruch, a bezczynny program budzi się dziesięć
// razy na sekundę i tylko wtedy, gdy naprawdę coś się kręci.
constexpr std::chrono::milliseconds pacing{100};

// Procent zajmuje cztery kratki na `100%` i piątą na odstęp od paska.
constexpr int telling = 5;

}

Gauge::Gauge(int wide, GaugeStyle style) : style_(style), wide_(wide) { role("talcite.gauge"); }

int Gauge::takes() const { return 0; }

// Szerokość podaje wołający, tak jak w polu: pasek w oknie ma być tak szeroki, jak zdecydował
// budujący okno. Wysokość zawsze jeden wiersz.
Size Gauge::wants(Size room) const {
    return {std::min(wide_, room.wide), std::min(1, room.high)};
}

// Ułamek przycinamy **tutaj**, bo to jedyne miejsce, którym wchodzi do widżetu. Porównanie
// odwrotne łapie przy okazji NaN: nie jest większe ani równe zeru, więc idzie na zero.
void Gauge::part(double given) {
    if (!(given >= 0.0)) given = 0.0;

    part_ = std::min(given, 1.0);
}

void Gauge::step() { ++at_; }

// Przejście na nieokreślony i z powrotem. Numer obrotu rośnie przy każdej zmianie, więc tyknięcie
// wysłane przed nią nie ożywi paska, który już się zatrzymał — i dwa włączenia pod rząd nie
// puszczą dwóch łańcuchów naraz.
void Gauge::busy(bool given) {
    if (busy_ == given) return;

    busy_ = given;
    at_ = 0;

    ++turn_;

    if (busy_) beating();
    else beat_.reset();
}

// **Pasek kręci się sam, zegarem biblioteki.** `Time::later` odmierza, `main()` przenosi ciąg
// dalszy na wątek pętli, a `Loop::post` w środku pętli budzi terminal i wymusza klatkę.
//
// Dwa warunki muszą być spełnione, inaczej nie ruszamy wcale. Bez pętli nie ma na co przenieść
// ciągu dalszego, a dotknięcie drzewa węzłów z wątku puli to wyścig o dane. Bez właściciela —
// czyli gdy nikt nie trzyma paska wskaźnikiem dzielonym — słaby wskaźnik jest od razu wygasły
// i tyknięcie nie miałoby czego obudzić; w drzewie tak nie bywa, bo `Node::add` bierze wskaźnik
// dzielony, ale pasek na stosie w próbie tak właśnie wygląda i ma prawo po prostu nie chodzić.
void Gauge::beating() {
    if (Loop::current() == nullptr) return;

    const std::weak_ptr<Node> weak = weak_from_this();

    if (weak.expired()) return;

    const int mine = turn_;

    // **Obietnicę trzeba trzymać.** Ogniwo wisi na obietnicy słabym wskazaniem, więc porzucone
    // `then(...).go()` po prostu nigdy nie zadziała — to jest ta sama reguła, dzięki której
    // śmierć właściciela odcina wywołania i dzięki której nie ma metody `clear()`. Typ chowamy
    // za `shared_ptr<void>`, żeby nagłówek widżetu nie wciągał całej maszynerii obietnic; tę samą
    // sztuczkę robi u siebie `Carry`.
    //
    // Podmiana w środku wywołania zwrotnego jest bezpieczna: `Carry` na czas przeniesienia na
    // pętlę trzyma sam siebie, więc puszczenie naszego uchwytu nie zabija kodu, który właśnie
    // biegnie.
    beat_ = std::make_shared<Promise<void>>(Time::later(pacing).main().then([weak, mine] {
        const std::shared_ptr<Node> one = weak.lock();

        if (!one) return;

        Gauge& gauge = static_cast<Gauge&>(*one);

        if (!gauge.busy_ || gauge.turn_ != mine) return;

        gauge.step();
        gauge.beating();
    }));

    std::static_pointer_cast<Promise<void>>(beat_)->go();
}

View Gauge::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — reguła opisana przy `Node::paint`.

    made.bare({{0, 0}, seen.size});

    if (given.wide <= 0 || given.high <= 0) return made;

    const Skin& how = skin();

    const Style look = how.get<Style>(role());
    const Style full = how.get<Style>(role() + ".done");

    const char32_t block = how.get<char32_t>(role() + ".done");
    const char32_t empty = how.get<char32_t>(role() + ".rest");

    // Procent odpada pierwszy, gdy jest ciasno, a klamry drugie — pasek na jedną kratkę dalej
    // coś mówi, sama klamra nie mówi nic.
    const bool told = style_.percent && given.wide >= telling + 3;
    const int room = told ? given.wide - telling : given.wide;

    const bool boxed = room >= 3;
    const int bar = boxed ? room - 2 : room;
    const int from = boxed ? 1 : 0;

    if (boxed) {
        made.letter({-seen.at.x, -seen.at.y}, how.get<char32_t>(role() + ".left"), look);
        made.letter({room - 1 - seen.at.x, -seen.at.y}, how.get<char32_t>(role() + ".right"),
                    look);
    }

    if (bar <= 0) return made;

    // Nieokreślony: kostka jedzie tam i z powrotem. Odbicie zamiast zawinięcia, bo skok z prawej
    // krawędzi na lewą czyta się jak mrugnięcie, a nie jak ruch.
    if (busy_) {
        const int much = std::max(bar / 5, 1);
        const int travel = bar - much;
        const int span = 2 * travel;
        const int which = span > 0 ? at_ % span : 0;
        const int at = which <= travel ? which : span - which;

        for (int x = 0; x < bar; ++x)
            made.letter({from + x - seen.at.x, -seen.at.y},
                        x >= at && x < at + much ? block : empty,
                        x >= at && x < at + much ? full : look);

        return made;
    }

    // **Pełne dopiero po skończeniu.** Zaokrąglamy, ale setki i pełnego paska nie pokazujemy,
    // dopóki ułamek nie jest dokładnie jedynką — a że obie liczby idą tą samą regułą, pasek
    // i procent nigdy nie mówią czegoś innego.
    int done = static_cast<int>(std::lround(part_ * bar));

    if (done >= bar && part_ < 1.0) done = bar - 1;

    for (int x = 0; x < bar; ++x)
        made.letter({from + x - seen.at.x, -seen.at.y}, x < done ? block : empty,
                    x < done ? full : look);

    if (!told) return made;

    int shown = static_cast<int>(std::lround(part_ * 100.0));

    if (shown >= 100 && part_ < 1.0) shown = 99;

    std::string said = std::to_string(shown) + "%";

    made.string({given.wide - static_cast<int>(said.size()) - seen.at.x, -seen.at.y}, said,
                how.get<Style>(role() + ".text"));

    return made;
}

}
