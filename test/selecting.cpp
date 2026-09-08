#include "proving.hpp"

#include <memory>
#include <string>
#include <vector>

#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/select.hpp"
#include "talcite/veilwindow.hpp"

using namespace talcite;

namespace {

std::vector<std::string> three() { return {"UTF-8", "ISO-8859-2", "CP1250"}; }

std::string read(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter == 0) continue;

        spelled(letter, made);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

void counted(const std::shared_ptr<ui::Select>& one, int& done) {
    (void)one->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });
}

// Wybór **w oknie**, bo bez okna zasłon nie ma go gdzie rozwinąć — a to jest część umowy, nie
// niedogodność próby.
struct Stage {
    ui::Root top;
    std::shared_ptr<ui::VeilWindow> window = std::make_shared<ui::VeilWindow>();
    std::shared_ptr<ui::Column> column = std::make_shared<ui::Column>();
    std::shared_ptr<ui::Select> one = std::make_shared<ui::Select>(three());

    Stage() {
        column->children().add(one);
        window->body()->children().add(column);
        top.children().add(window);
    }

    View drawn(ui::Size size = {30, 12}) { return top.render({{0, 0}, size}); }
};

}

// --- Co wybór wie o sobie ------------------------------------------------------------------------

TRIAL("Select: pusty nic nie niesie, ale ognisko bierze") {
    const ui::Select one;

    same(one.options().many(), std::size_t{0});
    ought(!one.which().has_value());
    ought(one.focusable());
    same(one.takes(), 0);
    ought(!one.opened());
}

TRIAL("Select: świeży stoi na pierwszej opcji") {
    const ui::Select one{three()};

    same(one.options().many(), std::size_t{3});
    same(*one.which(), std::string{"UTF-8"});
}

TRIAL("Select: wybór można podać przy tworzeniu i przyciąć") {
    same(*ui::Select(three(), 2).which(), std::string{"CP1250"});
    same(*ui::Select(three(), 9).which(), std::string{"CP1250"});
}

// Najdłuższa pozycja plus dwie kratki na spację i znak rozwinięcia; wysokość zawsze jeden wiersz.
TRIAL("Select: chce najdłuższej opcji i jednego wiersza") {
    const ui::Select one{three()};

    same(one.wants({40, 10}), (ui::Size{12, 1}));
    same(one.wants({6, 3}), (ui::Size{6, 1}));
}

TRIAL("Select: podanie nowych opcji przycina wybór") {
    ui::Select one{three(), 2};

    one.options().all({"raz"});

    same(*one.which(), std::string{"raz"});
}

// --- Rysowanie -----------------------------------------------------------------------------------

// Wiersz wychodzi szeroki na najdłuższą opcję plus dwie kratki, bo tyle chce — a znak rozwinięcia
// stoi przy prawej krawędzi tego, co dostał.
TRIAL("Select: rysuje bieżącą wartość i znak rozwinięcia przy prawej krawędzi") {
    Stage stage;

    const View made = stage.drawn();

    same(read(made, 0), std::string{"UTF-8"} + std::string(6, ' ') + "\xe2\x96\xbc");
}

TRIAL("Select: za wąski ucina napis, a nie znak rozwinięcia") {
    Stage stage;

    stage.one->which("ISO-8859-2");

    const View made = stage.drawn({8, 12});

    // Znak rozwinięcia zostaje na swoim miejscu, a napis ustępuje mu wielokropkiem.
    same(made.at({7, 0}).letter, U'▼');
    same(made.at({5, 0}).letter, U'…');
    same(made.at({0, 0}).letter, U'I');
}

TRIAL("Select: z ogniskiem ma własną rolę") {
    Stage stage;

    ought(!stage.drawn().at({0, 0}).style.reverse);

    stage.one->focus();

    ought(stage.drawn().at({0, 0}).style.reverse);
}

// --- Klawisze przy zamkniętym --------------------------------------------------------------------

// **Ruch jest wyborem** — strzałka od razu przestawia wartość, tak samo jak w grupie opcji.
TRIAL("Select: strzałki przestawiają wartość i zawiadamiają") {
    Stage stage;

    stage.one->focus();

    int done = 0;

    counted(stage.one, done);

    ui::KeyEvent down{ui::Code::down};

    ought(stage.top.deal(down));
    same(*stage.one->which(), std::string{"ISO-8859-2"});
    same(done, 1);

    ui::KeyEvent up{ui::Code::up};

    ought(stage.top.deal(up));
    same(*stage.one->which(), std::string{"UTF-8"});
    same(done, 2);

    ought(!stage.one->opened());
}

// Na krańcu klawisz **idzie wyżej**: w oknie strzałka ma wtedy przejść do następnego widżetu.
TRIAL("Select: na krańcu oddaje strzałkę wyżej") {
    Stage stage;

    stage.one->focus();

    ui::KeyEvent up{ui::Code::up};

    ought(!stage.top.deal(up));

    stage.one->which("CP1250");

    ui::KeyEvent down{ui::Code::down};

    ought(!stage.top.deal(down));
    same(*stage.one->which(), std::string{"CP1250"});
}

