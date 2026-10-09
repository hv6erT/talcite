#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/encoding.hpp"
#include "talcite/event.hpp"
#include "talcite/frame.hpp"
#include "talcite/root.hpp"
#include "talcite/row.hpp"
#include "talcite/window.hpp"
#include "talcite/windows.hpp"

using namespace talcite;

namespace {

// Pudełko, które bierze ognisko i wie, że w nie kliknięto — tyle wystarczy, żeby sprawdzić
// warstwy, ognisko i mysz.
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

// Najmniejsze potrzebne otoczenie: korzeń z oknami w środku.
struct Stage {
    ui::Root top;
    std::shared_ptr<ui::Windows> window = std::make_shared<ui::Windows>();

    Stage() { top.children().add(window); }

    std::shared_ptr<ui::Window> opened(char32_t what) {
        const auto one = std::make_shared<ui::Window>();

        one->children().add(std::make_shared<Box>(ui::Size{10, 3}, what));
        window->open(one);

        return one;
    }
};

std::string read(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x)
        if (const char32_t letter = one.at({x, y}).letter; letter != 0) talcite::utf8::encode(letter, made);

    return made;
}

ui::Node* inside(const ui::Window& one) { return one.children().empty() ? nullptr : one.children()[0].get(); }

}

// --- Warstwa i bieżące okno ----------------------------------------------------------------------

// Okno niemodalne nie zasłania niczego samo z siebie: pełne bierze cały pokój, postawione tylko
// swój prostokąt, a ostatnie otwarte jest **bieżące** — czyli na wierzchu i z ogniskiem.
TRIAL("Window: ostatnie otwarte jest bieżące, ognisko idzie w nie") {
    Stage stage;

    const auto one = stage.opened(U'a');

    ought(one->current());
    ought(stage.window->current() == one.get());
    same(stage.top.focused_node(), inside(*one));
    same(stage.window->many(), std::size_t{1});

    const auto two = stage.opened(U'b');

    ought(two->current());
    ought(!one->current());
    same(stage.top.focused_node(), inside(*two));
    same(stage.window->many(), std::size_t{2});
}

// Pełne okno maluje na całym pokoju; postawione stoi tam, gdzie mu kazano, i spod niego widać to,
// co niżej.
TRIAL("Window: pełne bierze cały pokój, postawione swój prostokąt") {
    Stage stage;

    const auto under = stage.opened(U'a');
    const auto over = stage.opened(U'b');

    ought(under->full());

    over->seat({2, 1}, {4, 2});

    ought(!over->full());
    same(over->seat().at.x, 2);

    const View made = stage.top.render({{0, 0}, {10, 3}});

    same(read(made, 0), std::string{"aaaaaaaaaa"});
    same(read(made, 1), std::string{"aabbbbaaaa"});
    same(read(made, 2), std::string{"aabbbbaaaa"});

    // Z powrotem na pełne: znów cały pokój.
    over->full(true);

    same(read(stage.top.render({{0, 0}, {10, 3}}), 0), std::string{"bbbbbbbbbb"});
}

// Podniesienie przenosi okno na wierzch **i oddaje mu ognisko tam, gdzie było** — plik, po którym
// się chodziło, zostaje tam, gdzie się go zostawiło.
TRIAL("Window: podniesienie wraca z ogniskiem tam, gdzie było") {
    Stage stage;

    const auto one = stage.opened(U'a');

    // Dwa chętne pudełka w pierwszym oknie; ognisko stawiamy na **drugim**, żeby powrót do
    // pierwszego chętnego nie mógł udawać pamięci.
    const auto first = std::make_shared<Box>(ui::Size{4, 1}, U'd');
    const auto other = std::make_shared<Box>(ui::Size{4, 1}, U'c');

    one->children().clear();

    const auto both = std::make_shared<ui::Row>();

    both->children().add(first);
    both->children().add(other);
    one->children().add(both);

    other->focus();

    const auto two = stage.opened(U'b');

    same(stage.top.focused_node(), inside(*two));

    one->raise();

    ought(one->current());
    same(stage.top.focused_node(), static_cast<ui::Node*>(other.get()));

    two->raise();

    same(stage.top.focused_node(), inside(*two));
}

// --- Kolejność i zamykanie -----------------------------------------------------------------------

