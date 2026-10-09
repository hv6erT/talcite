#include "proving.hpp"

#include <memory>
#include <string>
#include <vector>

#include "talcite/choice.hpp"
#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/modals.hpp"
#include "talcite/encoding.hpp"
#include "talcite/root.hpp"

using namespace talcite;

namespace {

std::string read(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter == 0) continue;

        utf8::encode(letter, made);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

std::vector<std::string> three() { return {"Nazwa", "Rozszerzenie", "Rozmiar"}; }

void counted(const std::shared_ptr<ui::Choice>& one, int& done) {
    (void)one->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });
}

}

// --- Co grupa wie o sobie ------------------------------------------------------------------------

TRIAL("Choice: pusta nie jest chętna na ognisko") {
    const ui::Choice one;

    same(one.options().many(), std::size_t{0});
    ought(!one.focusable());
    same(one.capacity(), 0);
}

TRIAL("Choice: świeża stoi na pierwszej opcji") {
    const ui::Choice one{three()};

    same(one.options().many(), std::size_t{3});
    same(*one.value(), std::string{"Nazwa"});
    ought(one.focusable());
}

TRIAL("Choice: wybór można podać przy tworzeniu i przyciąć") {
    same(*ui::Choice(three(), 2).value(), std::string{"Rozmiar"});
    same(*ui::Choice(three(), 9).value(), std::string{"Rozmiar"});
}

// **`which` to numer, `value` to napis** — ta sama wybrana opcja pod dwiema nazwami, tak jak
// numer i węzeł w liście. Ustawiacz numeru przycina, ustawiacz napisu pomija nieznany.
TRIAL("Choice: which to numer wybranej opcji, value to jej napis") {
    ui::Choice one{three()};

    same(one.which(), std::optional<std::size_t>{0});

    one.which(2);

    same(one.which(), std::optional<std::size_t>{2});
    same(*one.value(), std::string{"Rozmiar"});

    one.which(9);

    same(one.which(), std::optional<std::size_t>{2});

    one.value("Nazwa");

    same(one.which(), std::optional<std::size_t>{0});

    one.which(1);
    one.value("nie ma takiej");

    same(one.which(), std::optional<std::size_t>{1});

    ui::Choice empty;

    empty.which(1);

    ought(!empty.which().has_value());
    ought(!empty.value().has_value());

    // Numer spoza pustej grupy nie zostaje zapamiętany na później.
    empty.options().add("raz");
    empty.options().add("dwa");

    same(empty.which(), std::optional<std::size_t>{0});
}

// Przycięty numer jest zapamiętany przycięty, a zbiór, który zmalał pod wybraną opcją, oddaje
// ostatnią — nie numer spoza zbioru.
TRIAL("Choice: numer przycina się przy ustawieniu i przy czytaniu") {
    ui::Choice one{three()};

    one.which(9);
    one.options().add("Czas");

    same(one.which(), std::optional<std::size_t>{2});

    one.which(3);
    one.options().remove(3);

    same(one.which(), std::optional<std::size_t>{2});
    same(*one.value(), std::string{"Rozmiar"});
}

// Cztery kratki na klamrę i spację plus najdłuższy napis; wysokość to liczba opcji.
TRIAL("Choice: chce klamry, najdłuższego napisu i wiersza na opcję") {
    const ui::Choice one{three()};

    same(one.wants({40, 10}), (ui::Size{16, 3}));
    same(one.wants({8, 2}), (ui::Size{8, 2}));
}

// --- Rysowanie -----------------------------------------------------------------------------------

TRIAL("Choice: kropka stoi przy wybranej opcji") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three(), 1);

    top.children().add(one);

    const View made = top.render({{0, 0}, {16, 3}});

    same(read(made, 0), std::string{"( ) Nazwa"});
    same(read(made, 1), std::string{"(•) Rozszerzenie"});
    same(read(made, 2), std::string{"( ) Rozmiar"});
}

// **Podświetlenie tylko przy ognisku**: okno bez ogniska w grupie wyglądałoby inaczej, jakby na
// niej stało.
TRIAL("Choice: podświetlenie pokazuje się dopiero z ogniskiem") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three(), 1);

    top.children().add(one);

    ought(!top.render({{0, 0}, {16, 3}}).at({0, 1}).style.reverse);

    one->focus();

    const View made = top.render({{0, 0}, {16, 3}});

    ought(made.at({0, 1}).style.reverse);
    ought(!made.at({0, 0}).style.reverse);
    ought(!made.at({0, 2}).style.reverse);
}

// --- Ruch ----------------------------------------------------------------------------------------

