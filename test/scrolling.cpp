#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/encoding.hpp"
#include "talcite/hscroll.hpp"
#include "talcite/root.hpp"
#include "talcite/scroll.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"
#include "talcite/vscroll.hpp"

using namespace talcite;

namespace {

// Węzeł, który zapisuje, ile razy i z jaką widocznością go rysowano. Na nim widać, czy oszczędność
// naprawdę działa: niewidoczne dziecko nie powinno w ogóle dostać paint.
class Sheet : public ui::Node {
public:
    explicit Sheet(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    int drawn = 0;
    ui::Area last;

    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

protected:
    View paint(ui::Size given, ui::Area seen) override {
        ++drawn;
        last = seen;

        View made{seen.size};

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                made.letter({x - seen.at.x, y - seen.at.y},
                            static_cast<char32_t>(what_ + static_cast<char32_t>(y)), Style{});

        return made;
    }

private:
    ui::Size wants_;
    char32_t what_ = U'a';
};

std::string read(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter != 0) talcite::spelled(letter, made);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

std::shared_ptr<ui::Column> rows(int much) {
    auto made = std::make_shared<ui::Column>();

    for (int which = 0; which < much; ++which)
        made->children().add(std::make_shared<ui::Span>("wiersz " + std::to_string(which)));

    return made;
}

}

TRIAL("Widoczność: cały prostokąt, gdy nikt nie mówi inaczej") {
    const auto one = std::make_shared<Sheet>(ui::Size{4, 4}, U'a');

    (void)one->render({{0, 0}, {4, 4}});

    same(one->last.at.x, 0);
    same(one->last.at.y, 0);
    same(one->last.size.wide, 4);
    same(one->last.size.high, 4);
}

TRIAL("Widoczność: czego nie widać, tego się nie rysuje") {
    ui::Column top;

    const auto first = std::make_shared<Sheet>(ui::Size{4, 2}, U'a');
    const auto second = std::make_shared<Sheet>(ui::Size{4, 2}, U'a');

    top.children().add(first);
    top.children().add(second);

    (void)top.render({{0, 0}, {4, 2}});

    same(first->drawn, 1);
    same(second->drawn, 0);
}

TRIAL("Widoczność: część wspólna liczy się w obu kierunkach") {
    const ui::Area one = ui::Area::crossed(ui::Area{{0, 0}, {10, 10}}, ui::Area{{5, 5}, {10, 10}});

    same(one.at.x, 5);
    same(one.at.y, 5);
    same(one.size.wide, 5);
    same(one.size.high, 5);

    const ui::Area apart = ui::Area::crossed(ui::Area{{0, 0}, {4, 4}}, ui::Area{{9, 9}, {2, 2}});

    same(apart.size.wide, 0);
    same(apart.size.high, 0);

    const ui::Area over = ui::Area::crossed(ui::Area{{0, 0}, {4, 4}}, ui::Area{{-2, -2}, {3, 3}});

    same(over.at.x, 0);
    same(over.at.y, 0);
    same(over.size.wide, 1);
    same(over.size.high, 1);
}

TRIAL("VScroll: bez przewinięcia widać początek treści") {
    ui::VScroll one;

    one.children().add(rows(10));

    const View made = one.render({{0, 0}, {10, 3}});

    same(read(made, 0), std::string{"wiersz 0"});
    same(read(made, 2), std::string{"wiersz 2"});
    same(one.shift(), 0);
}

TRIAL("VScroll: przewinięcie przesuwa treść w górę") {
    ui::VScroll one;

    one.children().add(rows(10));

    one.shift(4);

    const View made = one.render({{0, 0}, {10, 3}});

    same(read(made, 0), std::string{"wiersz 4"});
    same(read(made, 1), std::string{"wiersz 5"});
}

TRIAL("VScroll: dalej niż koniec treści nie da się przewinąć") {
    ui::VScroll one;

    one.children().add(rows(5));

    one.shift(100);

    const View made = one.render({{0, 0}, {10, 3}});

    same(one.shift(), 2);
    same(read(made, 0), std::string{"wiersz 2"});
}

TRIAL("VScroll: treść krótsza niż okno nie przewija się wcale") {
    ui::VScroll one;

    one.children().add(rows(2));

    one.shift(5);

    (void)one.render({{0, 0}, {10, 6}});

    same(one.shift(), 0);
}

TRIAL("VScroll: ujemne przewinięcie schodzi do zera") {
    ui::VScroll one;

    one.shift(-5);

    same(one.shift(), 0);
}

TRIAL("VScroll: rysuje tylko wiersze, które widać") {
    ui::VScroll one;

    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Sheet>(ui::Size{6, 2}, U'a');
    const auto second = std::make_shared<Sheet>(ui::Size{6, 2}, U'b');
    const auto third = std::make_shared<Sheet>(ui::Size{6, 2}, U'c');

    column->children().add(first);
    column->children().add(second);
    column->children().add(third);

    one.children().add(column);

    one.shift(2);

    (void)one.render({{0, 0}, {6, 2}});

    same(first->drawn, 0);
    same(second->drawn, 1);
    same(third->drawn, 0);
}

TRIAL("VScroll: dziecko dostaje widoczność we własnych współrzędnych") {
    ui::VScroll one;

    const auto inner = std::make_shared<Sheet>(ui::Size{6, 10}, U'a');

    one.children().add(inner);

    one.shift(4);

    (void)one.render({{0, 0}, {6, 3}});

    same(inner->last.at.y, 4);
    same(inner->last.size.high, 3);
}

TRIAL("VScroll: kółko przewija, a na końcu oddaje zdarzenie wyżej") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::VScroll>();

