#include "proving.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "talcite/encoding.hpp"
#include "talcite/menu.hpp"
#include "talcite/menuitem.hpp"
#include "talcite/menuseparator.hpp"
#include "talcite/menubar.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"

using namespace talcite;

#include "doing.hpp"

namespace {

// Rozwinięte menu paska. To pytanie próby, nie biblioteki: pasek mówi, które menu jest bieżące,
// a menu — czy jest rozwinięte. Osobne `opened()` na pasku było drugą drogą do tej samej odpowiedzi.
ui::Menu* unfolded(const ui::Menubar& bar) {
    ui::Menu* one = bar.current();

    return one != nullptr && one->opened() ? one : nullptr;
}

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

TRIAL("Menu: zamknięte to sam tytuł w wierszu zerowym") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    top.children().add(window);

    const View made = top.render({{0, 0}, {20, 6}});

    same(read(made, 0), std::string{" Plik"});
    same(read(made, 1), std::string{""});

    // Zwinięte menu jest **poza drzewem** — nie ma rodzica ani prostokąta. To, co widać w wierszu
    // zerowym, to jego tytuł, osobny węzeł w rzędzie tytułów.
    ought(!one->opened());
    ought(one->parent() == nullptr);
}

// Opróżnienie paska — tak samo jak w legendzie i z tego samego powodu: przełączenie kontekstu
// zdejmuje stare menu i wpuszcza nowe, a `capacity()` musi iść w parze z drzewem.
TRIAL("Menu: opróżnienie paska pozwala złożyć go od nowa") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});

    (void)window->add("Plik");
    (void)window->add("Widok");
    top.children().add(window);

    same(static_cast<int>(window->menus().size()), 2);

    window->clear();

    ought(window->menus().empty());
    same(window->children().many(), std::size_t{1});

    (void)window->add("Pomoc");

    const View made = top.render({{0, 0}, {20, 6}});

    same(read(made, 0), std::string{" Pomoc"});
}

TRIAL("Menu: tytuły stoją obok siebie") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});

    (void)window->add("Plik");
    (void)window->add("Widok");
    top.children().add(window);

    const View made = top.render({{0, 0}, {20, 6}});

    same(read(made, 0), std::string{" Plik  Widok"});
}

// **F9 bierze pasek, a nie rozwija menu.** Rozwija dopiero strzałka w dół, Enter albo litera
// tytułu — jak w Midnight Commanderze przy `drop_menus` wyłączonym. Przedtem F9 rozwijało pierwsze
// menu, zanim człowiek zdążył powiedzieć które, a rozwinięte menu połykało litery swoich pozycji,
// więc do tytułów docierało tylko to, czego żadna z nich nie chciała.
TRIAL("Menu: F9 bierze pasek, dół rozwija, Esc zamyka") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    window->content(body);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {20, 6}});

    ui::KeyEvent open{ui::Code::f9};

    ought(top.dispatch(open));
    same(window->current(), one.get());
    ought(!one->opened());

    ui::KeyEvent drop{ui::Code::down};

    ought(top.dispatch(drop));
    ought(one->opened());

    ui::KeyEvent close{ui::Code::escape};

    ought(top.dispatch(close));
    ought(!one->opened());
}

// **Menu zwija się samo**, para do `open`: ognisko wraca na jego tytuł, a następne rozwinięcie
// zaczyna od góry. Pasek już nie sięga do numeru pozycji menu.
TRIAL("Menu: close zwija, oddaje ognisko tytułowi i zapomina pozycję") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");
    const auto second = std::make_shared<ui::MenuItem>("Przenieś");

    one->children().add(first);
    one->children().add(std::make_shared<ui::MenuSeparator>());
    one->children().add(second);
    window->content(body);
    top.children().add(window);

    body->focus();
    (void)top.render({{0, 0}, {20, 6}});

    one->open();
    one->which(2);

    ought(second->focused());

    one->close();

    ought(!one->opened());
    same(window->current(), one.get());
    ought(!one->which().has_value());

    one->open();

    ought(first->focused());

    // Kreska nie jest pozycją: strzałka ją przeskakuje, a na zamkniętym menu close nic nie psuje.
    ui::KeyEvent down{ui::Code::down};

    ought(top.dispatch(down));
    ought(second->focused());

    window->leave();
    one->close();

    ought(!one->opened());
    ought(body->focused());

    // Zwinięte menu zachowuje zapamiętaną pozycję: od niej zacznie przy rozwinięciu.
    one->which(2);
    one->close();

    same(one->which(), std::optional<std::size_t>{2});

    one->open();

    ought(second->focused());
}

// **`dropdown()` rozwija od wejścia** — jak Midnight Commander z włączonym „Drop down menus":
// F9 pokazuje pierwsze menu od razu, strzałka w bok przechodzi na sąsiada rozwinięte, Esc
// zamyka wszystko. Bez flagi zachowanie jak wyżej. Program stawia flagę z ustawienia.
TRIAL("Menu: z dropdown() F9 rozwija pierwsze menu, a strzałka w bok rozwija sąsiada") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto first = window->add("Plik");
    const auto second = window->add("Widok");

    first->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    second->children().add(std::make_shared<ui::MenuItem>("Drzewo"));
    window->content(body);
    top.children().add(window);

    ought(!window->dropdown());

    window->dropdown(true);

    ought(window->dropdown());

    body->focus();

    (void)top.render({{0, 0}, {20, 6}});

    ui::KeyEvent open{ui::Code::f9};

    ought(top.dispatch(open));
    same(window->current(), first.get());
    ought(first->opened());
    ought(window->expanded());

    ui::KeyEvent right{ui::Code::right};

    ought(top.dispatch(right));
    ought(!first->opened());
    ought(second->opened());

    ui::KeyEvent close{ui::Code::escape};

    ought(top.dispatch(close));
    ought(!second->opened());
    ought(window->current() == nullptr);
    ought(body->focused());

    // Drugie wejście — przez enter(), jak z pola paska klawiszy — też rozwija.
    window->enter();

    ought(first->opened());

    window->leave();

    // Bez flagi po staremu: sam tytuł.
    window->dropdown(false);

    ought(top.dispatch(open));
    same(window->current(), first.get());
    ought(!first->opened());

    ought(top.dispatch(close));
}

