#include "proving.hpp"

#include <memory>

#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/root.hpp"
#include "talcite/modal.hpp"
#include "talcite/modals.hpp"
#include "talcite/row.hpp"

using namespace talcite;

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

// Zasłona, która stawia pudełko tam, gdzie jej każą, zamiast na środku. Po to `seat()` jest
// wirtualne: to jedyne, czym zasłony różnią się między sobą.
class Aside : public ui::Modal {
public:
    explicit Aside(ui::Spot where) : ui::Modal(ui::ModalShape{false}), where_(where) {}

protected:
    [[nodiscard]] ui::Area seat(ui::Size given) const override {
        if (children().empty()) return {};

        ui::Size wants = children().front()->wants(given);

        return {where_, wants};
    }

private:
    ui::Spot where_;
};

// Najmniejsze potrzebne otoczenie: okno z treścią pod spodem i zasłona nad nią. Treścią jest
// wiersz, żeby próba mogła dostawić do niej drugi węzeł obok pierwszego.
struct Stage {
    ui::Root top;
    std::shared_ptr<ui::Modals> window = std::make_shared<ui::Modals>();
    std::shared_ptr<ui::Row> under = std::make_shared<ui::Row>();
    std::shared_ptr<Box> body = std::make_shared<Box>(ui::Size{40, 10}, U'.');

    Stage() {
        under->children().add(body);
        window->content(under);
        top.children().add(window);
        body->focus();
    }
};

}

// --- Co zasłona wie o sobie ----------------------------------------------------------------------

// Zasłona **jest ekranem**, choć widać na niej samo pudełko. Z tego jednego faktu bierze się cała
// modalność, więc pilnujemy go wprost.
TRIAL("Modal: zajmuje wszystko, co dostała, i leży nad rodzeństwem") {
    const ui::Modal one;

    same(one.wants({40, 10}), (ui::Size{40, 10}));
    same(one.capacity(), 1);
    ought(one.focusable());
}

TRIAL("Modal: pudełko staje na środku") {
    Stage stage;

    const auto one = std::make_shared<ui::Modal>(ui::ModalShape{false});

    one->children().add(std::make_shared<Box>(ui::Size{10, 4}, U'o'));

    stage.window->open(one);

    const View made = stage.top.render({{0, 0}, {40, 10}});

    // (40−10)/2 = 15, (10−4)/2 = 3.
    same(made.at({15, 3}).letter, U'o');
    same(made.at({24, 6}).letter, U'o');
    same(made.at({14, 3}).letter, U'.');
}

// --- Modalność -----------------------------------------------------------------------------------

TRIAL("Modal: kliknięcie obok pudełka nie dochodzi pod spód") {
    Stage stage;

    const auto one = std::make_shared<ui::Modal>(ui::ModalShape{false});

    one->children().add(std::make_shared<Box>(ui::Size{10, 4}, U'o'));

    stage.window->open(one);

    (void)stage.top.render({{0, 0}, {40, 10}});

    ui::ClickEvent what{{1, 1}, ui::Mouse::left};

    ought(stage.top.dispatch(what));
    same(stage.body->hit, 0);
}

// **Escape'a zasłona nie bierze.** Zamykał okno — czyli wykonywał czynność, której wołający nie
// miał jak odwołać. Połyka go jak każdy inny klawisz, bo połykanie niczego nie robi, a modalność
// inaczej się nie wyrazi.
TRIAL("Modal: Esc niczego nie zamyka, tylko zostaje połknięty") {
    Stage stage;

    const auto one = std::make_shared<ui::Modal>();
    const auto inside = std::make_shared<Box>(ui::Size{6, 3}, U'o');

    one->children().add(inside);

    stage.window->open(one);

    same(stage.top.focused_node(), static_cast<ui::Node*>(inside.get()));

    ui::KeyEvent what{ui::Code::escape};

    ought(stage.top.dispatch(what));

    ought(!stage.window->empty());
    same(stage.top.focused_node(), static_cast<ui::Node*>(inside.get()));
}

