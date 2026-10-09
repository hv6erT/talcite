#include "proving.hpp"

#include <chrono>
#include <memory>
#include <string>

#include "talcite/frame.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legend.hpp"
#include "talcite/pane.hpp"
#include "talcite/root.hpp"
#include "talcite/span.hpp"

using namespace talcite;

#include "doing.hpp"

namespace {

class Sheet : public ui::Root {
public:
    int clicks = 0;

protected:
    // Korzeń pochłania każdy klawisz — tak jak w każdym demie i tak jak zrobi to aplikacja.
    // Dzięki temu widać, że legenda dostaje swoje **od dołu**, a nie stąd.
    bool handle(ui::KeyEvent&) override { return true; }

    bool handle(ui::ClickEvent&) override {
        ++clicks;

        return true;
    }
};

// Dziecko liczące kliknięcia i mówiące, czy je bierze. Zajmuje całe miejsce, więc **stoi bliżej
// kursora niż pojemnik** — a to jest sedno wędrówki myszy: zdarzenie zaczyna się u najgłębszego
// węzła pod kursorem i idzie w górę, więc pojemnik widzi tylko to, czego dziecko nie wzięło.
class Noted : public ui::Node {
public:
    explicit Noted(bool takes) : takes_(takes) {}

    int clicks = 0;

    [[nodiscard]] ui::Size wants(ui::Size room) const override { return room; }

protected:
    bool handle(ui::ClickEvent&) override {
        ++clicks;

        return takes_;
    }

private:
    bool takes_ = true;
};

// Każde kliknięcie o sekundę po poprzednim, więc dwa z rzędu w tę samą kratkę nie składają się
// w dwuklik — próby pytają o pojedyncze.
void clicked(ui::Root& onto, ui::Spot where) {
    static std::chrono::steady_clock::time_point moment{};

    moment += std::chrono::seconds{1};

    ui::MouseEvent down{where, ui::Press::down, ui::Mouse::left, {}, moment};
    ui::MouseEvent up{where, ui::Press::up, ui::Mouse::left, {}, moment};

    (void)onto.dispatch(down);
    (void)onto.dispatch(up);
}

}

// --- Ognisko ------------------------------------------------------------------------------------

TRIAL("Pane: domyślnie bierze ognisko, bo po to jest") {
    Sheet top;

    const auto one = std::make_shared<ui::Pane>();

    ought(one->focusable());

    top.children().add(one);
    (void)top.render({{0, 0}, {20, 10}});

    clicked(top, {5, 5});

    same(static_cast<const void*>(top.focused_node()), static_cast<const void*>(one.get()));
    ought(one->focused());
}

TRIAL("Pane: wyłączony ogniska nie bierze") {
    Sheet top;

    const auto one = std::make_shared<ui::Pane>();

    one->enabled(false);

    ought(!one->focusable());

    top.children().add(one);
    (void)top.render({{0, 0}, {20, 10}});

    clicked(top, {5, 5});

    same(static_cast<const void*>(top.focused_node()), static_cast<const void*>(nullptr));
}

// **Pojemnik nie zabiera kliknięć swoim dzieciom.** Mysz wędruje od najgłębszego węzła pod kursorem
// w górę, więc dziecko jest pierwsze; kto je weźmie, ten kończy wędrówkę. Pojemnik dostaje tylko
// to, czego nikt niżej nie chciał — i wtedy dopiero bierze ognisko.
TRIAL("Pane: kliknięcie wzięte przez dziecko do pojemnika nie dochodzi") {
    Sheet top;

    const auto one = std::make_shared<ui::Pane>();
    const auto said = std::make_shared<Noted>(true);

    one->children().add(said);
    top.children().add(one);
    (void)top.render({{0, 0}, {20, 10}});

    clicked(top, {5, 5});

    same(said->clicks, 1);
    same(static_cast<const void*>(top.focused_node()), static_cast<const void*>(nullptr));
    same(top.clicks, 0);
}

TRIAL("Pane: kliknięcie odrzucone przez dziecko zabiera ognisko pojemnikowi") {
    Sheet top;

    const auto one = std::make_shared<ui::Pane>();
    const auto said = std::make_shared<Noted>(false);

    one->children().add(said);
    top.children().add(one);
    (void)top.render({{0, 0}, {20, 10}});

    clicked(top, {5, 5});

    same(said->clicks, 1);
    same(static_cast<const void*>(top.focused_node()), static_cast<const void*>(one.get()));
    same(top.clicks, 0);
}