// **`open()` to publiczne wejście do paska** — to samo, co robi F9, gdy pasek jest bezczynny —
// do pary z `close()`. Potrzebne, bo pole paska klawiszy woła metodę, a nie naciska klawisza;
// przedtem wołało `Menu::focus()`, czyli rozwijało pierwsze menu, i klik robił co innego niż F9.
// Pasek już czynny — z ogniskiem na drugim tytule albo z menu rozwiniętym — zostaje, jak stoi.
TRIAL("Menu: open() bierze pasek zwinięty i zostawia czynny w spokoju") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto first = window->add("Plik");
    const auto second = window->add("Widok");

    first->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    second->children().add(std::make_shared<ui::MenuItem>("Drzewo"));
    window->content(body);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {20, 6}});

    window->enter();

    same(window->current(), first.get());
    ought(unfolded(*window) == nullptr);
    ought(!window->expanded());
    ought(!body->focused());

    // Czynny pasek: ognisko na drugim tytule zostaje, gdzie było.
    ui::KeyEvent right{ui::Code::right};

    ought(top.dispatch(right));
    same(window->current(), second.get());

    window->enter();

    same(window->current(), second.get());
    ought(unfolded(*window) == nullptr);

    // Rozwinięte menu też zostaje rozwinięte.
    second->open();

    ought(second->opened());

    window->enter();

    ought(second->opened());
    ought(window->expanded());

    // Zamknięcie oddaje ognisko treści, jak po F9.
    window->leave();

    ought(body->focused());
}

// **Bit rozwinięcia żyje w pasku, nie w ognisku**, więc przeżywa odpięcie paska od drzewa:
// `Root::losing` gasi ognisko bez zdarzenia, a bit zostaje. Wejście musi go więc gasić samo —
// inaczej pasek dopięty z powrotem wchodziłby „zwinięty" z bitem rozwinięcia, i pierwsza
// strzałka w bok rozwijałaby sąsiada. Stan bierzemy z wędrówki przez pusty tytuł: bit stoi,
// a menu nie ma.
TRIAL("Menu: pasek dopięty z powrotem wchodzi zwinięty, choć odpięto go z bitem rozwinięcia") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto first = window->add("Plik");
    const auto second = window->add("Puste");

    first->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    window->content(body);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {20, 6}});

    first->open();

    ui::KeyEvent right{ui::Code::right};

    ought(top.dispatch(right));
    same(window->current(), second.get());
    ought(unfolded(*window) == nullptr);
    ought(window->expanded());

    ought(top.children().remove(window.get()));
    ought(top.focused_node() == nullptr);

    top.children().add(window);
    body->focus();

    ui::KeyEvent open{ui::Code::f9};

    ought(top.dispatch(open));
    same(window->current(), first.get());
    ought(!window->expanded());
}

TRIAL("Menu: otwarte rysuje pudełko z pozycjami") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik", ui::MenuShape{ui::Border::thin, false});

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj", "F5"));
    one->children().add(std::make_shared<ui::MenuItem>("Wyjście", "F10"));
    top.children().add(window);

    one->open();

    const View made = top.render({{0, 0}, {30, 8}});

    same(read(made, 0), std::string{" Plik"});
    same(read(made, 1), std::string{"┌──────────────┐"});
    same(read(made, 2), std::string{"│ Kopiuj    F5 │"});
    same(read(made, 3), std::string{"│ Wyjście  F10 │"});
    same(read(made, 4), std::string{"└──────────────┘"});
}

TRIAL("Menu: strzałki chodzą po pozycjach i omijają kreskę") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");
    const auto second = std::make_shared<ui::MenuItem>("Wyjście");

    one->children().add(first);
    one->children().add(std::make_shared<ui::MenuSeparator>());
    one->children().add(second);
    top.children().add(window);

    one->open();

    // Otwarcie staje od razu na pierwszej pozycji, jak w Midnight Commanderze, bo otwarcie **jest**
    // postawieniem ogniska, a ognisko siedzi na pozycji.
    same(one->which(), std::optional<std::size_t>{0});
    same(top.focused_node(), static_cast<ui::Node*>(first.get()));

    ui::KeyEvent down{ui::Code::down};

    ought(top.dispatch(down));
    same(one->which(), std::optional<std::size_t>{2});
    same(top.focused_node(), static_cast<ui::Node*>(second.get()));

    // Menu zawija — jest krótkie i widać je całe, więc nie ma gdzie się zgubić.
    ought(top.dispatch(down));
    same(one->which(), std::optional<std::size_t>{0});

    ui::KeyEvent up{ui::Code::up};

    ought(top.dispatch(up));
    same(one->which(), std::optional<std::size_t>{2});
}

