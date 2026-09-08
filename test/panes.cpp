#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/frame.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legendwindow.hpp"
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
    bool react(ui::KeyEvent&) override { return true; }

    bool react(ui::ClickEvent&) override {
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
    bool react(ui::ClickEvent&) override {
        ++clicks;

        return takes_;
    }

private:
    bool takes_ = true;
};

void clicked(ui::Root& onto, ui::Spot where) {
    ui::MouseEvent down{where, ui::Press::down, ui::Mouse::left};
    ui::MouseEvent up{where, ui::Press::up, ui::Mouse::left};

    (void)onto.deal(down);
    (void)onto.deal(up);

    onto.settle();
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

    same(static_cast<const void*>(top.focus()), static_cast<const void*>(one.get()));
    ought(one->focused());
}

TRIAL("Pane: wyłączony ogniska nie bierze") {
    Sheet top;

    const auto one = std::make_shared<ui::Pane>();

    one->able(false);

    ought(!one->focusable());

    top.children().add(one);
    (void)top.render({{0, 0}, {20, 10}});

    clicked(top, {5, 5});

    same(static_cast<const void*>(top.focus()), static_cast<const void*>(nullptr));
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
    same(static_cast<const void*>(top.focus()), static_cast<const void*>(nullptr));
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
    same(static_cast<const void*>(top.focus()), static_cast<const void*>(one.get()));
    same(top.clicks, 0);
}

// Pojemnik wyłączony jest przezroczysty **także dla myszy**: kliknięcie idzie dalej w górę, tak
// jakby go tu nie było.
TRIAL("Pane: wyłączony oddaje kliknięcie dalej w górę") {
    Sheet top;

    const auto one = std::make_shared<ui::Pane>();
    const auto said = std::make_shared<Noted>(false);

    one->able(false);
    one->children().add(said);
    top.children().add(one);
    (void)top.render({{0, 0}, {20, 10}});

    clicked(top, {5, 5});

    same(said->clicks, 1);
    same(top.clicks, 1);
    same(static_cast<const void*>(top.focus()), static_cast<const void*>(nullptr));
}

// **To jest prawdziwy powód, dla którego ten element istnieje.** Klawisz idzie bąbelkiem od ogniska
// w górę, a legenda jest przodkiem treści — więc bez czegoś, co potrzyma ognisko, `Root::deal`
// podaje klawisz korzeniowi i tam wędrówka się kończy. Okno bez przycisku i bez pola tekstowego nie
// ma wtedy działających F-klawiszy.
TRIAL("Pane: dopiero z ogniskiem legenda dostaje klawisze") {
    Sheet top;

    const auto legend = std::make_shared<ui::LegendWindow>();
    const auto one = std::make_shared<ui::Pane>();

    int hits = 0;

    one->children().add(std::make_shared<ui::Frame>("treść"));
    legend->body()->children().add(one);

    (void)proving::doing(legend->add(ui::Code::f5, "Coś"), [&hits] { ++hits; });

    top.children().add(legend);
    (void)top.render({{0, 0}, {30, 10}});

    ui::KeyEvent before{ui::Code::f5};

    (void)top.deal(before);

    same(hits, 0);

    one->focus();

    ui::KeyEvent after{ui::Code::f5};

    (void)top.deal(after);

    same(hits, 1);
}

// --- Miejsce ------------------------------------------------------------------------------------

TRIAL("Pane: domyślnie prosi o całe miejsce") {
    const auto one = std::make_shared<ui::Pane>();

    ought(one->full());

    one->children().add(std::make_shared<ui::Span>(std::string{"trzy"}));

    same(one->wants({20, 10}).wide, 20);
    same(one->wants({20, 10}).high, 10);
}

TRIAL("Pane: bez wypełniania schodzi do życzenia dziecka") {
    const auto one = std::make_shared<ui::Pane>();
    const auto said = std::make_shared<ui::Span>(std::string{"trzy"});

    one->full(false);
    one->children().add(said);

    ought(!one->full());
    same(one->wants({20, 10}).wide, said->wants({20, 10}).wide);
    same(one->wants({20, 10}).high, said->wants({20, 10}).high);
}

TRIAL("Pane: pusty i bez wypełniania nie prosi o nic") {
    const auto one = std::make_shared<ui::Pane>();

    one->full(false);

    same(one->wants({20, 10}).wide, 0);
    same(one->wants({20, 10}).high, 0);
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

    one->able(false);

    ought(!one->focused());
    ought(up->focused());
}

TRIAL("Pane: wyłączony bez chętnego przodka gasi ognisko") {
    Sheet top;

    const auto one = std::make_shared<ui::Pane>();

    top.children().add(one);

    (void)top.render({{0, 0}, {20, 10}});

    one->focus();

    one->able(false);

    same(static_cast<const void*>(top.focus()), static_cast<const void*>(nullptr));
}
