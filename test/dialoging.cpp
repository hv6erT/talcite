#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/modals.hpp"
#include "talcite/encoding.hpp"
#include "talcite/frame.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legend.hpp"
#include "talcite/root.hpp"
#include "talcite/span.hpp"

using namespace talcite;

#include "doing.hpp"

namespace {

class Box : public ui::Node {
public:
    Box(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    [[nodiscard]] bool focusable() const override { return true; }

    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

    int hit = 0;

protected:
    View paint(ui::Size given, ui::Area seen) override {
        View made{seen.size};

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                made.put({x - seen.at.x, y - seen.at.y}, what_, Style{});

        return made;
    }

    bool handle(ui::ClickEvent&) override {
        ++hit;

        return true;
    }

private:
    ui::Size wants_;
    char32_t what_ = U'?';
};

// Widżet, który sam bierze ognisko, a w środku ma chętnych — dokładnie ten kształt ma lista
// z pozycjami. Tu wystarczy sam kształt, bez listy.
class Nest : public ui::Node {
public:
    [[nodiscard]] bool focusable() const override { return true; }

    [[nodiscard]] int capacity() const override { return ui::plenty; }

    [[nodiscard]] ui::Size wants(ui::Size) const override { return {4, 2}; }
};

// To samo, ale ognisko oddaje swojemu wnętrzu, zamiast brać je na siebie.
class Deep : public Nest {
public:
    void focus() override {
        if (children().empty()) {
            Node::focus();

            return;
        }

        children().front()->focus();
    }
};

std::string read(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter != 0) talcite::utf8::encode(letter, made);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

}

TRIAL("Dialog: pudełko staje na środku, z miejscem na cień") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto under = std::make_shared<Box>(ui::Size{ui::plenty, ui::plenty}, U'x');
    const auto one = std::make_shared<ui::Dialog>();

    one->children().add(std::make_shared<Box>(ui::Size{6, 3}, U'o'));

    window->content(under);
    top.children().add(window);
    window->open(one);

    (void)top.render({{0, 0}, {20, 10}});

    // Miejsce na cień — dwie kolumny i wiersz — odejmuje się przed środkowaniem.
    same(one->children().front()->area(), (ui::Area{{6, 3}, {6, 3}}));
}

// Dialog jest wielkości ekranu, ale **przez wszystko poza pudełkiem widać treść pod spodem**.
// Świeża kartka jest wypełniona spacjami rysującego, więc bez zaczynania od pustki dialog
// zamalowałby cały ekran.
TRIAL("Dialog: poza pudełkiem widać to, co pod spodem") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto under = std::make_shared<Box>(ui::Size{ui::plenty, ui::plenty}, U'x');
    const auto one = std::make_shared<ui::Dialog>();

    one->children().add(std::make_shared<Box>(ui::Size{4, 1}, U'o'));

    window->content(under);
    top.children().add(window);
    window->open(one);

    const View made = top.render({{0, 0}, {10, 3}});

    // Ekran ma dziesięć na trzy, cień zabiera dwie kolumny i wiersz, więc pudełko cztery na jeden
    // staje w wierszu zerowym, na kolumnie drugiej. Wszędzie indziej widać treść pod spodem.
    same(read(made, 0), std::string{"xxooooxxxx"});
    same(read(made, 1), std::string{"xxxxxxxxxx"});
    same(read(made, 2), std::string{"xxxxxxxxxx"});
}