// Numer i węzeł podświetlonej pozycji — dwie nazwy jednej rzeczy, jak w liście. Pozycja spoza
// menu niczego nie ustawia.
TRIAL("Menu: which to numer, current to pozycja, a obca pozycja nie ustawia niczego") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");
    const auto second = std::make_shared<ui::MenuItem>("Wyjście");
    const auto stranger = std::make_shared<ui::MenuItem>("Obca");

    one->children().add(first);
    one->children().add(second);
    top.children().add(window);

    ought(!one->which().has_value());
    ought(one->current() == nullptr);

    one->current(*second);

    same(one->which(), std::optional<std::size_t>{1});
    same(one->current(), second.get());

    one->current(*stranger);

    same(one->current(), second.get());

    one->which(0);

    same(one->current(), first.get());
}

// **Podświetlona pozycja to ta z ogniskiem** — nie ma osobnego znacznika, który mógłby się z tym
// rozjechać. Stąd bierze się też to, że dopięty do pozycji uchwyt widzi klawisz przed menu.
TRIAL("Menu: podświetlenie jest ogniskiem pozycji") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");
    const auto second = std::make_shared<ui::MenuItem>("Wyjście");

    one->children().add(first);
    one->children().add(second);
    top.children().add(window);

    one->open();

    View made = top.render({{0, 0}, {20, 6}});

    ought(made.at({1, 2}).style.reverse);
    ought(!made.at({1, 3}).style.reverse);

    int seen = 0;

    second->on<ui::KeyEvent>([&seen](ui::KeyEvent&) {
        ++seen;

        return true;
    });

    one->which(1);

    same(top.focused_node(), static_cast<ui::Node*>(second.get()));

    made = top.render({{0, 0}, {20, 6}});

    ought(!made.at({1, 2}).style.reverse);
    ought(made.at({1, 3}).style.reverse);

    // Uchwyt dopięty do pozycji dostaje klawisz **przed** menu, ale go menu nie odbiera: prawda
    // z uchwytu zamyka drogę uchwytów, a `handle` menu biegnie swoją i przesuwa podświetlenie.
    // Widżet i wołający nie zasłaniają sobie nawzajem — każdy zamyka tylko własny kanał.
    ui::KeyEvent down{ui::Code::down};

    ought(top.dispatch(down));
    same(seen, 1);
    same(one->which(), std::optional<std::size_t>{0});
}

// --- Litery aktywne ------------------------------------------------------------------------------

// Tak działa menu w Midnight Commanderze: w otwartym „Plik" wciśnięcie „w" uruchamia „Wyjście" bez
// chodzenia strzałkami. **Menu nie porównuje klawiszy** — rozdaje je swoim pozycjom i czeka, aż
// któraś weźmie.
//
// Wybór **zamyka menu**, tak samo jak Enter i jak kliknięcie, więc numer podświetlenia po wszystkim
// gaśnie — o to samo chodzi w `Menu::pick`.
TRIAL("Menu: litera aktywna wybiera pozycję w otwartym menu") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");
    const auto second = std::make_shared<ui::MenuItem>("Wyjście");

    first->keys().add(U'k');
    second->keys().add(U'w');

    one->children().add(first);
    one->children().add(second);
    top.children().add(window);

    int said = 0;

    (void)second->on<ui::PickEvent>([&said](ui::PickEvent&) {
        ++said;

        return true;
    });

    one->open();

    ui::KeyEvent what{U'w'};

    ought(top.dispatch(what));

    same(said, 1);
    ought(!one->opened());
}

// Zamknięte menu nie rozdaje niczego: litery należą wtedy do treści pod paskiem.
TRIAL("Menu: zamknięte nie bierze liter") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");

    first->keys().add(U'k');

    one->children().add(first);
    top.children().add(window);

    int said = 0;

    (void)first->on<ui::PickEvent>([&said](ui::PickEvent&) {
        ++said;

        return true;
    });

    ui::KeyEvent what{U'k'};

    ought(!top.dispatch(what));
    same(said, 0);
}

// Litera, której żadna pozycja nie wzięła, **nie wybiera niczego** — a to, że otwarte menu i tak ją
// połyka, jest regułą paska, nie pozycji: rozwinięte menu jest modalne jak w MC.
TRIAL("Menu: litera, której nikt nie wziął, niczego nie wybiera") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");

    first->keys().add(U'k');

    one->children().add(first);
    top.children().add(window);

    int said = 0;

    (void)first->on<ui::PickEvent>([&said](ui::PickEvent&) {
        ++said;

        return true;
    });

    one->open();

    ui::KeyEvent what{U'z'};

    (void)top.dispatch(what);

    same(said, 0);
    ought(one->opened());
}

TRIAL("Menu: wyszarzona pozycja nie odpowiada na swoją literę") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");

    first->keys().add(U'k');
    first->enabled(false);

    one->children().add(first);
    top.children().add(window);

    int said = 0;

    (void)first->on<ui::PickEvent>([&said](ui::PickEvent&) {
        ++said;

        return true;
    });

    one->open();

    ui::KeyEvent what{U'k'};

    (void)top.dispatch(what);

    same(said, 0);
    ought(one->opened());
}

// Ze sterującym nie łapiemy — ta sama reguła co w przycisku i w legendzie.
TRIAL("Menu: litera ze sterującym nie wybiera pozycji") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");

    first->keys().add(U'k');

    one->children().add(first);
    top.children().add(window);

    int said = 0;

    (void)first->on<ui::PickEvent>([&said](ui::PickEvent&) {
        ++said;

        return true;
    });

    one->open();

    ui::Mods with;
    with.alt = true;

    ui::KeyEvent what{ui::Key{U'k', with}};

    (void)top.dispatch(what);

    same(said, 0);
}

