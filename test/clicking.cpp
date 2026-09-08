#include "proving.hpp"

#include <memory>
#include <string>
#include <vector>

#include "talcite/column.hpp"
#include "talcite/root.hpp"

using namespace talcite;

namespace {

// Korzeń, który zapisuje, co do niego dotarło i w jakiej kolejności.
class Sheet : public ui::Root {
public:
    [[nodiscard]] int takes() const override { return ui::plenty; }

    std::vector<std::string> told;

protected:
    bool react(ui::MouseEvent& what) override {
        told.push_back(what.press() == ui::Press::down ? "wciśnięcie"
                       : what.press() == ui::Press::up ? "puszczenie"
                                                       : "ruch");

        return true;
    }

    bool react(ui::ClickEvent& what) override {
        told.push_back("kliknięcie " + std::to_string(what.where().x) + "," +
                       std::to_string(what.where().y));

        return true;
    }

    bool react(ui::DoubleClickEvent& what) override {
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
    bool react(ui::ClickEvent&) override {
        ++clicks;

        return true;
    }

    bool react(ui::DoubleClickEvent&) override {
        ++doubles;

        return true;
    }

private:
    ui::Size wants_;
};

void pressed(ui::Root& onto, ui::Spot where, ui::Mouse which = ui::Mouse::left) {
    ui::MouseEvent what{where, ui::Press::down, which};

    (void)onto.deal(what);
}

void freed(ui::Root& onto, ui::Spot where, ui::Mouse which = ui::Mouse::left) {
    ui::MouseEvent what{where, ui::Press::up, which};

    (void)onto.deal(what);
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
    ought(top.clicking());
}

TRIAL("Kliknięcie: cisza po kliknięciu niczego nie dokłada, tylko zamyka okno dwukliku") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});

    top.settle();

    same(top.told.size(), std::size_t{3});
    ought(!top.clicking());

    // Następne kliknięcie w tę samą kratkę jest znów pojedynczym — okno się zamknęło.
    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.back(), std::string{"kliknięcie 3,2"});
    ought(top.clicking());
}

TRIAL("Kliknięcie: drugie w oknie daje dwuklik, bez drugiego pojedynczego") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});
    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.back(), std::string{"podwójne 3,2"});
    ought(!top.clicking());

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

    ought(top.clicking());
    same(top.told.back(), std::string{"kliknięcie 1,1"});
}

TRIAL("Kliknięcie: puszczenie w innej kratce to ciągnięcie") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {5, 2});

    ought(!top.clicking());

    top.settle();

    same(top.told.size(), std::size_t{2});
}

TRIAL("Kliknięcie: ciągnięcie nie psuje okna dwukliku po kliknięciu") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});

    pressed(top, {7, 7});
    freed(top, {9, 9});

    ought(top.clicking());

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
    ought(top.clicking());

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
    ought(top.clicking());
}

TRIAL("Kliknięcie: puszczenie innego przycisku niż wciśnięty nic nie znaczy") {
    Sheet top;

    pressed(top, {3, 2}, ui::Mouse::left);
    freed(top, {3, 2}, ui::Mouse::right);

    ought(!top.clicking());
}

TRIAL("Kliknięcie: samo puszczenie, bez wciśnięcia, nic nie zaczyna") {
    Sheet top;

    freed(top, {3, 2});

    ought(!top.clicking());
}

TRIAL("Kliknięcie: ruch myszy niczego nie rozstrzyga") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});

    ui::MouseEvent what{{4, 4}, ui::Press::move, ui::Mouse::none};

    (void)top.deal(what);

    ought(top.clicking());
}

TRIAL("Kliknięcie: rozstrzyganie bez czekającego kliknięcia nic nie robi") {
    Sheet top;

    top.settle();

    same(top.told.size(), std::size_t{0});
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

    top.settle();

    same(first->clicks, 0);
    same(second->clicks, 1);

    pressed(top, {2, 3});
    freed(top, {2, 3});
    pressed(top, {2, 3});
    freed(top, {2, 3});

    same(second->doubles, 1);
}
