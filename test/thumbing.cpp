#include "proving.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/column.hpp"
#include "talcite/encoding.hpp"
#include "talcite/list.hpp"
#include "talcite/listitem.hpp"
#include "talcite/root.hpp"
#include "talcite/scroll.hpp"
#include "talcite/table.hpp"
#include "talcite/viewer.hpp"

using namespace talcite;

// **Mysz na pasku przewijania** (U6 w twa): strzałka przesuwa o wiersz, tor przed i za suwakiem
// o stronę, suwak daje się ciągnąć — w tabeli, na stronie, w liście i w przewijaku, jedną
// mechaniką. Klik na pasku nie stawia wiersza tabeli i nie jest dwuklikiem na wierszu.
namespace {

// Korzeń, który liczy dwukliki dochodzące widżetową drogą — tą samą, którą panel twa robi z nich
// Enter.
class Sheet : public ui::Root {
public:
    int doubled = 0;

protected:
    bool handle(ui::DoubleClickEvent&) override {
        ++doubled;

        return true;
    }
};

std::string said_at(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter == 0) continue;

        utf8::encode(letter, made);
    }

    return made;
}

void pressed(ui::Root& top, ui::Spot at, ui::Press how, ui::Mouse button = ui::Mouse::left) {
    ui::MouseEvent what{at, how, button};

    (void)top.dispatch(what);
}

void clicked(ui::Root& top, ui::Spot at) {
    pressed(top, at, ui::Press::down);
    pressed(top, at, ui::Press::up);
}

ui::TableSource numbered(std::size_t much) {
    return [much](std::size_t which, std::size_t most) {
        std::vector<ui::TableRow> made;

        for (std::size_t one = which; one < much && made.size() < most; ++one)
            made.push_back(ui::TableRow{ui::TableCell{"plik" + std::to_string(one), {}}});

        return made;
    };
}

// Tabela stu wierszy w oknie 20x11: nagłówek i dziesięć wierszy, pasek w kolumnie 19 od wiersza 0
// do 10 — strzałka w górę w wierszu 0, w dół w wierszu 10.
std::shared_ptr<ui::Table> table(Sheet& top) {
    const auto one = std::make_shared<ui::Table>(ui::TableShape{.heading = true, .scrollbar = true});

    one->columns({ui::TableColumn{"Nazwa", 0, ui::Justify::left}, ui::TableColumn{"Wiek", 4, ui::Justify::right}});
    one->source(numbered(100), 100);

    top.children().add(one);

    (void)top.render({{0, 0}, {20, 11}});

    return one;
}

// Wiersz paska, w którym stoi suwak — szukamy po znaku, który rysuje go skórka domyślna.
int thumb_row(Sheet& top, int x, int from, int to) {
    const View shown = top.render({{0, 0}, {20, 11}});
    const char32_t track = shown.at({x, to - 1}).letter;

    for (int y = from; y < to; ++y)
        if (shown.at({x, y}).letter != track) return y;

    return -1;
}

std::string lines(int much) {
    std::string made;

    for (int one = 0; one < much; ++one) made += "wiersz " + std::to_string(one) + "\n";

    return made;
}

}

TRIAL("Pasek: strzałki tabeli przesuwają widok o wiersz, wiersza bieżącego nie ruszają") {
    Sheet top;

    const auto one = table(top);

    one->which(0);

    clicked(top, {19, 10});

    same(one->first(), std::size_t{1});
    same(*one->which(), std::size_t{0});

    clicked(top, {19, 10});
    clicked(top, {19, 0});

    same(one->first(), std::size_t{1});

    clicked(top, {19, 0});
    clicked(top, {19, 0});

    same(one->first(), std::size_t{0});
}

TRIAL("Pasek: tor tabeli przewija o stronę, na końcu staje") {
    Sheet top;

    const auto one = table(top);

    // Suwak stoi na górze toru — tor pod nim to strona w dół.
    clicked(top, {19, 9});

    same(one->first(), std::size_t{10});

    for (int round = 0; round < 20; ++round) {
        (void)top.render({{0, 0}, {20, 11}});
        clicked(top, {19, 9});
    }

    same(one->first(), std::size_t{90});

    (void)top.render({{0, 0}, {20, 11}});
    clicked(top, {19, 1});

    same(one->first(), std::size_t{80});

    // Strona za końcem staje na końcu, nie za nim.
    one->first(85);

    (void)top.render({{0, 0}, {20, 11}});
    clicked(top, {19, 1});

    same(one->first(), std::size_t{75});

    // W wyższym oknie (dwadzieścia wierszy, tor dziewiętnaście kratek) suwak przy 75 stoi w kratkach
    // 15–17 toru, więc tor pod nim jest — strona w dół z 75 to 80, nie 95.
    one->first(75);

    (void)top.render({{0, 0}, {20, 21}});
    clicked(top, {19, 19});

    same(one->first(), std::size_t{80});
}