// Cień pada w prawo i w dół, przesunięty o wiersz i o dwie kolumny — ta sama reguła co pod menu.
// Litery spod spodu zostają, tylko przygasają.
TRIAL("Dialog: cień przygasza, a nie zamalowuje") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto under = std::make_shared<Box>(ui::Size{ui::plenty, ui::plenty}, U'x');
    const auto one = std::make_shared<ui::Dialog>();

    one->children().add(std::make_shared<Box>(ui::Size{6, 3}, U'o'));

    window->content(under);
    top.children().add(window);
    window->open(one);

    const View made = top.render({{0, 0}, {20, 10}});

    // Pudełko stoi na {6,3} i ma 6 na 3, więc cień z prawej idzie kolumnami 12–13, wierszami 4–5.
    ought(made.at({12, 4}).style.faint);
    ought(made.at({13, 5}).style.faint);
    same(made.at({12, 4}).letter, U'x');

    // Pod spodem: wiersz 6, kolumny 8–13.
    ought(made.at({8, 6}).style.faint);
    ought(made.at({13, 6}).style.faint);

    // Tuż nad prawym cieniem i tuż obok dolnego cienia jest już zwykła treść.
    ought(!made.at({12, 3}).style.faint);
    ought(!made.at({7, 6}).style.faint);
}

TRIAL("Dialog: bez cienia pudełko środkuje się na całym ekranie") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Dialog>(ui::ModalShape{false});

    one->children().add(std::make_shared<Box>(ui::Size{6, 4}, U'o'));

    top.children().add(window);
    window->open(one);

    (void)top.render({{0, 0}, {20, 10}});

    same(one->children().front()->area(), (ui::Area{{7, 3}, {6, 4}}));
}

// Modalność myszy: dialog jest wielkości ekranu i leży warstwę wyżej, więc kliknięcie obok
// pudełka trafia w niego, a nie w treść pod spodem.
TRIAL("Dialog: kliknięcie obok pudełka nie dochodzi pod spód") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto under = std::make_shared<Box>(ui::Size{ui::plenty, ui::plenty}, U'x');
    const auto inside = std::make_shared<Box>(ui::Size{6, 3}, U'o');
    const auto one = std::make_shared<ui::Dialog>();

    one->children().add(inside);
    window->content(under);
    top.children().add(window);
    window->open(one);

    (void)top.render({{0, 0}, {20, 10}});

    ui::ClickEvent aside{{0, 0}, ui::Mouse::left};

    ought(top.dispatch(aside));
    same(under->hit, 0);

    // Kliknięcie w pudełko dochodzi do jego treści.
    ui::ClickEvent within{{7, 4}, ui::Mouse::left};

    ought(top.dispatch(within));
    same(inside->hit, 1);
    same(under->hit, 0);
}

// Modalność klawiatury: ognisko siedzi w dialogu, więc klawisz nie ma jak dojść do legendy pod
// spodem. To ta sama droga, którą otwarte menu połyka F5.
TRIAL("Dialog: klawisz nie dochodzi do legendy pod spodem") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto legend = std::make_shared<ui::Legend>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');

    int done = 0;

    legend->content(body);
    (void)proving::doing(legend->add(ui::Code::f5, "Kopiuj"), [&done] { ++done; });
    window->content(legend);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {20, 10}});

    // Bez dialogu legenda działa.
    ui::KeyEvent first{ui::Code::f5};

    ought(top.dispatch(first));
    same(done, 1);

    const auto one = std::make_shared<ui::Dialog>();

    one->children().add(std::make_shared<Box>(ui::Size{6, 3}, U'o'));

    window->open(one);

    (void)top.render({{0, 0}, {20, 10}});

    ui::KeyEvent second{ui::Code::f5};

    ought(top.dispatch(second));
    same(done, 1);
}

