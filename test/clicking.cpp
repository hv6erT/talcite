#include "proving.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "talcite/column.hpp"
#include "talcite/root.hpp"
#include "talcite/row.hpp"

using namespace talcite;

namespace {

// Korzeń, który zapisuje, co do niego dotarło i w jakiej kolejności.
class Sheet : public ui::Root {
public:
    [[nodiscard]] int capacity() const override { return ui::plenty; }

    std::vector<std::string> told;

protected:
    bool handle(ui::MouseEvent& what) override {
        told.push_back(what.press() == ui::Press::down ? "wciśnięcie"
                       : what.press() == ui::Press::up ? "puszczenie"
                                                       : "ruch");

        return true;
    }

    bool handle(ui::ClickEvent& what) override {
        told.push_back("kliknięcie " + std::to_string(what.where().x) + "," +
                       std::to_string(what.where().y));

        return true;
    }

    bool handle(ui::DoubleClickEvent& what) override {
        told.push_back("podwójne " + std::to_string(what.where().x) + "," +
                       std::to_string(what.where().y));

        return true;
    }
};

class Box : public ui::Node {
public:
    explicit Box(ui::Size wants) : wants_(wants) {}

    [[nodiscard]] bool focusable() const override { return true; }

    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

    int clicks = 0;
    int doubles = 0;

protected:
    bool handle(ui::ClickEvent&) override {
        ++clicks;

        return true;
    }

    bool handle(ui::DoubleClickEvent&) override {
        ++doubles;

        return true;
    }

private:
    ui::Size wants_;
};

// **Zegar próby.** Zdarzenia niosą chwile podane wprost, więc o tym, czy drugie kliknięcie
// zmieściło się w oknie, rozstrzyga próba, a nie obciążenie maszyny. Czas stoi, dopóki próba
// sama go nie przesunie przez `later`.
std::chrono::steady_clock::time_point moment{};

void later(std::chrono::milliseconds much) { moment += much; }

void pressed(ui::Root& onto, ui::Spot where, ui::Mouse which = ui::Mouse::left) {
    ui::MouseEvent what{where, ui::Press::down, which, {}, moment};

    (void)onto.dispatch(what);
}

void freed(ui::Root& onto, ui::Spot where, ui::Mouse which = ui::Mouse::left) {
    ui::MouseEvent what{where, ui::Press::up, which, {}, moment};

    (void)onto.dispatch(what);
}

}

TRIAL("Kliknięcie: wciśnięcie i puszczenie w tej samej kratce to kliknięcie, od razu") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.size(), std::size_t{3});
    same(top.told[0], std::string{"wciśnięcie"});
    same(top.told[1], std::string{"puszczenie"});
    same(top.told[2], std::string{"kliknięcie 3,2"});
}

TRIAL("Kliknięcie: po oknie dwukliku następne w tej samej kratce jest znów pojedyncze") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});

    later(top.double_click() + std::chrono::milliseconds{1});

    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.size(), std::size_t{6});
    same(top.told.back(), std::string{"kliknięcie 3,2"});

    // I to ono otwiera nowe okno: następne tuż po nim jest dwuklikiem.
    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.back(), std::string{"podwójne 3,2"});
}

// Granica okna należy do okna: drugie dokładnie po `double_click()` jeszcze jest dwuklikiem.
TRIAL("Kliknięcie: drugie równo na granicy okna jest dwuklikiem") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});

    later(top.double_click());

    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.back(), std::string{"podwójne 3,2"});
}

TRIAL("Kliknięcie: okno dwukliku ustawia się na korzeniu, domyślnie 400 ms") {
    Sheet top;

    same(top.double_click(), std::chrono::milliseconds{400});

    top.double_click(std::chrono::milliseconds{100});

    pressed(top, {3, 2});
    freed(top, {3, 2});

    later(std::chrono::milliseconds{150});

    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.back(), std::string{"kliknięcie 3,2"});

    top.double_click(std::chrono::milliseconds{1000});

    later(std::chrono::milliseconds{150});

    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.back(), std::string{"podwójne 3,2"});
}

// Chwila, której nie podano, to chwila powstania zdarzenia — tak pracuje terminal.
TRIAL("Kliknięcie: zdarzenie myszy bez podanej chwili niesie teraz") {
    const auto before = std::chrono::steady_clock::now();

    const ui::MouseEvent what{{0, 0}, ui::Press::down, ui::Mouse::left};

    const auto after = std::chrono::steady_clock::now();

    ought(what.when() >= before);
    ought(what.when() <= after);
}