// Litera podkreśla się w napisie, a rolę na to daje skórka — kod widżetu nie wie o niej nic.
TRIAL("Menu: litera aktywna podkreśla się w napisie pozycji") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");

    first->keys().add(U'p');

    one->children().add(first);
    top.children().add(window);

    one->open();

    const View made = top.render({{0, 0}, {20, 6}});

    // „ Kopiuj" — napis stoi o kratkę od lewej, więc „p" jest w czwartej kolumnie ramki.
    same(made.at({4, 2}).letter, U'p');
    ought(made.at({4, 2}).style.under);
    ought(!made.at({3, 2}).style.under);
}

// Wyszarzenie podświetlonej pozycji oddaje ognisko menu: otwarte zostaje otwarte, tylko nic w nim
// nie jest podświetlone.
TRIAL("Menu: wyszarzenie podświetlonej pozycji nie zamyka menu") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");

    one->children().add(first);
    top.children().add(window);

    one->open();

    same(top.focused_node(), static_cast<ui::Node*>(first.get()));

    first->enabled(false);

    ought(one->opened());
    same(top.focused_node(), static_cast<ui::Node*>(one.get()));
    ought(one->current() == nullptr);
}

TRIAL("Menu: Enter woła zadanie i zamyka") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    int done = 0;

    one->children().add(proving::doing(std::make_shared<ui::MenuItem>("Kopiuj"), [&done] { ++done; }));
    top.children().add(window);

    one->open();

    ui::KeyEvent down{ui::Code::down};

    (void)top.dispatch(down);

    ui::KeyEvent enter{ui::Code::enter};

    ought(top.dispatch(enter));
    same(done, 1);
    ought(!one->opened());
}

TRIAL("Menu: klik wybiera pozycję pod kursorem, nie podświetloną") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    int first = 0;
    int second = 0;

    one->children().add(proving::doing(std::make_shared<ui::MenuItem>("Kopiuj"), [&first] { ++first; }));
    one->children().add(proving::doing(std::make_shared<ui::MenuItem>("Wyjście"), [&second] { ++second; }));
    top.children().add(window);

    one->open();
    one->which(0);

    (void)top.render({{0, 0}, {30, 8}});

    ui::ClickEvent what{{3, 3}, ui::Mouse::left};

    ought(top.dispatch(what));

    same(first, 0);
    same(second, 1);
    ought(!one->opened());
}

// **Ognisko wychodzi z paska — menu się zwija.** Przedtem działo się to samo z siebie, bo
// „rozwinięte" wynikało z ogniska, i nikt tego nie zapisał; odkąd rozwinięcie jest dopięciem,
// pasek zdejmuje je wprost na `UnfocusEvent`. Wyjście robimy tak, jak robi je każdy widżet
// treści po kliknięciu — biorąc ognisko — bo to ognisko, nie klik, jest tu mechanizmem.
TRIAL("Menu: ognisko wychodzące z paska zwija rozwinięte menu") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    window->content(body);
    top.children().add(window);

    (void)top.render({{0, 0}, {30, 8}});

    one->open();
    ought(one->opened());

    body->focus();

    ought(!one->opened());
    ought(unfolded(*window) == nullptr);
    ought(!window->expanded());
    ought(body->focused());
}

TRIAL("Menu: klik w tytuł otwiera i zamyka") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    top.children().add(window);

    (void)top.render({{0, 0}, {30, 8}});

    ui::ClickEvent what{{2, 0}, ui::Mouse::left};

    ought(top.dispatch(what));
    ought(one->opened());

    (void)top.render({{0, 0}, {30, 8}});

    ought(top.dispatch(what));
    ought(!one->opened());
}

// Tytuł bez pozycji to przycisk w pasku, nie pusta lista: nie rozwija się, podświetla się jak
// inne przy wędrówce strzałkami, a Enter i klik wołają jego własne `PickEvent`.
TRIAL("Menu: bez pozycji nie rozwija się, Enter i klik wołają zdarzenie tytułu") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto left = window->add("Lewy");
    const auto file = window->add("Plik");

    file->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    window->content(body);
    top.children().add(window);

    body->focus();

    int picked = 0;

    (void)left->on<ui::PickEvent>([&picked](ui::PickEvent&) {
        ++picked;

        return true;
    });

    (void)top.render({{0, 0}, {30, 8}});

    // F9 stawia ognisko na pierwszym menu — pustym: ognisko jest na nim samym, nic nie stoi
    // otworem, nic nie leży na treści.
    ui::KeyEvent open{ui::Code::f9};

    ought(top.dispatch(open));
    same(window->current(), left.get());
    ought(!left->opened());

    // Pod tytułem widać treść, nie pudełko.
    const View shut = top.render({{0, 0}, {30, 8}});

    same(read(shut, 1), std::string{"xxxxxxxxxx"});

    // Strzałka w prawo idzie na pełne menu; **rozwinięcia ze sobą nie niesie**, bo pasek wszedł
    // zwinięty. Rozwija dopiero strzałka w dół — i to jest ta sama reguła, co przy F9.
    ui::KeyEvent right{ui::Code::right};

    ought(top.dispatch(right));
    same(window->current(), file.get());
    ought(!file->opened());

    ui::KeyEvent drop{ui::Code::down};

    ought(top.dispatch(drop));
    ought(file->opened());

    ui::KeyEvent back{ui::Code::left};

    ought(top.dispatch(back));
    same(window->current(), left.get());
    ought(!file->opened());

    // Enter woła zdarzenie tytułu i zamyka pasek.
    ui::KeyEvent enter{ui::Code::enter};

    ought(top.dispatch(enter));
    same(picked, 1);
    ought(window->current() == nullptr);
    ought(body->focused());
    ought(unfolded(*window) == nullptr);

    // Klik w tytuł — to samo, bez otwierania.
    (void)top.render({{0, 0}, {30, 8}});

    ui::ClickEvent what{{2, 0}, ui::Mouse::left};

    ought(top.dispatch(what));
    same(picked, 2);
    ought(!left->opened());
    ought(window->current() == nullptr);
}