// Pojemnik wyłączony jest przezroczysty **także dla myszy**: kliknięcie idzie dalej w górę, tak
// jakby go tu nie było.
TRIAL("Pane: wyłączony oddaje kliknięcie dalej w górę") {
    Sheet top;

    const auto one = std::make_shared<ui::Pane>();
    const auto said = std::make_shared<Noted>(false);

    one->enabled(false);
    one->children().add(said);
    top.children().add(one);
    (void)top.render({{0, 0}, {20, 10}});

    clicked(top, {5, 5});

    same(said->clicks, 1);
    same(top.clicks, 1);
    same(static_cast<const void*>(top.focused_node()), static_cast<const void*>(nullptr));
}

// **To jest prawdziwy powód, dla którego ten element istnieje.** Klawisz idzie bąbelkiem od ogniska
// w górę, a legenda jest przodkiem treści — więc bez czegoś, co potrzyma ognisko, `Root::dispatch`
// podaje klawisz korzeniowi i tam wędrówka się kończy. Okno bez przycisku i bez pola tekstowego nie
// ma wtedy działających F-klawiszy.
TRIAL("Pane: dopiero z ogniskiem legenda dostaje klawisze") {
    Sheet top;

    const auto legend = std::make_shared<ui::Legend>();
    const auto one = std::make_shared<ui::Pane>();

    int hits = 0;

    one->children().add(std::make_shared<ui::Frame>("treść"));
    legend->content(one);

    (void)proving::doing(legend->add(ui::Code::f5, "Coś"), [&hits] { ++hits; });

    top.children().add(legend);
    (void)top.render({{0, 0}, {30, 10}});

    ui::KeyEvent before{ui::Code::f5};

    (void)top.dispatch(before);

    same(hits, 0);

    one->focus();

    ui::KeyEvent after{ui::Code::f5};

    (void)top.dispatch(after);

    same(hits, 1);
}

// --- Miejsce ------------------------------------------------------------------------------------

// **Cały pokój, także gdy dziecko chce mniej** — i to jest cały rozmiarowy udział pojemnika.
// Stała, nie ustawienie: pole `full_` z parą ustawiaczy nie miało ani jednego wołającego poza
// próbami, które sprawdzały je same. Od zadawania rozmiaru jest `Box`.
TRIAL("Pane: prosi o całe miejsce, choćby dziecko chciało mniej") {
    const auto one = std::make_shared<ui::Pane>();
    const auto said = std::make_shared<ui::Span>(std::string{"trzy"});

    one->children().add(said);

    ought(said->wants({20, 10}).wide < 20);

    same(one->wants({20, 10}).wide, 20);
    same(one->wants({20, 10}).high, 10);
}

// Pusty też bierze wszystko — inaczej niż `Node`, który bez dziecka nie prosi o nic.
TRIAL("Pane: pusty prosi o całe miejsce") {
    const auto one = std::make_shared<ui::Pane>();

    same(one->wants({20, 10}).wide, 20);
    same(one->wants({20, 10}).high, 10);
}

// --- Malowanie ----------------------------------------------------------------------------------

// Pojemnik **nie maluje nic od siebie** — dziedziczy przezroczyste przepuszczenie dziecka i tym
// samym regułę „maluj to, czym jesteś, a nie prostokąt, który dostałeś".
TRIAL("Pane: pusty nie zostawia śladu na kartce") {
    Sheet top;

    const auto one = std::make_shared<ui::Pane>();

    top.children().add(one);

    const View made = top.render({{0, 0}, {6, 3}});

    bool held = true;

    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 6; ++x) held = held && made.at({x, y}).shown == Shown::none;

    ought(held);
}

TRIAL("Pane: dziecko dostaje cały przydzielony prostokąt") {
    Sheet top;

    const auto one = std::make_shared<ui::Pane>();
    const auto said = std::make_shared<ui::Span>(std::string{"abc"});

    one->children().add(said);
    top.children().add(one);

    const View made = top.render({{0, 0}, {6, 3}});

    same(made.at({0, 0}).letter, U'a');
    same(made.at({2, 0}).letter, U'c');
}

// **Wyłączenie oddaje ognisko**, tak jak w przycisku, polu i wierszu listy — pojemnik był jedynym,
// który zostawał z ogniskiem na węźle mówiącym o sobie, że chętny nie jest.
TRIAL("Pane: wyłączony pod ogniskiem oddaje je przodkowi") {
    Sheet top;

    const auto up = std::make_shared<ui::Pane>();
    const auto one = std::make_shared<ui::Pane>();

    up->children().add(one);
    top.children().add(up);

    (void)top.render({{0, 0}, {20, 10}});

    one->focus();

    ought(one->focused());

    one->enabled(false);

    ought(!one->focused());
    ought(up->focused());
}

TRIAL("Pane: wyłączony bez chętnego przodka gasi ognisko") {
    Sheet top;

    const auto one = std::make_shared<ui::Pane>();

    top.children().add(one);

    (void)top.render({{0, 0}, {20, 10}});

    one->focus();

    one->enabled(false);

    same(static_cast<const void*>(top.focused_node()), static_cast<const void*>(nullptr));
}