TRIAL("Kliknięcie: drugie w oknie daje dwuklik, bez drugiego pojedynczego") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});
    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.back(), std::string{"podwójne 3,2"});

    // Jedno kliknięcie — to pierwsze, jak w Windows; drugie puszczenie jest już dwuklikiem.
    int clicks = 0;

    for (const std::string& one : top.told) clicks += one.rfind("kliknięcie", 0) == 0 ? 1 : 0;

    same(clicks, 1);
}

TRIAL("Kliknięcie: po podwójnym zaczynamy od nowa") {
    Sheet top;

    pressed(top, {1, 1});
    freed(top, {1, 1});
    pressed(top, {1, 1});
    freed(top, {1, 1});

    pressed(top, {1, 1});
    freed(top, {1, 1});

    same(top.told.back(), std::string{"kliknięcie 1,1"});

    // Trzecie otworzyło nowe okno, więc czwarte jest znów dwuklikiem.
    pressed(top, {1, 1});
    freed(top, {1, 1});

    same(top.told.back(), std::string{"podwójne 1,1"});
}

TRIAL("Kliknięcie: puszczenie w innej kratce to ciągnięcie") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {5, 2});

    same(top.told.size(), std::size_t{2});

    // Ciągnięcie nie otworzyło okna: kliknięcie w kratkę puszczenia jest pojedyncze.
    pressed(top, {5, 2});
    freed(top, {5, 2});

    same(top.told.back(), std::string{"kliknięcie 5,2"});
}

TRIAL("Kliknięcie: ciągnięcie nie psuje okna dwukliku po kliknięciu") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});

    pressed(top, {7, 7});
    freed(top, {9, 9});

    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.back(), std::string{"podwójne 3,2"});
}

TRIAL("Kliknięcie: drugie w innej kratce jest osobnym pojedynczym") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});

    pressed(top, {8, 4});
    freed(top, {8, 4});

    same(top.told.back(), std::string{"kliknięcie 8,4"});

    // Okno przeniosło się na drugą kratkę: trzecie w niej jest dwuklikiem.
    pressed(top, {8, 4});
    freed(top, {8, 4});

    same(top.told.back(), std::string{"podwójne 8,4"});
}

TRIAL("Kliknięcie: drugie innym przyciskiem nie jest podwójnym") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});

    pressed(top, {3, 2}, ui::Mouse::right);
    freed(top, {3, 2}, ui::Mouse::right);

    same(top.told.back(), std::string{"kliknięcie 3,2"});

    // Okno przeszło na prawy: drugie prawym jest dwuklikiem.
    pressed(top, {3, 2}, ui::Mouse::right);
    freed(top, {3, 2}, ui::Mouse::right);

    same(top.told.back(), std::string{"podwójne 3,2"});
}

TRIAL("Kliknięcie: puszczenie innego przycisku niż wciśnięty nic nie znaczy") {
    Sheet top;

    pressed(top, {3, 2}, ui::Mouse::left);
    freed(top, {3, 2}, ui::Mouse::right);

    same(top.told.size(), std::size_t{2});
}

TRIAL("Kliknięcie: samo puszczenie, bez wciśnięcia, nic nie zaczyna") {
    Sheet top;

    freed(top, {3, 2});

    same(top.told.size(), std::size_t{1});

    // Nic nie czeka: pełne kliknięcie w tę kratkę jest pojedyncze.
    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.back(), std::string{"kliknięcie 3,2"});
}

TRIAL("Kliknięcie: ruch myszy niczego nie rozstrzyga") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});

    ui::MouseEvent what{{4, 4}, ui::Press::move, ui::Mouse::none, {}, moment};

    (void)top.dispatch(what);

    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.back(), std::string{"podwójne 3,2"});
}

TRIAL("Kliknięcie: surowe zdarzenie idzie przed pochodnym") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});
    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.size(), std::size_t{6});
    same(top.told[2], std::string{"kliknięcie 3,2"});
    same(top.told[4], std::string{"puszczenie"});
    same(top.told[5], std::string{"podwójne 3,2"});
}

TRIAL("Kliknięcie: celuje w węzeł pod kursorem, nie w korzeń") {
    Sheet top;

    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Box>(ui::Size{6, 2});
    const auto second = std::make_shared<Box>(ui::Size{6, 2});

    column->children().add(first);
    column->children().add(second);
    top.children().add(column);

    (void)top.render({{0, 0}, {6, 4}});

    pressed(top, {2, 3});
    freed(top, {2, 3});

    later(top.double_click() + std::chrono::milliseconds{1});

    same(first->clicks, 0);
    same(second->clicks, 1);

    pressed(top, {2, 3});
    freed(top, {2, 3});
    pressed(top, {2, 3});
    freed(top, {2, 3});

    same(second->doubles, 1);
}