TRIAL("Menu: lewo i prawo przełączają, nie zamykając") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto first = window->add("Plik");
    const auto second = window->add("Widok");

    first->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    second->children().add(std::make_shared<ui::MenuItem>("Odśwież"));
    top.children().add(window);

    first->open();

    ui::KeyEvent right{ui::Code::right};

    ought(top.dispatch(right));
    ought(!first->opened());
    ought(second->opened());

    ought(top.dispatch(right));
    ought(first->opened());
}

TRIAL("Menu: Esc wraca do tego, kto miał ognisko") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    window->content(body);
    top.children().add(window);

    body->focus();

    ui::KeyEvent open{ui::Code::f9};

    ought(top.dispatch(open));

    // Esc wraca z **czynnego paska** tak samo jak z rozwiniętego menu — ognisko sprzed wejścia
    // pasek zapamiętuje przy pierwszym wzięciu, nie przy rozwinięciu.
    ui::KeyEvent close{ui::Code::escape};

    ought(top.dispatch(close));
    same(static_cast<const ui::Node*>(top.focused_node()), static_cast<const ui::Node*>(body.get()));
}

TRIAL("Menu: przejście z menu na menu nie gubi ogniska aplikacji") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto first = window->add("Plik");
    const auto second = window->add("Widok");

    first->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    second->children().add(std::make_shared<ui::MenuItem>("Odśwież"));
    window->content(body);
    top.children().add(window);

    body->focus();

    ui::KeyEvent open{ui::Code::f9};
    ui::KeyEvent right{ui::Code::right};
    ui::KeyEvent close{ui::Code::escape};

    (void)top.dispatch(open);
    (void)top.dispatch(right);
    (void)top.dispatch(close);

    same(static_cast<const ui::Node*>(top.focused_node()), static_cast<const ui::Node*>(body.get()));
}

TRIAL("Menu: treść stoi pod wierszem tytułów") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{40, 40}, U'x');

    window->content(body);
    (void)window->add("Plik");
    top.children().add(window);

    const View made = top.render({{0, 0}, {6, 3}});

    same(read(made, 0), std::string{" Plik"});
    same(read(made, 1), std::string{"xxxxxx"});
    same(window->content()->parent()->area().at.y, 1);
    same(window->content()->parent()->area().size.high, 2);
    same(body->area().at.y, 0);
}

TRIAL("Menu: otwarte zakrywa treść przy trafianiu") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{40, 40}, U'x');
    const auto one = window->add("Plik");

    int done = 0;

    one->children().add(proving::doing(std::make_shared<ui::MenuItem>("Kopiuj"), [&done] { ++done; }));
    window->content(body);
    top.children().add(window);

    one->open();

    (void)top.render({{0, 0}, {30, 8}});

    ui::ClickEvent what{{3, 2}, ui::Mouse::left};

    ought(top.dispatch(what));

    same(done, 1);
    same(body->hit, 0);
}

TRIAL("Menu: zamknięte nie łapie kliknięć tam, gdzie było otwarte") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{40, 40}, U'x');
    const auto one = window->add("Plik");

    int done = 0;

    one->children().add(proving::doing(std::make_shared<ui::MenuItem>("Kopiuj"), [&done] { ++done; }));
    window->content(body);
    top.children().add(window);

    one->open();

    (void)top.render({{0, 0}, {30, 8}});

    window->leave();

    (void)top.render({{0, 0}, {30, 8}});

    ui::ClickEvent what{{3, 2}, ui::Mouse::left};

    ought(top.dispatch(what));

    same(done, 0);
    same(body->hit, 1);
}

TRIAL("Menu: kreska dochodzi do ramki") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik", ui::MenuShape{ui::Border::thin, false});

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    one->children().add(std::make_shared<ui::MenuSeparator>());
    one->children().add(std::make_shared<ui::MenuItem>("Wyjście"));
    top.children().add(window);

    one->open();

    const View made = top.render({{0, 0}, {30, 9}});

    same(read(made, 3), std::string{"├─────────┤"});
}

TRIAL("Menu: wyszarzonej pozycji nie da się wybrać") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto item = std::make_shared<ui::MenuItem>("Kopiuj");

    int done = 0;

    (void)item->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });
    item->enabled(false);

    one->children().add(item);
    top.children().add(window);

    one->open();

    ui::KeyEvent down{ui::Code::down};

    (void)top.dispatch(down);

    ought(one->current() == nullptr);

    ui::KeyEvent enter{ui::Code::enter};

    (void)top.dispatch(enter);

    same(done, 0);
}