// Kto chce „Esc zamyka", wiesza uchwyt — i wtedy działa, bo uchwyt widzi klawisz niezależnie od
// tego, że zasłona go połyka.
TRIAL("Modal: zamykanie Escapem da się dołożyć uchwytem") {
    Stage stage;

    const auto one = std::make_shared<ui::Modal>();
    const auto inside = std::make_shared<Box>(ui::Size{6, 3}, U'o');

    one->children().add(inside);

    stage.window->open(one);

    const std::weak_ptr<ui::Modal> weak = one;

    (void)one->on<ui::KeyEvent>([weak](ui::KeyEvent& what) {
        if (what.key().code() != ui::Code::escape) return false;

        if (const auto mine = weak.lock()) mine->close();

        return true;
    });

    ui::KeyEvent what{ui::Code::escape};

    ought(stage.top.dispatch(what));

    ought(stage.window->empty());
    same(stage.top.focused_node(), static_cast<ui::Node*>(stage.body.get()));
}

TRIAL("Modal: Tab wodzi ogniskiem po chętnych i zawija") {
    Stage stage;

    const auto one = std::make_shared<ui::Modal>();
    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Box>(ui::Size{6, 1}, U'a');
    const auto second = std::make_shared<Box>(ui::Size{6, 1}, U'b');

    column->children().add(first);
    column->children().add(second);
    one->children().add(column);

    stage.window->open(one);

    same(stage.top.focused_node(), static_cast<ui::Node*>(first.get()));

    ui::KeyEvent tab{ui::Code::tab};

    ought(stage.top.dispatch(tab));
    same(stage.top.focused_node(), static_cast<ui::Node*>(second.get()));

    ought(stage.top.dispatch(tab));
    same(stage.top.focused_node(), static_cast<ui::Node*>(first.get()));
}

// --- Podział ------------------------------------------------------------------------------------

// **To jest ta próba, dla której cały podział.** Enter naciskający przycisk podstawowy to reguła
// formularza, nie zasłony. Goła zasłona ma go połknąć i nie robić nic — inaczej rozwinięta lista
// zatwierdzałaby cudze okno tym samym klawiszem, którym wybiera pozycję.
TRIAL("Modal: Enter nie naciska przycisku podstawowego") {
    Stage stage;

    const auto one = std::make_shared<ui::Modal>();
    const auto press = std::make_shared<ui::Button>("OK");

    int done = 0;

    press->primary(true);

    (void)press->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });

    one->children().add(press);

    stage.window->open(one);

    // Ognisko odsuwamy z przycisku na samą zasłonę: przycisk z ogniskiem zna Enter sam z siebie,
    // a próba jest o tym, co robi z Enterem **okno**.
    one->focus();

    ui::KeyEvent what{ui::Code::enter};

    ought(stage.top.dispatch(what));
    same(done, 0);
}

// A w dialogu — czyli w zasłonie formularzowej — jest **tak samo**. Oznaczenie podstawowego rysuje
// klamrę ze strzałkami i na tym kończy się jego rola; okno nie szuka przycisku po drzewie i nie
// naciska go za wołającego. Enter należy do tego, kto ma ognisko.
TRIAL("Dialog: Enter też nie naciska przycisku podstawowego") {
    Stage stage;

    const auto one = std::make_shared<ui::Dialog>();
    const auto press = std::make_shared<ui::Button>("OK");

    int done = 0;

    press->primary(true);

    (void)press->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });

    one->children().add(press);

    stage.window->open(one);

    one->focus();

    ui::KeyEvent what{ui::Code::enter};

    // Zasłona klawisz połyka, bo jest modalna — ale nic nim nie naciska.
    ought(stage.top.dispatch(what));
    same(done, 0);
}

// --- Własne położenie pudełka --------------------------------------------------------------------

// Zasłona pochodna stawia pudełko tam, gdzie każe `seat()` — i **nic poza tym się nie zmienia**:
// przezroczystość, modalność i zamykanie zostają.
TRIAL("Modal: pochodna stawia pudełko tam, gdzie każe box") {
    Stage stage;

    const auto one = std::make_shared<Aside>(ui::Spot{3, 6});

    one->children().add(std::make_shared<Box>(ui::Size{8, 2}, U'o'));

    stage.window->open(one);

    const View made = stage.top.render({{0, 0}, {40, 10}});

    same(made.at({3, 6}).letter, U'o');
    same(made.at({10, 7}).letter, U'o');

    // Poza pudełkiem dalej widać to, co pod spodem.
    same(made.at({2, 6}).letter, U'.');
    same(made.at({3, 5}).letter, U'.');
}