    scroll->children().add(rows(10));
    top.children().add(scroll);

    (void)top.render({{0, 0}, {10, 3}});

    ui::WheelEvent down{{1, 1}, -1};

    ought(top.deal(down));
    same(scroll->shift(), 3);

    scroll->shift(7);

    (void)top.render({{0, 0}, {10, 3}});

    ui::WheelEvent more{{1, 1}, -1};

    ought(!top.deal(more));
    same(scroll->shift(), 7);
}

TRIAL("VScroll: kliknięcie trafia w to, co widać po przewinięciu") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::VScroll>();
    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Sheet>(ui::Size{6, 2}, U'a');
    const auto second = std::make_shared<Sheet>(ui::Size{6, 2}, U'b');

    column->children().add(first);
    column->children().add(second);
    scroll->children().add(column);
    top.children().add(scroll);

    scroll->shift(2);

    (void)top.render({{0, 0}, {6, 2}});

    same(first->area().at.y, 0);
    same(second->area().at.y, 2);
    same(scroll->children().front()->area().at.y, -2);
}

TRIAL("HScroll: przewinięcie przesuwa treść w lewo") {
    ui::HScroll one;

    one.children().add(std::make_shared<ui::Span>("abcdefghij"));

    one.shift(3);

    const View made = one.render({{0, 0}, {4, 1}});

    same(read(made, 0), std::string{"defg"});
}

TRIAL("HScroll: dalej niż koniec treści nie da się przewinąć") {
    ui::HScroll one;

    one.children().add(std::make_shared<ui::Span>("abcdefghij"));

    one.shift(100);

    (void)one.render({{0, 0}, {4, 1}});

    same(one.shift(), 6);
}

TRIAL("HScroll: chce co najwyżej tyle, ile dostał") {
    ui::HScroll one;

    one.children().add(std::make_shared<ui::Span>("abcdefghij"));

    same(one.wants({4, 1}).wide, 4);
    same(one.wants({40, 1}).wide, 10);
}

TRIAL("Scroll: przewija w obu kierunkach naraz") {
    ui::Scroll one;

    const auto inner = std::make_shared<Sheet>(ui::Size{20, 20}, U'a');

    one.children().add(inner);

    one.shift({3, 4});

    (void)one.render({{0, 0}, {5, 5}});

    same(one.shift().x, 3);
    same(one.shift().y, 4);
    same(inner->last.at.x, 3);
    same(inner->last.at.y, 4);
    same(inner->last.size.wide, 5);
    same(inner->last.size.high, 5);
}

TRIAL("Scroll: kółko przewija w pionie, a z Shiftem w poziomie") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>();

    scroll->children().add(std::make_shared<Sheet>(ui::Size{20, 20}, U'a'));
    top.children().add(scroll);

    (void)top.render({{0, 0}, {5, 5}});

    ui::WheelEvent down{{1, 1}, -1};

    ought(top.deal(down));

    same(scroll->shift().y, 3);
    same(scroll->shift().x, 0);

    ui::Mods with;
    with.shift = true;

    ui::WheelEvent aside{{1, 1}, -1, with};

    ought(top.deal(aside));

    same(scroll->shift().x, 3);
    same(scroll->shift().y, 3);
}

TRIAL("Scroll: przewinięcie nie wychodzi poza treść") {
    ui::Scroll one;

    one.children().add(std::make_shared<Sheet>(ui::Size{8, 8}, U'a'));

    one.shift({100, 100});

    (void)one.render({{0, 0}, {5, 5}});

    same(one.shift().x, 3);
    same(one.shift().y, 3);
}

TRIAL("Scroll: napis w przewijaku nie zawija się wcale") {
    ui::Scroll one;

    one.children().add(std::make_shared<ui::Span>("ala ma kota", ui::SpanStyle{true}));

    same(one.wants({4, 10}).high, 1);
}

TRIAL("Pasek: bez włączenia nie ma go wcale") {
    ui::VScroll one;

    one.children().add(rows(10));

    const View made = one.render({{0, 0}, {10, 3}});

    ought(!one.progress());
    same(read(made, 0), std::string{"wiersz 0"});
}