// **Pudełko staje pod swoim tytułem**, nie pod pierwszym z brzegu. To jest jedyne miejsce, w którym
// menu liczy swoje położenie — ze `standing()` tytułu, przy każdej klatce — i jedyna próba, która
// o nie pyta poza przypadkiem krawędzi: przy pierwszym menu tytuł stoi w zerze, więc pudełko
// w zerze wyglądałoby na dobrze policzone, choć nie byłoby policzone wcale.
TRIAL("Menu: pudełko drugiego menu staje pod drugim tytułem") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    (void)window->add("Plik", ui::MenuShape{ui::Border::thin, false});
    const auto second = window->add("Widok", ui::MenuShape{ui::Border::thin, false});

    second->children().add(std::make_shared<ui::MenuItem>("Odśwież"));
    top.children().add(window);

    second->open();

    const View made = top.render({{0, 0}, {30, 6}});

    // „ Plik " zajmuje sześć kolumn, więc „Widok" zaczyna się w szóstej — i tam stoi lewy róg.
    same(read(made, 0), std::string{" Plik  Widok"});
    same(made.at({6, 1}).letter, U'┌');
    same(made.at({0, 1}).letter, U' ');
}

TRIAL("Menu: przy prawej krawędzi wysuwa się w lewo") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto first = window->add("Plik");
    const auto second = window->add("Widok", ui::MenuShape{ui::Border::thin, false});

    first->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    second->children().add(std::make_shared<ui::MenuItem>("Bardzo długa pozycja"));
    top.children().add(window);

    second->open();

    const View made = top.render({{0, 0}, {26, 6}});

    same(second->area().at.x, 2);
    same(read(made, 0), std::string{" Plik  Widok"});
    same(read(made, 1), std::string{"  ┌──────────────────────┐"});
}

TRIAL("Menu: cień nie zjada tego, co pod spodem") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{40, 40}, U'x');
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    window->content(body);
    top.children().add(window);

    one->open();

    const View made = top.render({{0, 0}, {30, 8}});

    same(read(made, 1), std::string{"┌────────┐xxxxxxxxxxxxxxxxxxxx"});
    same(made.at({10, 2}).letter, U'x');
    ought(made.at({10, 2}).style.faint);
    ought(!made.at({10, 1}).style.faint);
    same(made.at({2, 4}).letter, U'x');
    ought(made.at({2, 4}).style.faint);
}

TRIAL("Menu: pasek nie przyjmuje niczego z zewnątrz") {
    ui::Menubar window;

    bool threw = false;

    try {
        window.children().add(std::make_shared<Box>(ui::Size{4, 4}, U'a'));
    } catch (const std::logic_error&) {
        threw = true;
    }

    ought(threw);
    same(static_cast<int>(window.children().many()), 1);
}

TRIAL("Menu: treść staje pod paskiem") {
    ui::Menubar window;

    ought(window.content() == nullptr);

    const auto one = std::make_shared<Box>(ui::Size{8, 4}, U'a');

    window.content(one);

    same(window.content(), static_cast<ui::Node*>(one.get()));

    // Gniazdo treści siedzi w kolumnie pod rzędem tytułów, a kolumna jest pierwszym dzieckiem
    // stosu paska.
    same(static_cast<const ui::Node*>(one->parent()->parent()->parent()),
         static_cast<const ui::Node*>(&window));

    const View made = window.render({{0, 0}, {8, 3}});

    same(read(made, 1), std::string{"aaaaaaaa"});
}

TRIAL("Menu: zamknięcie gasi podświetlenie") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    top.children().add(window);

    one->open();
    one->which(0);

    window->leave();

    ought(one->current() == nullptr);
}

// --- Najechanie myszą ----------------------------------------------------------------------------

// **Podświetlenie chodzi za kursorem**, jak w Midnight Commanderze. Ruch bez wciśniętego przycisku
// przychodzi tylko wtedy, gdy aplikacja poprosi o to w `Setup::hover` — ale sama droga jest ta
// sama, co przy każdym innym zdarzeniu myszy, i tu sprawdzamy właśnie ją.
TRIAL("Menu: najechanie przestawia podświetlenie") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    one->children().add(std::make_shared<ui::MenuItem>("Wyjście"));
    top.children().add(window);

    one->open();
    one->which(0);

    (void)top.render({{0, 0}, {30, 8}});

    same(one->which(), std::optional<std::size_t>{0});

    ui::MouseEvent what{{3, 3}, ui::Press::move, ui::Mouse::none};

    (void)top.dispatch(what);

    same(one->which(), std::optional<std::size_t>{1});
}

// Ruch **nie należy do nikogo na wyłączność**: pozycja przestawia podświetlenie i puszcza zdarzenie
// dalej, żeby nie odbierać go temu, kto też go potrzebuje.
TRIAL("Menu: najechanie nie połyka zdarzenia") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    one->children().add(std::make_shared<ui::MenuItem>("Wyjście"));
    top.children().add(window);

    one->open();

    (void)top.render({{0, 0}, {30, 8}});

    ui::MouseEvent what{{3, 3}, ui::Press::move, ui::Mouse::none};

    ought(!top.dispatch(what));
}

TRIAL("Menu: najechanie na wyłączoną pozycję nie przestawia") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    const auto second = std::make_shared<ui::MenuItem>("Wyjście");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    one->children().add(second);
    top.children().add(window);

    second->enabled(false);

    one->open();
    one->which(0);

    (void)top.render({{0, 0}, {30, 8}});

    ui::MouseEvent what{{3, 3}, ui::Press::move, ui::Mouse::none};

    (void)top.dispatch(what);

    same(one->which(), std::optional<std::size_t>{0});
}