// **Dialogu Escape nie zamyka.** Zamykał, dopóki brała go zasłona — czyli okno wykonywało czynność,
// której wołający nie mógł odwołać. Kto chce takie zamykanie, wiesza uchwyt; ta próba pokazuje obie
// połowy naraz, bo bez uchwytu Esc jest tu tylko połknięty.
TRIAL("Dialog: Esc zamyka dopiero z uchwytem wołającego") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');
    const auto one = std::make_shared<ui::Dialog>();

    const auto inside = std::make_shared<Box>(ui::Size{6, 3}, U'o');

    one->children().add(inside);

    window->content(body);
    top.children().add(window);

    body->focus();

    window->open(one);

    // Ognisko idzie **w głąb dialogu**, na pierwszego chętnego, a nie na sam dialog.
    same(top.focused_node(), static_cast<ui::Node*>(inside.get()));
    same(window->current(), static_cast<ui::Modal*>(one.get()));
    same(window->capacity(), 2);

    (void)top.render({{0, 0}, {20, 10}});

    ui::KeyEvent what{ui::Code::escape};

    ought(top.dispatch(what));

    // Sam Esc niczego nie zamknął — został połknięty przez modalność i tyle.
    ought(!window->empty());

    const std::weak_ptr<ui::Dialog> weak = one;

    (void)one->on<ui::KeyEvent>([weak](ui::KeyEvent& said) {
        if (said.key().code() != ui::Code::escape) return false;

        if (const auto mine = weak.lock()) mine->close();

        return true;
    });

    ought(top.dispatch(what));

    ought(window->empty());
    same(window->current(), static_cast<ui::Modal*>(nullptr));
    same(window->capacity(), 1);
    same(top.focused_node(), static_cast<ui::Node*>(body.get()));
}

// Stos: każdy dialog pamięta **swoje** ognisko, więc zamknięcie wierzchniego wraca do tego pod
// nim, a zamknięcie ostatniego — do aplikacji. Jedno pole w oknie musiałoby wybrać, które
// zapamiętać.
TRIAL("Dialog: stos wraca ognisko warstwa po warstwie") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');
    const auto one = std::make_shared<ui::Dialog>();
    const auto two = std::make_shared<ui::Dialog>();

    const auto first = std::make_shared<Box>(ui::Size{6, 3}, U'o');
    const auto second = std::make_shared<Box>(ui::Size{4, 1}, U'p');

    one->children().add(first);
    two->children().add(second);

    window->content(body);
    top.children().add(window);

    body->focus();

    window->open(one);
    window->open(two);

    same(window->current(), static_cast<ui::Modal*>(two.get()));
    same(top.focused_node(), static_cast<ui::Node*>(second.get()));
    same(static_cast<int>(window->many()), 2);

    (void)top.render({{0, 0}, {20, 10}});

    // Późniejszy leży na wierzchu.
    same(second->area(), (ui::Area{{7, 4}, {4, 1}}));

    window->close();

    same(window->current(), static_cast<ui::Modal*>(one.get()));
    same(top.focused_node(), static_cast<ui::Node*>(first.get()));

    window->close();

    ought(window->empty());
    same(top.focused_node(), static_cast<ui::Node*>(body.get()));
}

TRIAL("Dialog: zamknięcie po uchwycie zdejmuje ten, o który chodzi") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Dialog>();
    const auto two = std::make_shared<ui::Dialog>();

    one->children().add(std::make_shared<Box>(ui::Size{6, 3}, U'o'));
    two->children().add(std::make_shared<Box>(ui::Size{4, 1}, U'p'));

    top.children().add(window);

    window->open(one);
    window->open(two);

    // Zdejmujemy spodni, nie wierzchni.
    window->close(*one);

    same(static_cast<int>(window->many()), 1);
    same(window->current(), static_cast<ui::Modal*>(two.get()));

    // Zamknięcie własną ręką robi to samo.
    two->close();

    ought(window->empty());
}

TRIAL("Dialog: treść większa niż ekran przycina się do niego") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Dialog>();

    one->children().add(std::make_shared<Box>(ui::Size{ui::plenty, ui::plenty}, U'o'));

    top.children().add(window);
    window->open(one);

    (void)top.render({{0, 0}, {10, 4}});

    // Cień zabiera dwie kolumny i wiersz, więc na pudełko zostaje osiem na trzy.
    same(one->children().front()->area(), (ui::Area{{0, 0}, {8, 3}}));
}

