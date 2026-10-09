#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/modals.hpp"
#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/tick.hpp"

using namespace talcite;

namespace {

std::string read(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) utf8::encode(one.at({x, y}).letter, made);

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

int counted(const std::shared_ptr<ui::Tick>& one, int& done) {
    (void)one->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });

    return done;
}

}

// --- Co pole wie o sobie -------------------------------------------------------------------------

TRIAL("Tick: świeże jest puste i chętne na ognisko") {
    const ui::Tick one{"Zawijaj"};

    ought(!one.ticked());
    ought(one.enabled());
    ought(one.focusable());
    same(one.capacity(), 0);
    same(one.text(), std::string{"Zawijaj"});
}

TRIAL("Tick: stan można podać przy tworzeniu") {
    const ui::Tick one{"Zawijaj", true};

    ought(one.ticked());
}

// Cztery kratki na klamrę i spację, reszta na napis — jeden wiersz, nie tyle, ile dano.
TRIAL("Tick: chce klamry plus napisu i jednego wiersza") {
    const ui::Tick one{"Zawijaj"};

    same(one.wants({40, 5}), (ui::Size{11, 1}));
    same(one.wants({6, 5}), (ui::Size{6, 1}));
}

// --- Rysowanie -----------------------------------------------------------------------------------

TRIAL("Tick: rysuje klamrę ze stanem i napis") {
    ui::Root top;

    const auto one = std::make_shared<ui::Tick>("Zawijaj");

    top.children().add(one);

    same(read(top.render({{0, 0}, {11, 1}}), 0), std::string{"[ ] Zawijaj"});

    one->ticked(true);

    same(read(top.render({{0, 0}, {11, 1}}), 0), std::string{"[x] Zawijaj"});
}

// Za wąskie pole ucina **napis**, nie klamrę: klamra jest tym, co czyni z tego pole wyboru.
TRIAL("Tick: za wąskie ucina napis, a nie klamrę") {
    ui::Root top;

    const auto one = std::make_shared<ui::Tick>("Zawijaj");

    top.children().add(one);

    const std::string made = read(top.render({{0, 0}, {7, 1}}), 0);

    same(made.substr(0, 4), std::string{"[ ] "});
    same(made.size(), std::size_t{4 + 3 + 2});
}

TRIAL("Tick: z ogniskiem ma własną rolę") {
    ui::Root top;

    const auto one = std::make_shared<ui::Tick>("Zawijaj");

    top.children().add(one);

    ought(!top.render({{0, 0}, {11, 1}}).at({0, 0}).style.reverse);

    one->focus();

    ought(top.render({{0, 0}, {11, 1}}).at({0, 0}).style.reverse);
}

// --- Odwracanie ----------------------------------------------------------------------------------

// **Ustawienie z programu nie zawiadamia**, bo wypełnienie okna wartościami początkowymi nie jest
// naciskaniem. Odwrócenie przez `pick` zawiadamia.
TRIAL("Tick: podanie stanu milczy, odwrócenie zawiadamia") {
    ui::Root top;

    const auto one = std::make_shared<ui::Tick>("Zawijaj");

    top.children().add(one);

    int done = 0;

    (void)counted(one, done);

    one->ticked(true);

    same(done, 0);

    one->pick();

    ought(!one->ticked());
    same(done, 1);
}

TRIAL("Tick: spacja odwraca, a enter idzie wyżej") {
    ui::Root top;

    const auto one = std::make_shared<ui::Tick>("Zawijaj");

    top.children().add(one);

    one->focus();

    int done = 0;

    (void)counted(one, done);

    ui::KeyEvent space{U' '};

    ought(top.dispatch(space));
    ought(one->ticked());
    same(done, 1);

    ui::KeyEvent enter{ui::Code::enter};

    ought(!top.dispatch(enter));
    ought(one->ticked());
    same(done, 1);
}

TRIAL("Tick: kliknięcie bierze ognisko i odwraca") {
    ui::Root top;

    const auto one = std::make_shared<ui::Tick>("Zawijaj");

    top.children().add(one);

    (void)top.render({{0, 0}, {11, 1}});

    int done = 0;

    (void)counted(one, done);

    ui::ClickEvent what{{2, 0}, ui::Mouse::left};

    ought(top.dispatch(what));
    ought(one->ticked());
    same(done, 1);
    same(top.focused_node(), static_cast<ui::Node*>(one.get()));
}

// --- Wyłączone -----------------------------------------------------------------------------------

TRIAL("Tick: wyłączone nie odwraca się i nie bierze ogniska") {
    ui::Root top;

    const auto one = std::make_shared<ui::Tick>("Zawijaj");

    top.children().add(one);

    one->enabled(false);

    ought(!one->focusable());

    int done = 0;

    (void)counted(one, done);

    one->pick();

    ought(!one->ticked());
    same(done, 0);
}

// Wyłączone pole nie może zostać z ogniskiem — ognisko utknęłoby na czymś, z czego Tab już nie
// wychodzi, bo wędrówka wyłączonych nie widzi.
TRIAL("Tick: wyłączenie oddaje ognisko") {
    ui::Root top;

    const auto one = std::make_shared<ui::Tick>("Zawijaj");

    top.children().add(one);

    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(one.get()));

    one->enabled(false);

    ought(top.focused_node() != static_cast<ui::Node*>(one.get()));
}

// --- Klawisz aktywny -----------------------------------------------------------------------------

// To jest ta rzecz, dla której cała przebudowa: pole wyboru odpowiada na swoją literę **z dowolnego
// miejsca okna**, tą samą drogą co przycisk, bo okno rozdaje klawisz zdarzeniem, a nie pyta o typ.
TRIAL("Tick: klawisz aktywny działa z dowolnego miejsca okna") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto one = std::make_shared<ui::Tick>("Zawijaj");
    const auto other = std::make_shared<ui::Tick>("Wielkość liter");

    one->keys().add(U'Z');
    other->keys().add(U'W');

    column->children().add(one);
    column->children().add(other);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    // Ognisko stoi na pierwszym polu, a naciskamy literę drugiego.
    same(top.focused_node(), static_cast<ui::Node*>(one.get()));

    ui::KeyEvent what{U'w'};

    ought(top.dispatch(what));

    ought(!one->ticked());
    ought(other->ticked());
}

TRIAL("Tick: wyłączone nie odpowiada na swój klawisz") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto one = std::make_shared<ui::Tick>("Zawijaj");

    one->keys().add(U'Z');

    column->children().add(one);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    one->enabled(false);

    ui::KeyEvent what{U'z'};

    (void)top.dispatch(what);

    ought(!one->ticked());
}