TRIAL("Select: Home i End skaczą na krańce") {
    Stage stage;

    stage.one->focus();

    ui::KeyEvent end{ui::Code::end};

    ought(stage.top.deal(end));
    same(*stage.one->which(), std::string{"CP1250"});

    ui::KeyEvent home{ui::Code::home};

    ought(stage.top.deal(home));
    same(*stage.one->which(), std::string{"UTF-8"});
}

// **Litery idą wyżej i to jest warunek**, żeby klawisz aktywny przycisku działał, gdy ognisko stoi
// na wyborze. Tak samo robi menu: bierze swoje, resztę puszcza.
TRIAL("Select: litery i Tab idą wyżej") {
    Stage stage;

    stage.one->focus();

    ui::KeyEvent letter{U'z'};

    ought(!stage.top.deal(letter));

    ui::KeyEvent tab{ui::Code::tab};

    ought(!stage.top.deal(tab));
}

TRIAL("Select: podanie wartości milczy") {
    Stage stage;

    int done = 0;

    counted(stage.one, done);

    stage.one->which("CP1250");

    same(*stage.one->which(), std::string{"CP1250"});
    same(done, 0);
}

// --- Rozwijanie ----------------------------------------------------------------------------------

TRIAL("Select: Enter rozwija, Esc zamyka i oddaje ognisko") {
    Stage stage;

    stage.one->focus();

    ui::KeyEvent enter{ui::Code::enter};

    ought(stage.top.deal(enter));
    ought(stage.one->opened());
    same(static_cast<int>(stage.window->veils().size()), 1);

    ui::KeyEvent escape{ui::Code::escape};

    ought(stage.top.deal(escape));

    ought(!stage.one->opened());
    ought(stage.window->veils().empty());
    same(stage.top.focus(), static_cast<ui::Node*>(stage.one.get()));
}

// **Spacja nie rozwija.** Rozwijała kiedyś, razem z Enterem — a to zabierało aplikacji klawisz,
// który w oknie z polem tekstowym należy do pisania. Tak samo straciła ją przycisk.
TRIAL("Select: spacja nie rozwija") {
    Stage stage;

    stage.one->focus();

    ui::KeyEvent space{U' '};

    ought(!stage.top.deal(space));
    ought(!stage.one->opened());
}

// **Rozwinięta lista staje pod widżetem**, a nie na środku ekranu jak zwykłe okno.
TRIAL("Select: rozwinięta lista staje pod wyborem") {
    Stage stage;

    (void)stage.drawn();

    stage.one->open();

    const View made = stage.drawn();

    // Wiersz zero to sam wybór; ramka listy zaczyna się w wierszu pierwszym, od lewej krawędzi.
    same(made.at({0, 1}).letter, U'┌');
    same(made.at({0, 2}).letter, U'│');
    same(made.at({1, 2}).letter, U'U');
    same(made.at({1, 3}).letter, U'I');
}

// Pod dolną krawędzią nie ma miejsca, więc lista **wychodzi w górę**. O tym, że się nie mieści,
// wie tylko ten, kto ma cały ekran.
TRIAL("Select: bez miejsca pod spodem lista wychodzi w górę") {
    ui::Root top;

    const auto window = std::make_shared<ui::VeilWindow>();
    const auto column = std::make_shared<ui::Column>();
    const auto one = std::make_shared<ui::Select>(three());

    // Wybór stoi w ostatnim wierszu ekranu.
    for (int at = 0; at < 6; ++at)
        column->children().add(std::make_shared<ui::Select>(std::vector<std::string>{"x"}));

    column->children().add(one);
    window->body()->children().add(column);
    top.children().add(window);

    (void)top.render({{0, 0}, {30, 7}});

    one->open();

    const View made = top.render({{0, 0}, {30, 7}});

    // Wybór jest w ostatnim wierszu, więc w wierszu 6 dalej stoi on sam ze swoim znakiem.
    same(made.at({11, 6}).letter, U'▼');

    // A ramka listy kończy się tuż nad nim.
    same(made.at({0, 5}).letter, U'└');
    same(made.at({0, 1}).letter, U'┌');
}

TRIAL("Select: rozwinięcie dwa razy nie tworzy dwóch list") {
    Stage stage;

    stage.one->open();
    stage.one->open();

    same(static_cast<int>(stage.window->veils().size()), 1);
}

TRIAL("Select: pusty nie ma czego rozwijać") {
    ui::Root top;

    const auto window = std::make_shared<ui::VeilWindow>();
    const auto one = std::make_shared<ui::Select>();

    window->body()->children().add(one);
    top.children().add(window);

    one->open();

    ought(!one->opened());
    ought(window->veils().empty());
}