TRIAL("Modal: pochodna dalej połyka kliknięcia obok i Escape") {
    Stage stage;

    const auto one = std::make_shared<Aside>(ui::Spot{3, 6});

    one->children().add(std::make_shared<Box>(ui::Size{8, 2}, U'o'));

    stage.window->open(one);

    (void)stage.top.render({{0, 0}, {40, 10}});

    ui::ClickEvent what{{30, 1}, ui::Mouse::left};

    ought(stage.top.dispatch(what));
    same(stage.body->hit, 0);

    ui::KeyEvent esc{ui::Code::escape};

    ought(stage.top.dispatch(esc));
    ought(!stage.window->empty());
}

// Uchwyt `CloseEvent` ma prawo otworzyć następną zasłonę — tak robi ekran, gdy po OK w oknie
// opcji nie da się wczytać skórki i trzeba to powiedzieć. Otwarcie przesuwa wektor zasłon w trakcie
// zdejmowania, a nowa zasłona zapamiętała ognisko w oknie, które właśnie schodzi z drzewa: obie
// rzeczy trzeba przeżyć — zamykana schodzi, nowa zostaje z ogniskiem, a po jej zamknięciu ognisko
// wraca tam, skąd wyszło pierwsze okno.
TRIAL("Modal: uchwyt zamknięcia może otworzyć następną zasłonę, a ognisko wraca po obu") {
    Stage stage;

    // Kilka zasłon otwartych i zamkniętych z rzędu, żeby wektor miał co przesuwać.
    for (int round = 0; round < 3; ++round) {
        const auto first = std::make_shared<ui::Modal>();
        const auto inside = std::make_shared<Box>(ui::Size{6, 3}, U'o');

        first->children().add(inside);

        stage.window->open(first);

        same(stage.top.focused_node(), static_cast<ui::Node*>(inside.get()));

        std::shared_ptr<ui::Modal> second;

        (void)first->on<ui::CloseEvent>([&stage, &second](ui::CloseEvent&) {
            second = std::make_shared<ui::Modal>();
            second->children().add(std::make_shared<Box>(ui::Size{4, 2}, U'x'));

            stage.window->open(second);

            return false;
        });

        first->close();

        ought(second != nullptr);
        same(stage.window->many(), std::size_t{1});
        same(stage.window->current(), second.get());
        ought(first->parent() == nullptr);
        ought(stage.top.focused_node() != nullptr);
        ought(stage.top.focused_node() != static_cast<ui::Node*>(stage.body.get()));

        second->close();

        ought(stage.window->empty());
        same(stage.top.focused_node(), static_cast<ui::Node*>(stage.body.get()));
    }
}

// Zdjęcie niższej zasłony w stosie zostawia ognisko w wierzchniej — nie ściąga go do treści pod
// spodem, bo ta jest wciąż zasłonięta.
TRIAL("Modal: zdjęcie niższej zasłony zostawia ognisko wierzchniej") {
    Stage stage;

    const auto lower = std::make_shared<ui::Modal>();
    const auto upper = std::make_shared<ui::Modal>();
    const auto top = std::make_shared<Box>(ui::Size{4, 2}, U'x');

    lower->children().add(std::make_shared<Box>(ui::Size{6, 3}, U'o'));
    upper->children().add(top);

    stage.window->open(lower);
    stage.window->open(upper);

    same(stage.top.focused_node(), static_cast<ui::Node*>(top.get()));

    lower->close();

    same(stage.window->many(), std::size_t{1});
    same(stage.top.focused_node(), static_cast<ui::Node*>(top.get()));

    upper->close();

    same(stage.top.focused_node(), static_cast<ui::Node*>(stage.body.get()));
}