// `next` i `previous` chodzą po **kolejności otwarcia**, nie po warstwach: inaczej „następne okno"
// skakałoby po historii patrzenia i dwa naciśnięcia wracałyby w to samo miejsce.
TRIAL("Window: next i previous chodzą po kolejności otwarcia, w kółko") {
    Stage stage;

    const auto one = stage.opened(U'a');
    const auto two = stage.opened(U'b');
    const auto three = stage.opened(U'c');

    ought(three->current());

    stage.window->next();
    ought(one->current());

    stage.window->next();
    ought(two->current());

    stage.window->previous();
    ought(one->current());

    stage.window->previous();
    ought(three->current());

    // Po podniesieniu ręką kolejność chodzenia się nie zmienia.
    one->raise();
    stage.window->next();
    ought(two->current());
}

// Zamknięcie ogłasza się, **póki okno jeszcze stoi w drzewie**, a po nim bieżące staje się to,
// co zostało na wierzchu — razem z ogniskiem. Jedno okno w kółko to ono samo.
TRIAL("Window: zamknięcie ogłasza się i oddaje ognisko wierzchniemu") {
    Stage stage;

    const auto one = stage.opened(U'a');
    const auto two = stage.opened(U'b');

    int said = 0;
    bool stood = false;

    // Okno w uchwycie gołym wskazaniem: uchwyt mieszka w samym oknie, więc mocne wskazanie
    // trzymałoby je przy życiu na zawsze (sanitizer: wyciek z tej próby).
    (void)two->on<ui::CloseEvent>([&said, &stood, window = two.get()](ui::CloseEvent&) {
        ++said;
        stood = window->root() != nullptr;

        return false;
    });

    two->close();

    same(said, 1);
    ought(stood);
    same(stage.window->many(), std::size_t{1});
    ought(one->current());
    same(stage.top.focused_node(), inside(*one));

    // Jeden — `next` nie ma dokąd pójść.
    stage.window->next();
    ought(one->current());

    one->close();

    same(stage.window->many(), std::size_t{0});
    ought(stage.window->current() == nullptr);
}

// Kliknięcie w okno podnosi je, ale go **nie połyka**. Dochodzi przy tym tylko to, czego nie
// wzięła treść: kliknięcie w ramkę albo w margines podnosi, a kliknięcie w sam tekst obsługuje
// tekst i okno o nim nie wie. Po to okno oddaje fałsz: gdyby brało, klik w tle podnosiłby okno
// **zamiast** stawiać kursor.
TRIAL("Window: kliknięcie w ramkę podnosi, a treść i tak dostaje swoje") {
    Stage stage;

    const auto one = std::make_shared<ui::Window>();
    const auto framed = std::make_shared<ui::Frame>();
    const auto box = std::make_shared<Box>(ui::Size{8, 1}, U'a');

    framed->children().add(box);
    one->children().add(framed);
    stage.window->open(one);

    const auto two = stage.opened(U'b');

    two->seat({6, 0}, {4, 3});

    (void)stage.top.render({{0, 0}, {10, 3}});

    ought(two->current());

    // Klik w ramkę pierwszego okna: ramka niczego nie bierze, więc dochodzi do okna.
    ui::ClickEvent edge{{0, 0}, ui::Mouse::left};

    (void)stage.top.dispatch(edge);

    ought(one->current());
    same(box->hit, 0);

    // Klik w treść: bierze ją treść, a okno o nim nie wie.
    two->raise();

    ui::ClickEvent mine{{1, 1}, ui::Mouse::left};

    (void)stage.top.dispatch(mine);

    same(box->hit, 1);
    ought(two->current());
}

// --- Przesuwanie i zmiana rozmiaru ---------------------------------------------------------------

// Przesunięcie jak `WindowMove` w mcedicie: górny wiersz zostaje w pokoju, a z każdej strony
// zostaje choć jedna kolumna. Pokój bierze się z ostatniej klatki pojemnika.
TRIAL("Window: przesunięcie trzyma górny wiersz i jedną kolumnę w pokoju") {
    Stage stage;

    const auto one = stage.opened(U'a');

    one->seat({2, 1}, {4, 2});

    (void)stage.top.render({{0, 0}, {10, 5}});

    one->move(3, 2);
    same(one->seat().at.x, 5);
    same(one->seat().at.y, 3);

    // W prawo do ostatniej kolumny, w dół do ostatniego wiersza — i ani kratki dalej.
    one->move(100, 100);
    same(one->seat().at.x, 9);
    same(one->seat().at.y, 4);

    // W lewo tak daleko, że z okna zostaje jedna kolumna; w górę do pierwszego wiersza.
    one->move(-100, -100);
    same(one->seat().at.x, -3);
    same(one->seat().at.y, 0);

    // Rozmiar przy przesuwaniu stoi.
    same(one->seat().size.wide, 4);
    same(one->seat().size.high, 2);
}