// Zamknięte menu to sam tytuł w pasku: pod kursorem nie ma wtedy żadnej pozycji, więc nie ma czego
// podświetlać i nic się nie dzieje.
TRIAL("Menu: najechanie na zamknięte niczego nie otwiera") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    top.children().add(window);

    (void)top.render({{0, 0}, {30, 8}});

    ui::MouseEvent what{{1, 0}, ui::Press::move, ui::Mouse::none};

    (void)top.dispatch(what);

    ought(!one->opened());
}

// --- Grupa ------------------------------------------------------------------------------------

TRIAL("Menu: zdjęte menu znika też z drzewa") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto first = window->add("Plik");
    const auto second = window->add("Widok");

    top.children().add(window);

    same(window->menus().size(), std::size_t{2});

    // Zwinięte menu nie jest w drzewie; rozwinięte jest dzieckiem paska — i tylko wtedy.
    ought(!window->children().contains(second.get()));

    second->children().add(std::make_shared<ui::MenuItem>("Odśwież"));
    second->open();

    ought(window->children().contains(second.get()));

    window->clear();

    ought(window->menus().empty());
    ought(!window->children().contains(second.get()));
    ought(!window->children().contains(first.get()));

    // Opróżniony pasek składa się od nowa, a stare menu nie mają już z nim nic wspólnego.
    const auto again = window->add("Plik");

    same(window->menus().size(), std::size_t{1});
    same(read(top.render({{0, 0}, {26, 6}}), 0), std::string{" Plik"});
}

TRIAL("Menu: rdzeń roli tytułu można podmienić") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    same(one->role(), std::string{"talcite.menubar"});

    top.children().add(window);

    ui::Styles these;

    Style bar{};

    bar.foreground = Color{false, 3, 3, 3};

    these["moj.pasek"] = bar;

    top.skin(ui::Skin{these});
    one->role("moj.pasek");

    same(top.render({{0, 0}, {26, 6}}).at({1, 0}).style.foreground, (Color{false, 3, 3, 3}));
}

// --- Co menu przyjmuje ---------------------------------------------------------------------------

// **`capacity()` mówi ile, `accepts()` mówi czego.** Menu chodzi po swoich dzieciach jak po pozycjach —
// liczy je, przeskakuje rozdzielniki, sięga po `enabled()` — więc obce dziecko byłoby dla niego pustką
// w środku wędrówki. Zamiast oddawać tę pustkę w kółko przy każdym dostępie, odmawiamy raz,
// przy dopięciu.
TRIAL("Menu: przyjmuje pozycje i rozdzielniki, obcego dziecka nie") {
    const auto one = std::make_shared<ui::Menu>("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    one->children().add(std::make_shared<ui::MenuSeparator>());

    same(one->children().many(), std::size_t{2});

    bool threw = false;

    try {
        one->children().add(std::make_shared<ui::Span>("obce"));
    } catch (const std::logic_error&) {
        threw = true;
    }

    ought(threw);
    same(one->children().many(), std::size_t{2});
}

// Kreska sięgała po `"talcite.menu.frame"` wpisane na sztywno, choć rdzeń roli miała po pozycji menu
// od początku. Podmiana rdzenia obejmuje teraz i kreskę.
TRIAL("MenuSeparator: kreska idzie za rdzeniem roli pozycji") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto menu = window->add("Plik");
    const auto line = std::make_shared<ui::MenuSeparator>();

    same(line->role(), std::string{"talcite.menu"});

    // Kreska nie rozpycha menu, więc bez zwykłej pozycji obok miałaby zerową szerokość i nie
    // narysowałaby ani jednej kratki.
    menu->children().add(std::make_shared<ui::MenuItem>("Otwórz"));
    menu->children().add(line);

    top.children().add(window);

    ui::Styles these;

    Style look{};

    look.foreground = Color{false, 5, 5, 5};

    these["moje.menu.frame"] = look;

    top.skin(ui::Skin{these});
    line->role("moje.menu");

    menu->open();

    const View made = top.render({{0, 0}, {20, 6}});

    bool found = false;

    for (int y = 0; y < 6 && !found; ++y)
        for (int x = 0; x < 20 && !found; ++x)
            if (made.at({x, y}).style.foreground == Color{false, 5, 5, 5}) found = true;

    ought(found);
}

// **Litera tytułu przełącza menu przy czynnym pasku.** Tytuł jest dziś węzłem z klawiszami, więc
// bierze ją tym samym mechanizmem, co pozycja menu i pole legendy.
TRIAL("Menu: litera tytułu przełącza menu przy czynnym pasku") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto first = window->add("Plik");
    const auto second = window->add("Widok");

    first->keys().add(ui::Key{U'p'});
    second->keys().add(ui::Key{U'w'});

    first->children().add(std::make_shared<ui::MenuItem>("Otwórz"));
    second->children().add(std::make_shared<ui::MenuItem>("Odśwież"));

    window->content(body);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {20, 6}});

    ui::KeyEvent nine{ui::Code::f9};

    // F9 bierze pasek zwinięty — i **właśnie dlatego** litera tytułu ma teraz gdzie zadziałać:
    // gdyby pierwsze menu było rozwinięte, połknęłoby literę na rzecz swoich pozycji.
    ought(top.dispatch(nine));
    same(window->current(), first.get());
    same(unfolded(*window), static_cast<ui::Menu*>(nullptr));

    ui::KeyEvent look{U'w'};

    ought(top.dispatch(look));
    same(unfolded(*window), second.get());

    ui::KeyEvent file{U'P'};

    ought(top.dispatch(file));
    same(unfolded(*window), first.get());
}