TRIAL("Pasek: strzałki na końcach, suwak pomiędzy") {
    ui::VScroll one{ui::ScrollStyle{true}};

    one.children().add(rows(20));

    const View made = one.render({{0, 0}, {10, 5}});

    ought(one.progress());
    same(made.at({9, 0}).letter, U'▲');
    same(made.at({9, 4}).letter, U'▼');
    same(made.at({9, 1}).letter, U'█');
    same(made.at({9, 3}).letter, U'░');
}

TRIAL("Pasek: suwak schodzi w dół razem z przewinięciem") {
    ui::VScroll one{ui::ScrollStyle{true}};

    one.children().add(rows(20));

    one.shift(100);

    const View made = one.render({{0, 0}, {10, 5}});

    same(made.at({9, 3}).letter, U'█');
    same(made.at({9, 1}).letter, U'░');
}

TRIAL("Pasek: treść mieszcząca się w oknie daje suwak na cały tor") {
    ui::VScroll one{ui::ScrollStyle{true}};

    one.children().add(rows(2));

    const View made = one.render({{0, 0}, {10, 5}});

    same(made.at({9, 1}).letter, U'█');
    same(made.at({9, 2}).letter, U'█');
    same(made.at({9, 3}).letter, U'█');
}

TRIAL("Pasek: zabiera treści jedną kolumnę") {
    ui::VScroll one;

    const auto inner = std::make_shared<Sheet>(ui::Size{20, 4}, U'a');

    one.children().add(inner);

    (void)one.render({{0, 0}, {10, 4}});

    same(inner->area().size.wide, 10);

    one.progress(true);

    (void)one.render({{0, 0}, {10, 4}});

    same(inner->area().size.wide, 9);
}

TRIAL("Pasek: wchodzi też do tego, ile przewijak chce") {
    ui::VScroll one{ui::ScrollStyle{true}};

    one.children().add(std::make_shared<ui::Span>("kot"));

    same(one.wants({80, 5}).wide, 4);
}

TRIAL("Pasek poziomy: strzałki w bok i dolny wiersz") {
    ui::HScroll one{ui::ScrollStyle{true}};

    one.children().add(std::make_shared<ui::Span>("abcdefghijklmnoprst"));

    const View made = one.render({{0, 0}, {6, 2}});

    same(made.at({0, 1}).letter, U'◀');
    same(made.at({5, 1}).letter, U'▶');
    same(read(made, 0), std::string{"abcdef"});
}

TRIAL("Scroll: dwa paski i pusty narożnik") {
    ui::Scroll one{ui::ScrollStyle{true}};

    one.children().add(std::make_shared<Sheet>(ui::Size{20, 20}, U'a'));

    const View made = one.render({{0, 0}, {6, 6}});

    same(made.at({5, 0}).letter, U'▲');
    same(made.at({5, 4}).letter, U'▼');
    same(made.at({0, 5}).letter, U'◀');
    same(made.at({4, 5}).letter, U'▶');
    same(made.at({5, 5}).letter, U' ');
}

TRIAL("Scroll: paski zabierają treści kolumnę i wiersz") {
    ui::Scroll one{ui::ScrollStyle{true}};

    const auto inner = std::make_shared<Sheet>(ui::Size{20, 20}, U'a');

    one.children().add(inner);

    (void)one.render({{0, 0}, {6, 6}});

    same(inner->last.size.wide, 5);
    same(inner->last.size.high, 5);
}

TRIAL("Przewijaki biorą jedno dziecko") {
    ui::VScroll first;
    ui::HScroll second;
    ui::Scroll third;

    same(first.takes(), 1);
    same(second.takes(), 1);
    same(third.takes(), 1);
}

// Suwak rysują wolne funkcje, które nie mają węzła, przez który dosięgłyby roli — więc rdzeń
// dostają w argumencie. Dzięki temu przewijak, lista, tabela i strona mogą mieć suwak ubrany
// osobno, zamiast jednego na całą bibliotekę.
TRIAL("VScroll: rdzeń roli suwaka można podmienić") {
    ui::Root top;

    const auto one = std::make_shared<ui::VScroll>(ui::ScrollStyle{true});
    const auto body = std::make_shared<Sheet>(ui::Size{4, 20}, U'x');

    same(one->bar(), std::string{"talcite.scroll"});

    one->children().add(body);
    top.children().add(one);

    ui::Styles these;

    Style look{};

    look.ink = Color{false, 6, 6, 6};

    these["moj.suwak"] = look;

    top.skin(ui::Skin{these});
    one->bar("moj.suwak");

    const View made = top.render({{0, 0}, {6, 5}});

    same(made.at({5, 0}).style.ink, (Color{false, 6, 6, 6}));
}