// Zmiana rozmiaru jak `WindowResize`: lewy górny róg stoi, okno rośnie tylko do prawego i dolnego
// brzegu, a maleje najwyżej do `least`.
TRIAL("Window: zmiana rozmiaru do brzegu pokoju i nie mniej niż least") {
    Stage stage;

    const auto one = stage.opened(U'a');

    one->seat({2, 1}, {4, 2});
    one->least({3, 2});

    same(one->least().wide, 3);

    (void)stage.top.render({{0, 0}, {10, 5}});

    one->resize(1, 1);
    same(one->seat().size.wide, 5);
    same(one->seat().size.high, 3);

    one->resize(100, 100);
    same(one->seat().size.wide, 8);
    same(one->seat().size.high, 4);

    one->resize(-100, -100);
    same(one->seat().size.wide, 3);
    same(one->seat().size.high, 2);

    // Róg stoi.
    same(one->seat().at.x, 2);
    same(one->seat().at.y, 1);

    // Najmniejsze okno ma choć jedną kratkę.
    one->least({0, -4});
    same(one->least().wide, 1);
    same(one->least().high, 1);
}

// Okno, które już stoi za granicą — bo terminal zwęził się pod nim — **nie skacze do niej**:
// krok na zewnątrz nie robi nic, a krok do środka idzie o jedną kratkę. Tak liczy mcedit: warunek
// pyta o miejsce, w którym okno stoi, a nie przycina wyniku.
TRIAL("Window: okno za granicą nie skacze, tylko wraca krok po kroku") {
    Stage stage;

    const auto one = stage.opened(U'a');

    one->seat({15, 8}, {12, 6});

    (void)stage.top.render({{0, 0}, {10, 5}});

    one->move(1, 1);
    same(one->seat().at.x, 15);
    same(one->seat().at.y, 8);

    one->move(-1, -1);
    same(one->seat().at.x, 14);
    same(one->seat().at.y, 7);

    // Za szerokie i za wysokie: rosnąć nie może, maleć może o krok.
    one->resize(1, 1);
    same(one->seat().size.wide, 12);
    same(one->seat().size.high, 6);

    one->resize(-1, -1);
    same(one->seat().size.wide, 11);
    same(one->seat().size.high, 5);

    // Tak samo z drugiej strony: nad górnym brzegiem i węższe niż `least`. W górę i w lewo nie
    // idzie dalej, maleć nie maleje — ale też nie skacze do granicy.
    one->seat({-20, -2}, {2, 1});
    one->least({3, 2});

    one->move(-1, -1);
    same(one->seat().at.x, -20);
    same(one->seat().at.y, -2);

    one->resize(-1, -1);
    same(one->seat().size.wide, 2);
    same(one->seat().size.high, 1);
}

// Pełne okno nie ma czego przesuwać, a okno bez pokoju — jeszcze nie rysowane — nie ma względem
// czego liczyć granic. Oba stoją.
TRIAL("Window: pełne i nierysowane stoją") {
    Stage stage;

    const auto one = stage.opened(U'a');

    one->seat({2, 1}, {4, 2});

    one->move(1, 1);
    one->move(-1, -1);
    one->resize(1, 1);
    one->resize(-1, -1);
    same(one->seat().at.x, 2);
    same(one->seat().at.y, 1);
    same(one->seat().size.wide, 4);
    same(one->seat().size.high, 2);

    (void)stage.top.render({{0, 0}, {10, 5}});

    one->full(true);
    one->move(1, 1);
    one->resize(1, 1);
    same(one->seat().at.x, 2);
    same(one->seat().at.y, 1);
    same(one->seat().size.wide, 4);
    same(one->seat().size.high, 2);

    // Okno poza pojemnikiem też stoi.
    ui::Window alone;

    alone.seat({1, 1}, {2, 2});
    alone.move(1, 1);
    alone.resize(1, 1);
    same(alone.seat().at.x, 1);
    same(alone.seat().size.wide, 2);
}