// Bez czynnego paska litera nie ma prawa niczego rozwinąć — inaczej pisanie nad panelem
// otwierałoby menu.
TRIAL("Menu: litera tytułu przy zamkniętym pasku nic nie robi") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto first = window->add("Plik");

    first->keys().add(ui::Key{U'p'});
    first->children().add(std::make_shared<ui::MenuItem>("Otwórz"));

    window->content(body);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {20, 6}});

    ui::KeyEvent file{U'p'};

    ought(!top.dispatch(file));
    ought(unfolded(*window) == nullptr);
}

// Pozycja otwartego menu bierze literę **przed** tytułami: leży bliżej ogniska.
TRIAL("Menu: litera pozycji wygrywa z literą tytułu") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto first = window->add("Plik");
    const auto second = window->add("Widok");

    first->keys().add(ui::Key{U'p'});
    second->keys().add(ui::Key{U'w'});

    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto item = std::make_shared<ui::MenuItem>("Wytnij");

    item->keys().add(ui::Key{U'w'});

    int said = 0;

    (void)item->on<ui::PickEvent>([&said](ui::PickEvent&) {
        ++said;

        return true;
    });

    first->children().add(item);
    second->children().add(std::make_shared<ui::MenuItem>("Odśwież"));

    window->content(body);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {20, 6}});

    ui::KeyEvent nine{ui::Code::f9};

    ought(top.dispatch(nine));

    // Rozwinięcie jest tu **warunkiem pytania**: pierwszeństwo pozycji nad tytułem rozstrzyga się
    // dopiero przy rozwiniętym menu. Przy zwiniętym ta sama litera należy do tytułu — tak samo
    // rozdziela to Midnight Commander, jedną zmienną `is_dropped`.
    ui::KeyEvent drop{ui::Code::down};

    ought(top.dispatch(drop));

    ui::KeyEvent cut{U'w'};

    ought(top.dispatch(cut));
    same(said, 1);
}

// Tytuł podkreśla swoją literę, tak jak pozycja menu i przycisk.
TRIAL("Menu: litera tytułu podkreśla się w pasku") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto first = window->add("Plik");

    first->keys().add(ui::Key{U'l'});
    first->children().add(std::make_shared<ui::MenuItem>("Otwórz"));

    top.children().add(window);

    (void)top.render({{0, 0}, {20, 3}});

    const View made = top.render({{0, 0}, {20, 3}});

    ought(made.at({2, 0}).style.under);
    ought(!made.at({1, 0}).style.under);
}

// **Bez podanego klawisza pasek nie otwiera się z klawiatury.** Stało tu wpisane na sztywno F9 —
// biblioteka zajmowała sobie kombinację, o której nikt jej nie powiedział, i nie dało się jej ani
// zmienić, ani wyłączyć.
TRIAL("Menu: bez podanego klawisza pasek nie otwiera się z klawiatury") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    window->content(body);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {20, 6}});

    ui::KeyEvent nine{ui::Code::f9};

    ought(!top.dispatch(nine));
    ought(!one->opened());
}

// Klawisz podany przez wołającego przełącza pasek — otwiera zamknięty, zamyka otwarty. I nie musi
// być F9: pasek jest węzłem z klawiszami, jak pozycja menu i pole legendy.
TRIAL("Menu: podany klawisz przełącza pasek") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto one = window->add("Plik");

    window->keys().add(ui::Key{ui::Code::f2});

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    window->content(body);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {20, 6}});

    ui::KeyEvent two{ui::Code::f2};

    ought(top.dispatch(two));
    same(window->current(), one.get());

    ought(top.dispatch(two));
    ought(window->current() == nullptr);

    ui::KeyEvent nine{ui::Code::f9};

    ought(!top.dispatch(nine));
    ought(!one->opened());
}

TRIAL("Menu: znaki dwukolumnowe poszerzają pudełko o szerokość boków") {
    ui::Root top;

    top.skin(ui::Skin{{},
                      {{"talcite.line.thin.upper.left", U'口'},
                       {"talcite.line.thin.upper.right", U'口'},
                       {"talcite.line.thin.lower.left", U'口'},
                       {"talcite.line.thin.lower.right", U'口'},
                       {"talcite.line.thin.across", U'一'},
                       {"talcite.line.thin.upright", U'丨'},
                       {"talcite.line.thin.left.joint", U'卜'},
                       {"talcite.line.thin.right.joint", U'卜'}}});

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik", ui::MenuShape{ui::Border::thin, false});

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj", "F5"));
    one->children().add(std::make_shared<ui::MenuSeparator>());
    one->children().add(std::make_shared<ui::MenuItem>("Wyjście", "F10"));
    top.children().add(window);

    one->open();

    const View made = top.render({{0, 0}, {30, 8}});

    // Wnętrze ma czternaście kratek, jak w próbie ze zwykłą ramką — boki dokładają po dwie.
    same(one->wants({80, 25}).wide, 18);
    same(read(made, 1), std::string{"口一一一一一一一口"});
    same(read(made, 2), std::string{"丨 Kopiuj    F5 丨"});
    same(read(made, 3), std::string{"卜一一一一一一一卜"});
    same(read(made, 4), std::string{"丨 Wyjście  F10 丨"});
    same(read(made, 5), std::string{"口一一一一一一一口"});
}
