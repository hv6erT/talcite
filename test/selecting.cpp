#include "proving.hpp"

#include <memory>
#include <string>
#include <vector>

#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/encoding.hpp"
#include "talcite/list.hpp"
#include "talcite/modal.hpp"
#include "talcite/root.hpp"
#include "talcite/select.hpp"
#include "talcite/modals.hpp"

using namespace talcite;

namespace {

std::vector<std::string> three() { return {"UTF-8", "ISO-8859-2", "CP1250"}; }

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

// Rozwinięta lista, jeśli wisi: zasłona → ramka → lista. Próby pytają ją wprost o wiersze, suwak
// i bieżącą pozycję, bo to są rzeczy, których z ekranu nie da się odczytać bez liczenia kratek.
ui::List* rolled(const ui::Modals& window) {
    const ui::Modal* veil = window.current();

    if (veil == nullptr) return nullptr;

    return dynamic_cast<ui::List*>(veil->children().front()->children().front().get());
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
    std::shared_ptr<ui::Modals> window = std::make_shared<ui::Modals>();
    std::shared_ptr<ui::Column> column = std::make_shared<ui::Column>();
    std::shared_ptr<ui::Select> one = std::make_shared<ui::Select>(three());

    Stage() {
        column->children().add(one);
        window->content(column);
        top.children().add(window);
    }

    View drawn(ui::Size size = {30, 12}) { return top.render({{0, 0}, size}); }
};

}

// --- Co wybór wie o sobie ------------------------------------------------------------------------

TRIAL("Select: pusty nic nie niesie, ale ognisko bierze") {
    const ui::Select one;

    same(one.options().many(), std::size_t{0});
    ought(!one.value().has_value());
    ought(one.focusable());
    same(one.capacity(), 0);
    ought(!one.opened());
}

TRIAL("Select: świeży stoi na pierwszej opcji") {
    const ui::Select one{three()};

    same(one.options().many(), std::size_t{3});
    same(*one.value(), std::string{"UTF-8"});
}

TRIAL("Select: wybór można podać przy tworzeniu i przyciąć") {
    same(*ui::Select(three(), 2).value(), std::string{"CP1250"});
    same(*ui::Select(three(), 9).value(), std::string{"CP1250"});
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

    same(*one.value(), std::string{"raz"});
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

    stage.one->value("ISO-8859-2");

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

    ought(stage.top.dispatch(down));
    same(*stage.one->value(), std::string{"ISO-8859-2"});
    same(done, 1);

    ui::KeyEvent up{ui::Code::up};

    ought(stage.top.dispatch(up));
    same(*stage.one->value(), std::string{"UTF-8"});
    same(done, 2);

    ought(!stage.one->opened());
}

// Na krańcu klawisz **idzie wyżej**: w oknie strzałka ma wtedy przejść do następnego widżetu.
TRIAL("Select: na krańcu oddaje strzałkę wyżej") {
    Stage stage;

    stage.one->focus();

    ui::KeyEvent up{ui::Code::up};

    ought(!stage.top.dispatch(up));

    stage.one->value("CP1250");

    ui::KeyEvent down{ui::Code::down};

    ought(!stage.top.dispatch(down));
    same(*stage.one->value(), std::string{"CP1250"});
}

TRIAL("Select: Home i End skaczą na krańce") {
    Stage stage;

    stage.one->focus();

    ui::KeyEvent end{ui::Code::end};

    ought(stage.top.dispatch(end));
    same(*stage.one->value(), std::string{"CP1250"});

    ui::KeyEvent home{ui::Code::home};

    ought(stage.top.dispatch(home));
    same(*stage.one->value(), std::string{"UTF-8"});
}

// **Litery idą wyżej i to jest warunek**, żeby klawisz aktywny przycisku działał, gdy ognisko stoi
// na wyborze. Tak samo robi menu: bierze swoje, resztę puszcza.
TRIAL("Select: litery i Tab idą wyżej") {
    Stage stage;

    stage.one->focus();

    ui::KeyEvent letter{U'z'};

    ought(!stage.top.dispatch(letter));

    ui::KeyEvent tab{ui::Code::tab};

    ought(!stage.top.dispatch(tab));
}

TRIAL("Select: podanie wartości milczy") {
    Stage stage;

    int done = 0;

    counted(stage.one, done);

    stage.one->value("CP1250");

    same(*stage.one->value(), std::string{"CP1250"});
    same(done, 0);
}

// --- Rozwijanie ----------------------------------------------------------------------------------