// Pełne okno chce całego pokoju, postawione — swojego miejsca, niezależnie od pokoju.
TRIAL("Window: pełne chce pokoju, postawione swojego rozmiaru") {
    ui::Window one;

    same(one.wants({10, 5}).wide, 10);
    same(one.wants({10, 5}).high, 5);

    one.seat({1, 1}, {4, 2});

    same(one.wants({10, 5}).wide, 4);
    same(one.wants({10, 5}).high, 2);
}

namespace {

// Węzeł, który liczy kliknięcia, jakie do niego doszły — żeby sprawdzić, że okno ich nie połyka.
class Catcher : public ui::Node {
public:
    int hit = 0;

protected:
    bool handle(ui::ClickEvent&) override {
        ++hit;

        return true;
    }
};

}

// **Okno kliknięcia nie połyka**: podnosi się i puszcza je dalej, w górę drzewa — niemodalne okno
// nie ma prawa zjadać myszy temu, co stoi nad nim.
TRIAL("Window: kliknięcie w okno idzie dalej w górę") {
    ui::Root top;

    const auto catcher = std::make_shared<Catcher>();
    const auto window = std::make_shared<ui::Windows>();

    catcher->children().add(window);
    top.children().add(catcher);

    const auto one = std::make_shared<ui::Window>();

    one->children().add(std::make_shared<ui::Frame>());
    window->open(one);

    (void)top.render({{0, 0}, {10, 3}});

    ui::ClickEvent edge{{0, 0}, ui::Mouse::left};

    (void)top.dispatch(edge);

    same(catcher->hit, 1);
}

// --- Ciągnięcie myszą ----------------------------------------------------------------------------

namespace {

void mouse(ui::Root& top, ui::Spot where, ui::Press press, ui::Mouse which = ui::Mouse::left) {
    ui::MouseEvent what{where, press, which};

    (void)top.dispatch(what);
}

}

// Wciśnięcie na górnym wierszu chwyta położenie, a okno idzie za kursorem **w tym samym miejscu
// chwytu**. Kursor za granicą ciągnie okno do granicy, a wracając — z powrotem tam, gdzie je
// trzyma; mysz dochodzi przy tym do okna także spoza niego, bo okno prosi o przytrzymanie.
TRIAL("Window: ciągnięcie za górny wiersz przesuwa okno, chwyt zostaje na miejscu") {
    Stage stage;

    const auto one = stage.opened(U'a');

    one->seat({2, 1}, {6, 3});

    (void)stage.top.render({{0, 0}, {20, 10}});

    mouse(stage.top, {4, 1}, ui::Press::down);

    ought(one->dragged());

    mouse(stage.top, {10, 5}, ui::Press::move);
    same(one->seat().at.x, 8);
    same(one->seat().at.y, 5);

    mouse(stage.top, {100, 100}, ui::Press::move);
    same(one->seat().at.x, 19);
    same(one->seat().at.y, 9);

    mouse(stage.top, {10, 5}, ui::Press::move);
    same(one->seat().at.x, 8);
    same(one->seat().at.y, 5);

    // Rozmiar przy przesuwaniu stoi.
    same(one->seat().size.wide, 6);
    same(one->seat().size.high, 3);

    mouse(stage.top, {12, 7}, ui::Press::up);

    ought(!one->dragged());

    mouse(stage.top, {15, 8}, ui::Press::move, ui::Mouse::none);
    same(one->seat().at.x, 8);
    same(one->seat().at.y, 5);
}

// Wciśnięcie w prawym dolnym rogu chwyta rozmiar: róg idzie za kursorem, nie mniej niż `least`.
TRIAL("Window: ciągnięcie za prawy dolny róg zmienia rozmiar") {
    Stage stage;

    const auto one = stage.opened(U'a');

    one->seat({2, 1}, {6, 3});
    one->least({3, 2});

    (void)stage.top.render({{0, 0}, {20, 10}});

    mouse(stage.top, {7, 3}, ui::Press::down);

    ought(one->dragged());

    mouse(stage.top, {11, 6}, ui::Press::move);
    same(one->seat().size.wide, 10);
    same(one->seat().size.high, 6);

    mouse(stage.top, {0, 0}, ui::Press::move);
    same(one->seat().size.wide, 3);
    same(one->seat().size.high, 2);

    // Róg lewy górny stoi.
    same(one->seat().at.x, 2);
    same(one->seat().at.y, 1);

    mouse(stage.top, {0, 0}, ui::Press::up);

    ought(!one->dragged());
}