TRIAL("Pasek: suwak tabeli daje się ciągnąć, także poza pasek") {
    Sheet top;

    const auto one = table(top);

    const int y = thumb_row(top, 19, 1, 10);

    ought(y == 1);

    pressed(top, {19, y}, ui::Press::down);
    pressed(top, {19, 9}, ui::Press::move);

    same(one->first(), std::size_t{90});

    // Kratka toru to dziewięćdziesiąt ósmych wiersza — zaokrąglone, nie ucięte: trzecia daje 34.
    pressed(top, {19, 4}, ui::Press::move);

    same(one->first(), std::size_t{34});

    // Ruch w bok, poza pasek — przytrzymanie dalej prowadzi go do tabeli.
    pressed(top, {3, 5}, ui::Press::move);

    ought(one->first() > 0 && one->first() < 90);

    pressed(top, {40, 0}, ui::Press::move);

    same(one->first(), std::size_t{0});

    pressed(top, {40, 0}, ui::Press::up);

    // Po puszczeniu ruch niczego nie ciągnie.
    pressed(top, {19, 9}, ui::Press::move);

    same(one->first(), std::size_t{0});
}

// Suwak długi na kilka kratek trzyma się kursora tą kratką, za którą go chwycono — ruch o kratkę
// przesuwa widok o kratkę toru, a nie skacze początkiem suwaka pod kursor.
TRIAL("Pasek: suwak chwycony w środku idzie za kursorem tą samą kratką") {
    Sheet top;

    const auto one = std::make_shared<ui::Table>(ui::TableShape{.heading = true, .scrollbar = true});

    one->columns({ui::TableColumn{"Nazwa", 0, ui::Justify::left}});
    one->source(numbered(20), 20);

    top.children().add(one);

    (void)top.render({{0, 0}, {20, 11}});

    // Tor 9 kratek, okno 10 z 20 — suwak cztery kratki, od wiersza 1 do 4. Chwyt w wierszu 3.
    pressed(top, {19, 3}, ui::Press::down);
    pressed(top, {19, 3}, ui::Press::move);

    same(one->first(), std::size_t{0});

    pressed(top, {19, 4}, ui::Press::move);

    same(one->first(), std::size_t{2});

    pressed(top, {19, 4}, ui::Press::up);
}

TRIAL("Pasek: klik na pasku tabeli nie stawia wiersza i nie jest dwuklikiem na wierszu") {
    Sheet top;

    const auto one = table(top);

    one->which(3);

    clicked(top, {19, 5});
    clicked(top, {19, 5});

    same(*one->which(), std::size_t{3});
    same(top.doubled, 0);

    // Klik obok paska stawia wiersz jak zawsze.
    (void)top.render({{0, 0}, {20, 11}});
    clicked(top, {2, 2});

    same(*one->which(), one->first() + 1);
}

TRIAL("Pasek: prawy przycisk i tabela bez paska nic nie przewijają") {
    Sheet top;

    const auto one = table(top);

    pressed(top, {19, 10}, ui::Press::down, ui::Mouse::right);

    same(one->first(), std::size_t{0});

    one->scrollbar(false);

    (void)top.render({{0, 0}, {20, 11}});

    clicked(top, {19, 10});

    same(one->first(), std::size_t{0});
}

TRIAL("Tabela: column_under mówi, nad którą kolumną nagłówka stoi kratka") {
    Sheet top;

    const auto one = table(top);

    // Szerokość 19 bez paska: Wiek 4, odstęp 1, Nazwa 14.
    same(one->column_under({0, 0}).value_or(9), std::size_t{0});
    same(one->column_under({13, 0}).value_or(9), std::size_t{0});
    same(one->column_under({14, 0}).value_or(9), std::size_t{0});
    same(one->column_under({15, 0}).value_or(9), std::size_t{1});
    same(one->column_under({18, 0}).value_or(9), std::size_t{1});
    ought(!one->column_under({19, 0}).has_value());
    ought(!one->column_under({2, 1}).has_value());
}