TRIAL("Dialog: pudełkiem jest zwykła rama z tytułem") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Dialog>();
    const auto frame = std::make_shared<ui::Frame>("Uwaga");

    frame->children().add(std::make_shared<Box>(ui::Size{7, 1}, U'o'));
    one->children().add(frame);

    top.children().add(window);
    window->open(one);

    const View made = top.render({{0, 0}, {21, 6}});

    // Rama dokłada po kratce z każdej strony: dziewięć na trzy.
    same(frame->area(), (ui::Area{{5, 1}, {9, 3}}));

    ought(read(made, 1).find("Uwaga") != std::string::npos);
    ought(read(made, 2).find("ooooooo") != std::string::npos);
}

// Widżet chętny na ognisko jest dla wędrówki **liściem**: Tab staje na nim, a nie na każdym
// z jego chętnych dzieci z osobna. Bez tej reguły okno z listą stu plików miałoby sto przystanków.
TRIAL("Dialog: chętny widżet jest jednym przystankiem, a nie tyloma, ile ma dzieci") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();

    const auto first = std::make_shared<Box>(ui::Size{4, 1}, U'a');
    const auto nest = std::make_shared<Nest>();
    const auto inner = std::make_shared<Box>(ui::Size{4, 1}, U'i');
    const auto other = std::make_shared<Box>(ui::Size{4, 1}, U'j');
    const auto last = std::make_shared<Box>(ui::Size{4, 1}, U'z');

    nest->children().add(inner);
    nest->children().add(other);

    column->children().add(first);
    column->children().add(nest);
    column->children().add(last);

    one->children().add(column);

    top.children().add(window);
    window->open(one);

    same(top.focused_node(), static_cast<ui::Node*>(first.get()));

    ui::KeyEvent step{ui::Code::tab};

    ought(top.dispatch(step));
    same(top.focused_node(), static_cast<ui::Node*>(nest.get()));

    ui::KeyEvent again{ui::Code::tab};

    ought(top.dispatch(again));
    same(top.focused_node(), static_cast<ui::Node*>(last.get()));

    // Zawinięcie na koniec — i po drodze ani razu nie stanęliśmy w środku gniazda.
    ui::KeyEvent round{ui::Code::tab};

    ought(top.dispatch(round));
    same(top.focused_node(), static_cast<ui::Node*>(first.get()));
}

// Ognisko, które zeszło głębiej niż przystanek, dalej należy do przystanku: krok Tabem liczy się
// od widżetu, a nie od miejsca, w którym ognisko naprawdę siedzi.
TRIAL("Dialog: ognisko w środku widżetu liczy się jako ognisko na nim") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();

    const auto first = std::make_shared<Box>(ui::Size{4, 1}, U'a');
    const auto deep = std::make_shared<Deep>();
    const auto inner = std::make_shared<Box>(ui::Size{4, 1}, U'i');
    const auto last = std::make_shared<Box>(ui::Size{4, 1}, U'z');

    deep->children().add(inner);

    column->children().add(first);
    column->children().add(deep);
    column->children().add(last);

    one->children().add(column);

    top.children().add(window);
    window->open(one);

    same(top.focused_node(), static_cast<ui::Node*>(first.get()));

    // Krok na gniazdo stawia ognisko **w jego wnętrzu**, bo tak rozstrzyga samo gniazdo.
    ui::KeyEvent step{ui::Code::tab};

    ought(top.dispatch(step));
    same(top.focused_node(), static_cast<ui::Node*>(inner.get()));

    // Następny krok wychodzi z gniazda, a nie kręci się w nim.
    ui::KeyEvent again{ui::Code::tab};

    ought(top.dispatch(again));
    same(top.focused_node(), static_cast<ui::Node*>(last.get()));

    // Wstecz wraca do wnętrza gniazda, znów przez nie samo.
    ui::KeyEvent back{ui::Key{ui::Code::tab, ui::Mods{false, false, true}}};

    ought(top.dispatch(back));
    same(top.focused_node(), static_cast<ui::Node*>(inner.get()));
}

