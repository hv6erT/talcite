#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/row.hpp"
#include "talcite/span.hpp"

using namespace talcite;

namespace {

class Box : public ui::Node {
public:
    Box(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

    void wanting(ui::Size given) { wants_ = given; }

protected:
    View paint(ui::Size given, ui::Area seen) override {
        View made{seen.size};

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                made.put({x - seen.at.x, y - seen.at.y}, what_, Style{});

        return made;
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

std::shared_ptr<Box> box(ui::Size wants, char32_t what) {
    return std::make_shared<Box>(wants, what);
}

}

TRIAL("Dzieci: zwykły węzeł bierze jedno i przy drugim rzuca") {
    ui::Node one;

    same(one.capacity(), 1);

    one.children().add(box({2, 1}, U'a'));

    threw(one.children().add(box({2, 1}, U'b')));
}

TRIAL("Dzieci: napis nie bierze żadnego") {
    ui::Span one{"kot"};

    same(one.capacity(), 0);

    threw(one.children().add(box({2, 1}, U'a')));
}

TRIAL("Dzieci: korzeń jest opakowaniem, nie układem") {
    ui::Root top;

    same(top.capacity(), 1);

    top.children().add(box({2, 1}, U'a'));

    threw(top.children().add(box({2, 1}, U'b')));
}

TRIAL("Dzieci: wiersz i kolumna biorą, ile trzeba") {
    ui::Row row;
    ui::Column column;

    same(row.capacity(), ui::plenty);
    same(column.capacity(), ui::plenty);

    for (int which = 0; which < 5; ++which) {
        row.children().add(box({1, 1}, U'a'));
        column.children().add(box({1, 1}, U'a'));
    }

    same(row.children().many(), std::size_t{5});
    same(column.children().many(), std::size_t{5});
}

TRIAL("Węzeł: jedyne dziecko dostaje cały prostokąt") {
    ui::Node one;

    const auto inner = box({2, 1}, U'a');

    one.children().add(inner);

    (void)one.render({{0, 0}, {6, 3}});

    same(inner->area().size.wide, 6);
    same(inner->area().size.high, 3);
}

TRIAL("Węzeł: chce tyle, co jego jedyne dziecko") {
    ui::Node one;

    one.children().add(box({4, 2}, U'a'));

    const ui::Size wants = one.wants({80, 25});

    same(wants.wide, 4);
    same(wants.high, 2);
}

TRIAL("Węzeł bez dzieci nie chce nic") {
    const ui::Node one;

    same(one.wants({80, 25}).wide, 0);
    same(one.wants({80, 25}).high, 0);
}

TRIAL("Kolumna: dzieci stoją jedno pod drugim") {
    ui::Column one;

    const auto first = box({6, 1}, U'a');
    const auto second = box({6, 2}, U'b');

    one.children().add(first);
    one.children().add(second);

    const View made = one.render({{0, 0}, {6, 4}});

    same(first->area().at.y, 0);
    same(second->area().at.y, 1);
    same(read(made, 0), std::string{"aaaaaa"});
    same(read(made, 1), std::string{"bbbbbb"});
    same(read(made, 2), std::string{"bbbbbb"});
    same(read(made, 3), std::string{""});
}

TRIAL("Wiersz: dzieci stoją obok siebie") {
    ui::Row one;

    const auto first = box({2, 1}, U'a');
    const auto second = box({3, 1}, U'b');

    one.children().add(first);
    one.children().add(second);

    const View made = one.render({{0, 0}, {6, 1}});

    same(first->area().at.x, 0);
    same(second->area().at.x, 2);
    same(read(made, 0), std::string{"aabbb"});
}

// Wiersz, który się łamie: dziecko, które nie mieści się w reszcie linii, zaczyna następną, a każda
// linia stoi pośrodku. Dziecko o zerowej szerokości nie otwiera odstępu ani linii; szersze niż cały
// wiersz stoi samo, przycięte.
TRIAL("Wiersz: łamany przenosi to, co się nie mieści, i stawia linie pośrodku") {
    ui::Row one{ui::RowShape{1, true}};

    const auto first = box({5, 1}, U'a');
    const auto empty = box({0, 1}, U'x');
    const auto second = box({5, 1}, U'b');
    const auto third = box({5, 1}, U'c');
    const auto wide = box({20, 1}, U'd');

    one.children().add(first);
    one.children().add(empty);
    one.children().add(second);
    one.children().add(third);
    one.children().add(wide);

    ought(one.wrapping());
    same(one.wants({12, 10}), (ui::Size{12, 3}));
    same(one.wants({12, 2}), (ui::Size{12, 2}));

    const View made = one.render({{0, 0}, {12, 3}});

    same(read(made, 0), std::string{"aaaaa bbbbb"});
    same(read(made, 1), std::string{"   ccccc"});
    same(read(made, 2), std::string{"dddddddddddd"});
    same(third->area().at.y, 1);
    same(empty->area().size.wide, 0);
    same(wide->area().size.wide, 12);

    // Odstęp liczy się przy łamaniu: dwie piątki w dziesięciu kolumnach to dwie linie, nie jedna.
    same(one.wants({10, 10}), (ui::Size{10, 4}));

    // Dziecko, które przestało cokolwiek zajmować, dostaje prostokąt zerowy, a nie stary.
    first->wanting({0, 1});

    (void)one.render({{0, 0}, {12, 3}});

    same(first->area().size.wide, 0);

    first->wanting({5, 1});

    // Dość miejsca: jedna linia, jak bez łamania.
    same(one.wants({60, 10}), (ui::Size{38, 1}));

    one.wrapping(false);

    same(one.wants({12, 10}).high, 1);
}

TRIAL("Wiersz: dziecko dostaje pełną wysokość") {
    ui::Row one;

    const auto first = box({2, 1}, U'a');

    one.children().add(first);

    (void)one.render({{0, 0}, {6, 3}});

    same(first->area().size.high, 3);
}

TRIAL("Kolumna: dziecko dostaje pełną szerokość") {
    ui::Column one;

    const auto first = box({2, 1}, U'a');

    one.children().add(first);

    (void)one.render({{0, 0}, {6, 3}});

    same(first->area().size.wide, 6);
}

TRIAL("Kolumna: chce sumy wysokości, ale nie więcej niż dostała") {
    ui::Column one;

    one.children().add(box({4, 3}, U'a'));
    one.children().add(box({4, 3}, U'b'));

    same(one.wants({10, 10}).high, 6);
    same(one.wants({10, 4}).high, 4);
    same(one.wants({2, 10}).wide, 2);
}

TRIAL("Wiersz: chce sumy szerokości, ale nie więcej niż dostał") {
    ui::Row one;

    one.children().add(box({4, 1}, U'a'));
    one.children().add(box({4, 1}, U'b'));

    same(one.wants({10, 10}).wide, 8);
    same(one.wants({5, 10}).wide, 5);
    same(one.wants({10, 1}).high, 1);
}

TRIAL("Wiersz: dziecko poza prawą krawędzią dostaje zerową szerokość") {
    ui::Row one;

    const auto first = box({4, 1}, U'a');
    const auto second = box({4, 1}, U'b');

    one.children().add(first);
    one.children().add(second);

    (void)one.render({{0, 0}, {4, 1}});

    same(first->area().size.wide, 4);
    same(second->area().size.wide, 0);
    same(second->area().at.x, 4);
}

TRIAL("Wiersz w kolumnie układa się tak, jak się spodziewamy") {
    ui::Column outer;

    const auto row = std::make_shared<ui::Row>();

    row->children().add(std::make_shared<ui::Span>("ala"));
    row->children().add(std::make_shared<ui::Span>("kot"));

    outer.children().add(row);
    outer.children().add(std::make_shared<ui::Span>("pies"));

    const View made = outer.render({{0, 0}, {8, 2}});

    same(read(made, 0), std::string{"alakot"});
    same(read(made, 1), std::string{"pies"});
}