TRIAL("Pasek: strona tekstu — strzałka, tor i suwak") {
    Sheet top;

    const auto one = std::make_shared<ui::Viewer>(ui::ScrollShape{.scrollbar = true});

    one->hold(bytes(lines(100)));

    top.children().add(one);

    (void)top.render({{0, 0}, {20, 10}});

    clicked(top, {19, 9});

    same(one->first().value_or(99), std::size_t{1});

    (void)top.render({{0, 0}, {20, 10}});
    clicked(top, {19, 8});

    same(one->first().value_or(99), std::size_t{11});

    (void)top.render({{0, 0}, {20, 10}});
    clicked(top, {19, 0});

    same(one->first().value_or(99), std::size_t{10});

    (void)top.render({{0, 0}, {20, 10}});

    // Suwak ciągnięty na sam dół toru — koniec tekstu.
    const View shown = top.render({{0, 0}, {20, 10}});
    const char32_t track = shown.at({19, 8}).letter;

    int y = -1;

    for (int row = 1; row < 9 && y < 0; ++row)
        if (shown.at({19, row}).letter != track) y = row;

    ought(y > 0);

    pressed(top, {19, y}, ui::Press::down);
    pressed(top, {19, 30}, ui::Press::move);
    pressed(top, {19, 30}, ui::Press::up);

    same(one->first().value_or(0), std::size_t{90});
}

TRIAL("Pasek: lista — strzałka, tor i suwak ruszają widok, nie wybór") {
    Sheet top;

    const auto one = std::make_shared<ui::List>(ui::plenty, ui::plenty, ui::ScrollShape{.scrollbar = true});

    for (int at = 0; at < 50; ++at) one->children().add(std::make_shared<ui::ListItem>("pozycja " + std::to_string(at)));

    top.children().add(one);

    const View first = top.render({{0, 0}, {20, 10}});

    ought(said_at(first, 0).starts_with("pozycja 0"));

    clicked(top, {19, 9});

    ought(said_at(top.render({{0, 0}, {20, 10}}), 0).starts_with("pozycja 1"));

    clicked(top, {19, 8});

    ought(said_at(top.render({{0, 0}, {20, 10}}), 0).starts_with("pozycja 11"));

    pressed(top, {19, 3}, ui::Press::down);
    pressed(top, {19, 40}, ui::Press::move);
    pressed(top, {19, 40}, ui::Press::up);

    ought(said_at(top.render({{0, 0}, {20, 10}}), 0).starts_with("pozycja 40"));

    // Tor nad suwakiem — strona w górę; strzałka w dół na końcu niczego nie rusza i nie zawiadamia.
    clicked(top, {19, 1});

    ought(said_at(top.render({{0, 0}, {20, 10}}), 0).starts_with("pozycja 30"));

    // Suwak przy 30 stoi w szóstej kratce toru; tor pod nim to strona w dół, do końca.
    clicked(top, {19, 8});

    ought(said_at(top.render({{0, 0}, {20, 10}}), 0).starts_with("pozycja 40"));

    int moved = 0;

    (void)one->on<ui::ScrollEvent>([&moved](ui::ScrollEvent&) {
        ++moved;

        return false;
    });

    clicked(top, {19, 9});

    same(moved, 0);
    ought(said_at(top.render({{0, 0}, {20, 10}}), 0).starts_with("pozycja 40"));
}

TRIAL("Pasek: przewijak w obu osiach — każdy pasek swoją osią") {
    Sheet top;

    const auto one = std::make_shared<ui::Scroll>(ui::Axis::both, ui::ScrollShape{.scrollbar = true});
    const auto column = std::make_shared<ui::Column>();

    for (int at = 0; at < 40; ++at)
        column->children().add(std::make_shared<ui::ListItem>("bardzo długi wiersz numer " + std::to_string(at)));

    one->children().add(column);
    top.children().add(one);

    (void)top.render({{0, 0}, {20, 10}});

    // Pionowy: kolumna 19, wiersze 0–8; poziomy: wiersz 9, kolumny 0–18.
    clicked(top, {19, 8});

    same(one->shift().y, 1);
    same(one->shift().x, 0);

    (void)top.render({{0, 0}, {20, 10}});
    clicked(top, {18, 9});

    same(one->shift().x, 1);
    same(one->shift().y, 1);

    (void)top.render({{0, 0}, {20, 10}});
    clicked(top, {0, 9});

    // Wiersz przewinięty pod poziomy pasek nie łapie kliku — dostaje go pasek, a widok w pionie stoi.
    same(one->shift().x, 0);
    same(one->shift().y, 1);

    (void)top.render({{0, 0}, {20, 10}});
    clicked(top, {19, 7});

    same(one->shift().y, 10);

    // Suwak pionowy ciągnięty przez poziomy pasek dalej jest pionowym: ruch idzie do chwytu, który
    // trzyma, a nie do paska pod kursorem.
    one->shift({0, 0});

    (void)top.render({{0, 0}, {20, 10}});

    pressed(top, {19, 1}, ui::Press::down);
    pressed(top, {5, 9}, ui::Press::move);

    same(one->shift().y, 31);
    same(one->shift().x, 0);

    pressed(top, {5, 9}, ui::Press::up);
}