// Ta sama granica co w wędrówce, tylko dla klawiszy aktywnych i przycisku podstawowego: przycisk
// schowany wewnątrz widżetu chętnego na ognisko nie odpowiada z całego okna. Gdyby odpowiadał,
// litera pisana wewnątrz takiego widżetu naciskałaby coś, czego z niego nie widać.
TRIAL("Dialog: klawisz aktywny nie sięga w głąb chętnego widżetu") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto nest = std::make_shared<Nest>();

    int hidden = 0;
    int plain = 0;

    const auto inside =
        proving::doing(std::make_shared<ui::Button>("Tak"), [&hidden] { ++hidden; });
    const auto outside = proving::doing(std::make_shared<ui::Button>("Nie"), [&plain] { ++plain; });

    inside->keys().add(U'T');
    inside->primary(true);

    outside->keys().add(U'N');

    nest->children().add(inside);
    column->children().add(nest);
    column->children().add(outside);

    one->children().add(column);

    top.children().add(window);
    window->open(one);

    ui::KeyEvent held{U'T'};

    ought(top.dispatch(held));
    same(hidden, 0);

    ui::KeyEvent free{U'N'};

    ought(top.dispatch(free));
    same(plain, 1);

    // Przycisk podstawowy ma tę samą granicę — Enter nie znajduje go w środku gniazda.
    ui::KeyEvent what{ui::Code::enter};

    ought(top.dispatch(what));
    same(hidden, 0);
}

TRIAL("Dialog: pusty nie rysuje niczego i nie zasłania") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto under = std::make_shared<Box>(ui::Size{ui::plenty, ui::plenty}, U'x');
    const auto one = std::make_shared<ui::Dialog>();

    window->content(under);
    top.children().add(window);
    window->open(one);

    const View made = top.render({{0, 0}, {8, 2}});

    same(read(made, 0), std::string{"xxxxxxxx"});
    same(read(made, 1), std::string{"xxxxxxxx"});
}

// **Klawisz aktywny jest zdarzeniem, a nie pytaniem o typ.** Okno rozdaje go w głąb, więc uchwyt
// powieszony przez aplikację bierze go na równi z wbudowanym zachowaniem przycisku — a to była
// cała racja tej przebudowy: jeden mechanizm, nie dwa.
TRIAL("Dialog: uchwyt na klawisz aktywny działa jak wbudowany") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto span = std::make_shared<ui::Span>("nie jestem przyciskiem");

    column->children().add(span);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    int done = 0;

    (void)span->on<ui::ShortcutEvent>([&done](ui::ShortcutEvent& what) {
        if (what.key() != ui::Key{U'z'}) return false;

        ++done;

        return true;
    });

    ui::KeyEvent other{ui::Key{U'x'}};

    ought(top.dispatch(other));
    same(done, 0);

    ui::KeyEvent mine{ui::Key{U'z'}};

    ought(top.dispatch(mine));
    same(done, 1);
}

// Zdarzenie skierowane **nie wędruje w górę**: gdyby wędrowało, wróciłoby z dziecka do okna,
// weszło w rozdawanie po raz drugi i tak w kółko.
TRIAL("Dialog: rozdany klawisz nie wraca w górę") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto span = std::make_shared<ui::Span>("napis");

    column->children().add(span);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    int above = 0;

    (void)column->on<ui::ShortcutEvent>([&above](ui::ShortcutEvent&) {
        ++above;

        return false;
    });

    int under = 0;

    (void)span->on<ui::ShortcutEvent>([&under](ui::ShortcutEvent&) {
        ++under;

        return false;
    });

    ui::KeyEvent what{ui::Key{U'z'}};

    (void)top.dispatch(what);

    // Kolumna dostaje je raz jako ona sama, a **nie** drugi raz bąbelkiem od napisu.
    same(above, 1);
    same(under, 1);
}