// --- Przytrzymanie myszy -------------------------------------------------------------------------

namespace {

// Pole, które liczy surowe zdarzenia myszy i — gdy każemy — prosi przy wciśnięciu o przytrzymanie.
class Grabber : public ui::Node {
public:
    explicit Grabber(bool grabs) : grabs_(grabs) {}

    [[nodiscard]] ui::Size wants(ui::Size) const override { return {5, 1}; }

    int downs = 0;
    int moves = 0;
    int ups = 0;

protected:
    bool handle(ui::MouseEvent& what) override {
        if (what.press() == ui::Press::down) {
            ++downs;

            if (grabs_ && root() != nullptr) root()->capture(*this);
        } else if (what.press() == ui::Press::move) {
            ++moves;
        } else {
            ++ups;
        }

        return true;
    }

private:
    bool grabs_ = false;
};

struct Pair {
    ui::Root top;
    std::shared_ptr<ui::Row> row = std::make_shared<ui::Row>();
    std::shared_ptr<Grabber> left;
    std::shared_ptr<Grabber> right = std::make_shared<Grabber>(false);

    explicit Pair(bool grabs) : left(std::make_shared<Grabber>(grabs)) {
        row->children().add(left);
        row->children().add(right);
        top.children().add(row);

        (void)top.render({{0, 0}, {10, 1}});
    }

    void sent(ui::Spot where, ui::Press press, ui::Mouse which = ui::Mouse::left) {
        ui::MouseEvent what{where, press, which};

        (void)top.dispatch(what);
    }
};

}

// Węzeł, który poprosił o przytrzymanie, dostaje ruch i puszczenie **gdziekolwiek jest kursor**,
// aż do puszczenia. Po nim ruch znów idzie pod kursor.
TRIAL("Root: przytrzymany dostaje ruch i puszczenie poza sobą") {
    Pair pair{true};

    pair.sent({1, 0}, ui::Press::down);
    pair.sent({7, 0}, ui::Press::move);
    pair.sent({8, 0}, ui::Press::up);

    same(pair.left->downs, 1);
    same(pair.left->moves, 1);
    same(pair.left->ups, 1);
    same(pair.right->moves, 0);
    same(pair.right->ups, 0);

    pair.sent({7, 0}, ui::Press::move, ui::Mouse::none);

    same(pair.left->moves, 1);
    same(pair.right->moves, 1);
}

// Przytrzymanie jest na życzenie: kto nie poprosił, nie dostaje ruchu poza sobą — tak działa
// najechanie myszą na pozycje menu.
TRIAL("Root: bez prośby ruch idzie pod kursor") {
    Pair pair{false};

    pair.sent({1, 0}, ui::Press::down);
    pair.sent({7, 0}, ui::Press::move);
    pair.sent({8, 0}, ui::Press::up);

    same(pair.left->downs, 1);
    same(pair.left->moves, 0);
    same(pair.left->ups, 0);
    same(pair.right->moves, 1);
    same(pair.right->ups, 1);
}

// Zgubione puszczenie nie przywiązuje myszy na zawsze: nowe wciśnięcie idzie pod kursor
// i zdejmuje stare przytrzymanie.
TRIAL("Root: wciśnięcie zdejmuje stare przytrzymanie") {
    Pair pair{true};

    pair.sent({1, 0}, ui::Press::down);
    pair.sent({7, 0}, ui::Press::down);
    pair.sent({8, 0}, ui::Press::move);

    same(pair.left->downs, 1);
    same(pair.right->downs, 1);
    same(pair.left->moves, 0);
    same(pair.right->moves, 1);
}

// Przytrzymany, który zszedł z drzewa w trakcie ciągnięcia, nie dostaje już nic.
TRIAL("Root: przytrzymany, który zszedł z drzewa, nie dostaje nic") {
    Pair pair{true};

    pair.sent({1, 0}, ui::Press::down);

    (void)pair.row->children().remove(pair.left.get());
    (void)pair.top.render({{0, 0}, {10, 1}});

    // Prawe pole przesunęło się na miejsce lewego.
    pair.sent({1, 0}, ui::Press::move);

    same(pair.left->moves, 0);
    same(pair.right->moves, 1);
}