// --- Zasłona jest modalna także dla ogniska ------------------------------------------------------

// Ta sama treść drugi raz nie jest zmianą. Wyjęcie i włożenie z powrotem zabrałoby jej ognisko,
// a wołający, który ustawia treść „na pewno", nie spodziewa się niczego.
TRIAL("Modals: ta sama treść drugi raz nie zabiera ogniska") {
    Stage stage;

    stage.window->content(stage.under);

    same(stage.window->content(), static_cast<ui::Node*>(stage.under.get()));
    same(stage.top.focused_node(), static_cast<ui::Node*>(stage.body.get()));
}

// Węzeł pod otwartą zasłoną może poprosić o ognisko w każdej chwili — w twa robi to kartka
// podglądu, gdy plik dojdzie z puli, choć nad nią wisi już komunikat o błędzie. Prośba nie jest
// spełniana od razu, bo klawisze poszłyby pod okno, którego nie dałoby się wtedy zamknąć ani
// Enterem, ani Escapem. Staje się za to tym, **dokąd ognisko wróci po zamknięciu** zasłony.
TRIAL("Modal: prośba o ognisko spod zasłony wraca dopiero po jej zamknięciu") {
    Stage stage;

    const auto other = std::make_shared<Box>(ui::Size{5, 2}, U'y');

    stage.under->children().add(other);

    const auto one = std::make_shared<ui::Modal>();
    const auto inside = std::make_shared<Box>(ui::Size{4, 2}, U'x');

    one->children().add(inside);
    stage.window->open(one);

    same(stage.top.focused_node(), static_cast<ui::Node*>(inside.get()));

    // Prośba z treści: ognisko zostaje w zasłonie.
    other->focus();

    same(stage.top.focused_node(), static_cast<ui::Node*>(inside.get()));

    // Po zamknięciu ognisko idzie tam, gdzie prosił ostatni chętny — nie tam, skąd je wzięto.
    one->close();

    same(stage.top.focused_node(), static_cast<ui::Node*>(other.get()));
}

// Przykryty węzeł nie dostaje przy tym **żadnego zdarzenia ogniska**: mignięcie `FocusEvent`
// i `UnfocusEvent` przestawiłoby panelom czynny panel, bo one śledzą go właśnie tak.
TRIAL("Modal: wstrzymana prośba nie posyła zdarzeń ogniska") {
    Stage stage;

    const auto other = std::make_shared<Box>(ui::Size{5, 2}, U'y');

    stage.under->children().add(other);

    int came = 0;
    int went = 0;

    (void)other->on<ui::FocusEvent>([&came](ui::FocusEvent&) {
        ++came;

        return false;
    });

    (void)other->on<ui::UnfocusEvent>([&went](ui::UnfocusEvent&) {
        ++went;

        return false;
    });

    const auto one = std::make_shared<ui::Modal>();

    one->children().add(std::make_shared<Box>(ui::Size{4, 2}, U'x'));
    stage.window->open(one);

    other->focus();

    same(came, 0);
    same(went, 0);

    one->close();

    same(came, 1);
    same(went, 0);
}

// Prośba z niższej zasłony idzie do wierzchniej: to ona zejdzie pierwsza i to ona odda ognisko.
// Prośba z wierzchniej zasłony jest zwykłą prośbą — nic jej nie wstrzymuje.
TRIAL("Modal: prośba z niższej zasłony czeka, z wierzchniej działa od razu") {
    Stage stage;

    const auto lower = std::make_shared<ui::Modal>();
    const auto under = std::make_shared<Box>(ui::Size{6, 3}, U'o');
    const auto beside = std::make_shared<Box>(ui::Size{6, 3}, U'p');

    const auto twain = std::make_shared<ui::Column>();

    twain->children().add(under);
    twain->children().add(beside);
    lower->children().add(twain);

    const auto upper = std::make_shared<ui::Modal>();
    const auto top = std::make_shared<Box>(ui::Size{4, 2}, U'x');
    const auto next = std::make_shared<Box>(ui::Size{4, 2}, U'z');

    const auto pair = std::make_shared<ui::Column>();

    pair->children().add(top);
    pair->children().add(next);
    upper->children().add(pair);

    stage.window->open(lower);
    stage.window->open(upper);

    same(stage.top.focused_node(), static_cast<ui::Node*>(top.get()));

    // Z niższej zasłony: czeka.
    beside->focus();

    same(stage.top.focused_node(), static_cast<ui::Node*>(top.get()));

    // W wierzchniej: od razu.
    next->focus();

    same(stage.top.focused_node(), static_cast<ui::Node*>(next.get()));

    // Po zejściu wierzchniej ognisko trafia tam, gdzie prosiła niższa.
    upper->close();

    same(stage.top.focused_node(), static_cast<ui::Node*>(beside.get()));

    lower->close();

    same(stage.top.focused_node(), static_cast<ui::Node*>(stage.body.get()));
}