// --- Wybór z rozwiniętej listy -------------------------------------------------------------------

// Wybór wraca **jednym zdarzeniem z wyboru**, nie z listy — aplikacja nie musi wiedzieć, że pod
// spodem stała jakakolwiek lista.
TRIAL("Select: pozycja wybrana Enterem wraca zdarzeniem i zamyka listę") {
    Stage stage;

    stage.one->focus();

    int done = 0;

    counted(stage.one, done);

    stage.one->open();

    (void)stage.drawn();

    ui::KeyEvent down{ui::Code::down};

    ought(stage.top.deal(down));

    ui::KeyEvent enter{ui::Code::enter};

    ought(stage.top.deal(enter));

    same(*stage.one->which(), std::string{"ISO-8859-2"});
    same(*stage.one->which(), std::string{"ISO-8859-2"});
    same(done, 1);
    ought(!stage.one->opened());
}

// Wybranie tej samej pozycji **nie jest zmianą** — ta sama reguła co przy kliknięciu w już wybraną
// opcję w grupie.
TRIAL("Select: wybranie tej samej pozycji nie zawiadamia") {
    Stage stage;

    stage.one->focus();

    int done = 0;

    counted(stage.one, done);

    stage.one->open();

    (void)stage.drawn();

    ui::KeyEvent enter{ui::Code::enter};

    ought(stage.top.deal(enter));

    same(done, 0);
    ought(!stage.one->opened());
}

// --- Kliknięcie ----------------------------------------------------------------------------------

// Kliknięcie rozwija, a ognisko idzie **w głąb rozwiniętej listy** — tak jak przy każdym innym
// otwarciu zasłony. Do wyboru wraca dopiero po zamknięciu.
TRIAL("Select: kliknięcie bierze ognisko i rozwija") {
    Stage stage;

    (void)stage.drawn();

    ui::ClickEvent what{{2, 0}, ui::Mouse::left};

    ought(stage.top.deal(what));
    ought(stage.one->opened());
    ought(stage.top.focus() != static_cast<ui::Node*>(stage.one.get()));

    ui::KeyEvent escape{ui::Code::escape};

    ought(stage.top.deal(escape));

    ought(!stage.one->opened());
    same(stage.top.focus(), static_cast<ui::Node*>(stage.one.get()));
}

// --- Wyłączony -----------------------------------------------------------------------------------

TRIAL("Select: wyłączony nie rozwija, nie wybiera i oddaje ognisko") {
    Stage stage;

    stage.one->focus();
    stage.one->able(false);

    ought(!stage.one->focusable());
    ought(stage.top.focus() != static_cast<ui::Node*>(stage.one.get()));

    stage.one->open();

    ought(!stage.one->opened());

    ui::KeyEvent down{ui::Code::down};

    (void)stage.top.deal(down);

    same(*stage.one->which(), std::string{"UTF-8"});
}

TRIAL("Select: wyłączenie w trakcie zamyka rozwiniętą listę") {
    Stage stage;

    stage.one->open();

    ought(stage.one->opened());

    stage.one->able(false);

    ought(!stage.one->opened());
    ought(stage.window->veils().empty());
}

// --- W oknie dialogowym --------------------------------------------------------------------------

// Wybór jest **jednym przystankiem** wędrówki, a rozwinięta lista jej nie psuje.
TRIAL("Select: w oknie jest jednym przystankiem Taba") {
    ui::Root top;

    const auto window = std::make_shared<ui::VeilWindow>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto one = std::make_shared<ui::Select>(three());
    const auto other = std::make_shared<ui::Select>(three());

    column->children().add(one);
    column->children().add(other);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    same(top.focus(), static_cast<ui::Node*>(one.get()));

    ui::KeyEvent tab{ui::Code::tab};

    ought(top.deal(tab));
    same(top.focus(), static_cast<ui::Node*>(other.get()));

    ought(top.deal(tab));
    same(top.focus(), static_cast<ui::Node*>(one.get()));
}

// Rozwinięta lista **leży nad oknem dialogowym**, bo obie są zasłonami w tym samym oknie, a stos
// rozstrzyga kolejność. Zamknięcie listy oddaje ognisko oknu, a nie aplikacji.
TRIAL("Select: lista rozwinięta w oknie zamyka się nad nim i oddaje mu ognisko") {
    ui::Root top;

    const auto window = std::make_shared<ui::VeilWindow>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto one = std::make_shared<ui::Select>(three());

    column->children().add(one);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    (void)top.render({{0, 0}, {30, 12}});

    one->open();

    same(static_cast<int>(window->veils().size()), 2);
    same(window->opened(), static_cast<ui::Veil*>(one->opened() ? window->veils().back().get()
                                                             : nullptr));

    ui::KeyEvent escape{ui::Code::escape};

    ought(top.deal(escape));

    same(static_cast<int>(window->veils().size()), 1);
    same(top.focus(), static_cast<ui::Node*>(one.get()));
}