TRIAL("Select: Enter rozwija, Esc zamyka i oddaje ognisko") {
    Stage stage;

    stage.one->focus();

    ui::KeyEvent enter{ui::Code::enter};

    ought(stage.top.dispatch(enter));
    ought(stage.one->opened());
    same(static_cast<int>(stage.window->many()), 1);

    ui::KeyEvent escape{ui::Code::escape};

    ought(stage.top.dispatch(escape));

    ought(!stage.one->opened());
    ought(stage.window->empty());
    same(stage.top.focused_node(), static_cast<ui::Node*>(stage.one.get()));
}

// **Spacja nie rozwija.** Rozwijała kiedyś, razem z Enterem — a to zabierało aplikacji klawisz,
// który w oknie z polem tekstowym należy do pisania. Tak samo straciła ją przycisk.
TRIAL("Select: spacja nie rozwija") {
    Stage stage;

    stage.one->focus();

    ui::KeyEvent space{U' '};

    ought(!stage.top.dispatch(space));
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

    const auto window = std::make_shared<ui::Modals>();
    const auto column = std::make_shared<ui::Column>();
    const auto one = std::make_shared<ui::Select>(three());

    // Wybór stoi w ostatnim wierszu ekranu.
    for (int at = 0; at < 6; ++at)
        column->children().add(std::make_shared<ui::Select>(std::vector<std::string>{"x"}));

    column->children().add(one);
    window->content(column);
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

// Ile wierszy widać naraz, mówi wołający; mniej niż jednego nie ma sensu, więc zero znaczy jeden.
TRIAL("Select: liczba wierszy listy ma dolną granicę") {
    ui::Select one{three()};

    same(one.rows(), 8);

    one.rows(0);

    same(one.rows(), 1);

    one.rows(2);

    same(one.rows(), 2);
}

// Lista dłuższa niż okno wierszy dostaje tyle wierszy, ile kazano, i suwak obok. Krótsza — tyle,
// ile ma pozycji, i bez suwaka.
TRIAL("Select: lista ma tyle wierszy, ile kazano, a dłuższa dostaje suwak") {
    Stage stage;

    stage.one->rows(2);
    stage.one->open();

    const ui::List* list = rolled(*stage.window);

    ought(list != nullptr);
    same(list->high(), 2);
    ought(list->scrollbar());

    stage.one->close();
    stage.one->rows(5);
    stage.one->open();

    list = rolled(*stage.window);

    ought(list != nullptr);
    same(list->high(), 3);
    ought(!list->scrollbar());
}

// Rozwinięta lista stoi na **bieżącej wartości** — Enter bez ruchu to wybór tego, co już jest,
// a nie skok na pierwszą pozycję.
TRIAL("Select: rozwinięta lista stoi na bieżącej wartości") {
    Stage stage;

    stage.one->value("CP1250");
    stage.one->open();

    const ui::List* list = rolled(*stage.window);

    ought(list != nullptr);
    same(list->which(), std::optional<std::size_t>{2});
}

// Zamknięcie z ręki wołającego mówi „zamknięte" od razu, choć zdjęta zasłona może jeszcze chwilę
// żyć w korzeniu.
TRIAL("Select: zamknięcie od razu mówi, że zamknięte") {
    Stage stage;

    stage.one->open();
    stage.one->close();

    ought(!stage.one->opened());
    ought(stage.window->empty());
}

TRIAL("Select: rozwinięcie dwa razy nie tworzy dwóch list") {
    Stage stage;

    stage.one->open();
    stage.one->open();

    same(static_cast<int>(stage.window->many()), 1);
}

TRIAL("Select: pusty nie ma czego rozwijać") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Select>();

    window->content(one);
    top.children().add(window);

    one->open();

    ought(!one->opened());
    ought(window->empty());
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

    ought(stage.top.dispatch(down));

    ui::KeyEvent enter{ui::Code::enter};

    ought(stage.top.dispatch(enter));

    same(*stage.one->value(), std::string{"ISO-8859-2"});
    same(done, 1);
    ought(!stage.one->opened());
}

// Wybór przychodzi, **gdy listy już nie ma** — wołający, który w odpowiedzi otwiera coś nowego albo
// pyta o `opened()`, widzi stan po zwinięciu.
TRIAL("Select: zdarzenie wyboru przychodzi przy zwiniętej liście") {
    Stage stage;

    bool open = true;

    (void)stage.one->on<ui::PickEvent>([&open, &stage](ui::PickEvent&) {
        open = stage.one->opened();

        return true;
    });

    stage.one->focus();
    stage.one->open();

    (void)stage.drawn();

    ui::KeyEvent down{ui::Code::down};

    ought(stage.top.dispatch(down));

    ui::KeyEvent enter{ui::Code::enter};

    ought(stage.top.dispatch(enter));

    ought(!open);
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

    ought(stage.top.dispatch(enter));

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

    ought(stage.top.dispatch(what));
    ought(stage.one->opened());
    ought(stage.top.focused_node() != static_cast<ui::Node*>(stage.one.get()));

    ui::KeyEvent escape{ui::Code::escape};

    ought(stage.top.dispatch(escape));

    ought(!stage.one->opened());
    same(stage.top.focused_node(), static_cast<ui::Node*>(stage.one.get()));
}