// --- Warstwy: to, co zasłony i okna mają wspólne ---------------------------------------------------

// Zasłona i okno to **warstwy** jednego rodzaju: zamyka je pojemnik, a bieżąca jest ta na wierzchu.
// Treść pod zasłonami warstwą nie jest i ani nie liczy się do `many()`, ani nie bywa bieżąca.
TRIAL("Layers: bieżąca jest wierzchnia, treść się nie liczy") {
    Stage stage;

    ought(stage.window->empty());
    same(stage.window->many(), std::size_t{0});
    ought(stage.window->current() == nullptr);

    const auto first = std::make_shared<ui::Modal>();
    const auto second = std::make_shared<ui::Modal>();

    stage.window->open(first);
    stage.window->open(second);

    ought(!stage.window->empty());
    same(stage.window->many(), std::size_t{2});
    same(stage.window->current(), static_cast<ui::Modal*>(second.get()));
    ought(second->current());
    ought(!first->current());
}

TRIAL("Layers: close bez argumentu zamyka wierzchnią, a cudzej warstwy nie rusza") {
    Stage stage;

    const auto first = std::make_shared<ui::Modal>();
    const auto second = std::make_shared<ui::Modal>();
    const auto stranger = std::make_shared<ui::Modal>();

    stage.window->open(first);
    stage.window->open(second);

    stage.window->close(*stranger);

    same(stage.window->many(), std::size_t{2});

    stage.window->close();

    same(stage.window->many(), std::size_t{1});
    same(stage.window->current(), static_cast<ui::Modal*>(first.get()));
    ought(second->parent() == nullptr);

    stage.window->close();
    stage.window->close();

    ought(stage.window->empty());
}

// Warstwa poza pojemnikiem nie ma czego zamykać i nie jest bieżąca — nie rzuca, bo zamykanie bywa
// wołane z uchwytów, które nie wiedzą, czy okno jeszcze wisi.
TRIAL("Layer: poza pojemnikiem close nic nie robi, a current jest fałszem") {
    const auto alone = std::make_shared<ui::Modal>();

    int closed = 0;

    (void)alone->on<ui::CloseEvent>([&closed](ui::CloseEvent&) {
        ++closed;

        return true;
    });

    alone->close();

    same(closed, 0);
    ought(!alone->current());
}

TRIAL("Layer: zamknięcie ogłasza się raz, a warstwa schodzi z drzewa") {
    Stage stage;

    const auto one = std::make_shared<ui::Modal>();

    int closed = 0;

    (void)one->on<ui::CloseEvent>([&closed, &one](ui::CloseEvent&) {
        ++closed;

        ought(one->parent() != nullptr);

        return true;
    });

    stage.window->open(one);

    one->close();
    one->close();

    same(closed, 1);
    ought(one->parent() == nullptr);
    ought(stage.window->empty());
}

// Otwarcie, które się nie udało, nie zostawia po sobie śladu: warstwa wisząca już gdzie indziej
// nie da się dopiąć drugi raz, a lista otwarcia nie może jej wtedy liczyć.
TRIAL("Layers: nieudane otwarcie nie zostaje na liście") {
    Stage stage;

    const auto elsewhere = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Modal>();

    elsewhere->open(one);

    threw(stage.window->open(one));

    ought(stage.window->empty());
    ought(stage.window->current() == nullptr);
}