// Wciśnięcie gdzie indziej podnosi okno, ale go nie chwyta; prawy przycisk nie chwyta wcale,
// pełne okno — też nie.
TRIAL("Window: środek podnosi bez chwytu, prawy przycisk i pełne okno nie chwytają") {
    Stage stage;

    const auto one = stage.opened(U'a');
    const auto two = stage.opened(U'b');

    one->seat({0, 0}, {6, 4});
    two->seat({8, 0}, {6, 4});

    (void)stage.top.render({{0, 0}, {20, 10}});

    ought(two->current());

    mouse(stage.top, {2, 2}, ui::Press::down);

    ought(one->current());
    ought(!one->dragged());

    mouse(stage.top, {5, 5}, ui::Press::move);
    same(one->seat().at.x, 0);
    same(one->seat().at.y, 0);

    mouse(stage.top, {5, 5}, ui::Press::up);

    // Prawa krawędź i dolna krawędź to jeszcze nie róg.
    mouse(stage.top, {5, 2}, ui::Press::down);
    ought(!one->dragged());
    mouse(stage.top, {5, 2}, ui::Press::up);

    mouse(stage.top, {2, 3}, ui::Press::down);
    ought(!one->dragged());
    mouse(stage.top, {2, 3}, ui::Press::up);

    mouse(stage.top, {9, 0}, ui::Press::down, ui::Mouse::right);

    ought(!two->dragged());
    ought(one->current());

    mouse(stage.top, {9, 0}, ui::Press::up, ui::Mouse::right);

    two->full(true);

    (void)stage.top.render({{0, 0}, {20, 10}});

    mouse(stage.top, {9, 0}, ui::Press::down);

    ought(two->current());
    ought(!two->dragged());
}

// Zgubione puszczenie nie zostawia okna chwyconego: nowe wciśnięcie gdzie indziej zaczyna od nowa
// i ruch już okna nie niesie.
TRIAL("Window: nowe wciśnięcie po zgubionym puszczeniu nie ciągnie dalej") {
    Stage stage;

    const auto one = stage.opened(U'a');

    one->seat({2, 1}, {6, 3});

    (void)stage.top.render({{0, 0}, {20, 10}});

    mouse(stage.top, {4, 1}, ui::Press::down);

    ought(one->dragged());

    mouse(stage.top, {4, 2}, ui::Press::down);

    ought(!one->dragged());

    mouse(stage.top, {12, 7}, ui::Press::move);

    same(one->seat().at.x, 2);
    same(one->seat().at.y, 1);
}

namespace {

// Węzeł nad oknami, który liczy surowe zdarzenia myszy, jakie do niego doszły.
class Watcher : public ui::Node {
public:
    int heard = 0;

protected:
    bool handle(ui::MouseEvent&) override {
        ++heard;

        return true;
    }
};

}

// Ciągnięcie należy do okna: wciśnięcie, ruch i puszczenie nie idą wyżej. Wciśnięcie, którego okno
// nie chwyciło, idzie dalej, jak kliknięcie.
TRIAL("Window: ciągnięcie nie wychodzi poza okno") {
    ui::Root top;

    const auto watcher = std::make_shared<Watcher>();
    const auto window = std::make_shared<ui::Windows>();

    watcher->children().add(window);
    top.children().add(watcher);

    const auto one = std::make_shared<ui::Window>();

    one->children().add(std::make_shared<ui::Frame>());
    window->open(one);
    one->seat({2, 1}, {6, 3});

    (void)top.render({{0, 0}, {20, 10}});

    mouse(top, {4, 1}, ui::Press::down);
    mouse(top, {8, 4}, ui::Press::move);
    mouse(top, {8, 4}, ui::Press::up);

    same(one->seat().at.x, 6);
    same(watcher->heard, 0);

    (void)top.render({{0, 0}, {20, 10}});

    // Środek okna, które stoi teraz w (6, 1): okno go nie chwyta, więc wciśnięcie idzie wyżej.
    mouse(top, {8, 2}, ui::Press::down);

    ought(!one->dragged());
    same(watcher->heard, 1);
}