// --- Wyłączony -----------------------------------------------------------------------------------

TRIAL("Select: wyłączony nie rozwija, nie wybiera i oddaje ognisko") {
    Stage stage;

    stage.one->focus();
    stage.one->enabled(false);

    ought(!stage.one->focusable());
    ought(stage.top.focused_node() != static_cast<ui::Node*>(stage.one.get()));

    stage.one->open();

    ought(!stage.one->opened());

    ui::KeyEvent down{ui::Code::down};

    (void)stage.top.dispatch(down);

    same(*stage.one->value(), std::string{"UTF-8"});
}

TRIAL("Select: wyłączenie w trakcie zamyka rozwiniętą listę") {
    Stage stage;

    stage.one->focus();
    stage.one->open();

    ought(stage.one->opened());
    ought(stage.top.focused_node() != static_cast<ui::Node*>(stage.one.get()));

    stage.one->enabled(false);

    ought(!stage.one->opened());
    ought(stage.window->empty());

    // Ognisko siedziało w liście. Zamknięta lista oddaje je wyborowi, a ten — już wyłączony — dalej.
    ought(stage.top.focused_node() != static_cast<ui::Node*>(stage.one.get()));
    ought(!stage.one->focused());
}

namespace {

// Pojemnik, który jako widżet słucha wyłączeń w swoim poddrzewie.
class Watcher : public ui::Column {
public:
    const ui::Handler* seen = nullptr;

protected:
    bool handle(ui::DisableEvent& what) override {
        seen = what.target();

        return true;
    }
};

}

TRIAL("Select: zwinięcie listy nie zasłania wyłączenia przodkom") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto column = std::make_shared<Watcher>();
    const auto one = std::make_shared<ui::Select>(three());

    column->children().add(one);
    window->content(column);
    top.children().add(window);

    one->open();
    one->enabled(false);

    ought(!one->opened());
    same(static_cast<const void*>(column->seen), static_cast<const void*>(one.get()));
}

// --- W oknie dialogowym --------------------------------------------------------------------------

// Ognisko siedziało w rozwiniętej liście, a zamknięta lista oddaje je temu, kto je miał — wyborowi,
// który już go nie bierze. Kiedyś zostawało więc nigdzie, a dialog połyka klawisze i rozdaje
// klawisze aktywne tylko wtedy, gdy ognisko jest w nim — klawisz szedł prosto do korzenia.
TRIAL("Select: wyłączony w dialogu z rozwiniętą listą oddaje ognisko dialogowi") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto one = std::make_shared<ui::Select>(three());

    column->children().add(one);
    dialog->children().add(column);
    top.children().add(window);
    window->open(dialog);
    (void)top.render({{0, 0}, {30, 12}});

    one->focus();
    one->open();
    one->enabled(false);

    ought(!one->opened());
    same(static_cast<const void*>(top.focused_node()), static_cast<const void*>(dialog.get()));

    ui::KeyEvent key{ui::Key{U'x'}};

    ought(top.dispatch(key));
}

// Wybór jest **jednym przystankiem** wędrówki, a rozwinięta lista jej nie psuje.
TRIAL("Select: w oknie jest jednym przystankiem Taba") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto one = std::make_shared<ui::Select>(three());
    const auto other = std::make_shared<ui::Select>(three());

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

// Rozwinięta lista **leży nad oknem dialogowym**, bo obie są zasłonami w tym samym oknie, a stos
// rozstrzyga kolejność. Zamknięcie listy oddaje ognisko oknu, a nie aplikacji.
TRIAL("Select: lista rozwinięta w oknie zamyka się nad nim i oddaje mu ognisko") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto one = std::make_shared<ui::Select>(three());

    column->children().add(one);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    (void)top.render({{0, 0}, {30, 12}});

    one->open();

    same(static_cast<int>(window->many()), 2);
    ought(one->opened());
    ought(window->current() != static_cast<ui::Modal*>(dialog.get()));

    ui::KeyEvent escape{ui::Code::escape};

    ought(top.dispatch(escape));

    same(static_cast<int>(window->many()), 1);
    same(top.focused_node(), static_cast<ui::Node*>(one.get()));
}