// **Ruch jest wyborem** — strzałka od razu przestawia zaznaczenie, a nie osobny kursor.
TRIAL("Choice: strzałki przestawiają wybór i zawiadamiają") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three());

    top.children().add(one);
    one->focus();

    int done = 0;

    counted(one, done);

    ui::KeyEvent down{ui::Code::down};

    ought(top.dispatch(down));
    same(*one->value(), std::string{"Rozszerzenie"});
    same(done, 1);

    ought(top.dispatch(down));
    same(*one->value(), std::string{"Rozmiar"});
    same(done, 2);

    ui::KeyEvent up{ui::Code::up};

    ought(top.dispatch(up));
    same(*one->value(), std::string{"Rozszerzenie"});
    same(done, 3);
}

// Na krańcu grupy klawisz **idzie wyżej**: w oknie strzałka ma wtedy przejść do następnego widżetu,
// a nie utknąć.
TRIAL("Choice: na krańcu oddaje strzałkę wyżej") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three());

    top.children().add(one);
    one->focus();

    ui::KeyEvent up{ui::Code::up};

    ought(!top.dispatch(up));
    same(*one->value(), std::string{"Nazwa"});

    one->value("Rozmiar");

    ui::KeyEvent down{ui::Code::down};

    ought(!top.dispatch(down));
    same(*one->value(), std::string{"Rozmiar"});
}

TRIAL("Choice: Home i End skaczą na krańce") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three());

    top.children().add(one);
    one->focus();

    ui::KeyEvent end{ui::Code::end};

    ought(top.dispatch(end));
    same(*one->value(), std::string{"Rozmiar"});

    ui::KeyEvent home{ui::Code::home};

    ought(top.dispatch(home));
    same(*one->value(), std::string{"Nazwa"});
}

// **Ustawienie z programu nie zawiadamia** — tak samo jak w polu wyboru.
TRIAL("Choice: podanie wyboru milczy") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three());

    top.children().add(one);

    int done = 0;

    counted(one, done);

    one->value("Rozmiar");

    same(*one->value(), std::string{"Rozmiar"});
    same(done, 0);
}

TRIAL("Choice: enter i litery idą wyżej") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three());

    top.children().add(one);
    one->focus();

    ui::KeyEvent enter{ui::Code::enter};

    ought(!top.dispatch(enter));

    ui::KeyEvent letter{U'z'};

    ought(!top.dispatch(letter));
}

// --- Kliknięcie ----------------------------------------------------------------------------------

TRIAL("Choice: kliknięcie wybiera swoją opcję i bierze ognisko") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three());

    top.children().add(one);

    (void)top.render({{0, 0}, {16, 3}});

    int done = 0;

    counted(one, done);

    ui::ClickEvent what{{6, 2}, ui::Mouse::left};

    ought(top.dispatch(what));
    same(*one->value(), std::string{"Rozmiar"});
    same(done, 1);
    same(top.focused_node(), static_cast<ui::Node*>(one.get()));

    // Kliknięcie w już wybraną nie jest zmianą.
    ought(top.dispatch(what));
    same(done, 1);
}

// --- W oknie -------------------------------------------------------------------------------------

// Grupa jest **jednym przystankiem** wędrówki: Tab wchodzi do niej raz i raz z niej wychodzi,
// a strzałki chodzą w środku.
TRIAL("Choice: w oknie jest jednym przystankiem Taba") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto one = std::make_shared<ui::Choice>(three());
    const auto other = std::make_shared<ui::Choice>(three());

    column->children().add(one);
    column->children().add(other);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    same(top.focused_node(), static_cast<ui::Node*>(one.get()));

    ui::KeyEvent tab{ui::Code::tab};

    ought(top.dispatch(tab));
    same(top.focused_node(), static_cast<ui::Node*>(other.get()));

    ought(top.dispatch(tab));
    same(top.focused_node(), static_cast<ui::Node*>(one.get()));
}

TRIAL("Choice: wyłączona nie wybiera i oddaje ognisko") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three());

    top.children().add(one);
    one->focus();

    one->enabled(false);

    ought(!one->focusable());
    ought(top.focused_node() != static_cast<ui::Node*>(one.get()));

    ui::KeyEvent down{ui::Code::down};

    (void)top.dispatch(down);

    same(*one->value(), std::string{"Nazwa"});
}

// Zbiór opcji nie ma wskazania na właściciela, więc opróżnienie go nie ma jak zawiadomić grupy.
// Ognisko schodzi więc przy najbliższym rysowaniu — ale schodzi.
TRIAL("Choice: opróżniona pod ogniskiem oddaje je przy rysowaniu") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three());

    top.children().add(one);
    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(one.get()));

    one->options().clear();

    ought(!one->focusable());
    same(top.focused_node(), static_cast<ui::Node*>(one.get()));

    (void)top.render({{0, 0}, {16, 3}});

    ought(top.focused_node() != static_cast<ui::Node*>(one.get()));
}
