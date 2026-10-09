#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/box.hpp"
#include "talcite/column.hpp"
#include "talcite/encoding.hpp"
#include "talcite/event.hpp"
#include "talcite/field.hpp"
#include "talcite/scroll.hpp"
#include "talcite/root.hpp"
#include "talcite/row.hpp"
#include "talcite/scroll.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"
#include "talcite/scroll.hpp"

#include "scrollbar.hpp"

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
                made.put({x - seen.at.x, y - seen.at.y},
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

        if (letter != 0) talcite::utf8::encode(letter, made);
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

TRIAL("Scroll w pionie: bez przewinięcia widać początek treści") {
    ui::Scroll one{ui::Axis::vertical};

    one.children().add(rows(10));

    const View made = one.render({{0, 0}, {10, 3}});

    same(read(made, 0), std::string{"wiersz 0"});
    same(read(made, 2), std::string{"wiersz 2"});
    same(one.shift().y, 0);
}

TRIAL("Scroll w pionie: przewinięcie przesuwa treść w górę") {
    ui::Scroll one{ui::Axis::vertical};

    one.children().add(rows(10));

    one.shift({0, 4});

    const View made = one.render({{0, 0}, {10, 3}});

    same(read(made, 0), std::string{"wiersz 4"});
    same(read(made, 1), std::string{"wiersz 5"});
}

TRIAL("Scroll w pionie: dalej niż koniec treści nie da się przewinąć") {
    ui::Scroll one{ui::Axis::vertical};

    one.children().add(rows(5));

    one.shift({0, 100});

    const View made = one.render({{0, 0}, {10, 3}});

    same(one.shift().y, 2);
    same(read(made, 0), std::string{"wiersz 2"});
}

TRIAL("Scroll w pionie: treść krótsza niż okno nie przewija się wcale") {
    ui::Scroll one{ui::Axis::vertical};

    one.children().add(rows(2));

    one.shift({0, 5});

    (void)one.render({{0, 0}, {10, 6}});

    same(one.shift().y, 0);
}

TRIAL("Scroll w pionie: ujemne przewinięcie schodzi do zera") {
    ui::Scroll one{ui::Axis::vertical};

    one.shift({0, -5});

    same(one.shift().y, 0);
}

TRIAL("Scroll w pionie: rysuje tylko wiersze, które widać") {
    ui::Scroll one{ui::Axis::vertical};

    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Sheet>(ui::Size{6, 2}, U'a');
    const auto second = std::make_shared<Sheet>(ui::Size{6, 2}, U'b');
    const auto third = std::make_shared<Sheet>(ui::Size{6, 2}, U'c');

    column->children().add(first);
    column->children().add(second);
    column->children().add(third);

    one.children().add(column);

    one.shift({0, 2});

    (void)one.render({{0, 0}, {6, 2}});

    same(first->drawn, 0);
    same(second->drawn, 1);
    same(third->drawn, 0);
}

TRIAL("Scroll w pionie: dziecko dostaje widoczność we własnych współrzędnych") {
    ui::Scroll one{ui::Axis::vertical};

    const auto inner = std::make_shared<Sheet>(ui::Size{6, 10}, U'a');

    one.children().add(inner);

    one.shift({0, 4});

    (void)one.render({{0, 0}, {6, 3}});

    same(inner->last.at.y, 4);
    same(inner->last.size.high, 3);
}

TRIAL("Scroll w pionie: kółko przewija, a na końcu oddaje zdarzenie wyżej") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::vertical);

    scroll->children().add(rows(10));
    top.children().add(scroll);

    (void)top.render({{0, 0}, {10, 3}});

    ui::WheelEvent down{{1, 1}, -1};

    ought(top.dispatch(down));
    same(scroll->shift().y, 3);

    scroll->shift({0, 7});

    (void)top.render({{0, 0}, {10, 3}});

    ui::WheelEvent more{{1, 1}, -1};

    ought(!top.dispatch(more));
    same(scroll->shift().y, 7);
}

TRIAL("Scroll w pionie: kliknięcie trafia w to, co widać po przewinięciu") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::vertical);
    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Sheet>(ui::Size{6, 2}, U'a');
    const auto second = std::make_shared<Sheet>(ui::Size{6, 2}, U'b');

    column->children().add(first);
    column->children().add(second);
    scroll->children().add(column);
    top.children().add(scroll);

    scroll->shift({0, 2});

    (void)top.render({{0, 0}, {6, 2}});

    same(first->area().at.y, 0);
    same(second->area().at.y, 2);
    same(scroll->children().front()->area().at.y, -2);
}

TRIAL("Scroll w poziomie: przewinięcie przesuwa treść w lewo") {
    ui::Scroll one{ui::Axis::horizontal};

    one.children().add(std::make_shared<ui::Span>("abcdefghij"));

    one.shift({3, 0});

    const View made = one.render({{0, 0}, {4, 1}});

    same(read(made, 0), std::string{"defg"});
}

TRIAL("Scroll w poziomie: dalej niż koniec treści nie da się przewinąć") {
    ui::Scroll one{ui::Axis::horizontal};

    one.children().add(std::make_shared<ui::Span>("abcdefghij"));

    one.shift({100, 0});

    (void)one.render({{0, 0}, {4, 1}});

    same(one.shift().x, 6);
}

TRIAL("Scroll w poziomie: chce co najwyżej tyle, ile dostał") {
    ui::Scroll one{ui::Axis::horizontal};

    one.children().add(std::make_shared<ui::Span>("abcdefghij"));

    same(one.wants({4, 1}).wide, 4);
    same(one.wants({40, 1}).wide, 10);
}

TRIAL("Scroll: przewija w obu kierunkach naraz") {
    ui::Scroll one{ui::Axis::both};

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

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::both);

    scroll->children().add(std::make_shared<Sheet>(ui::Size{20, 20}, U'a'));
    top.children().add(scroll);

    (void)top.render({{0, 0}, {5, 5}});

    ui::WheelEvent down{{1, 1}, -1};

    ought(top.dispatch(down));

    same(scroll->shift().y, 3);
    same(scroll->shift().x, 0);

    ui::Mods with;
    with.shift = true;

    ui::WheelEvent aside{{1, 1}, -1, with};

    ought(top.dispatch(aside));

    same(scroll->shift().x, 3);
    same(scroll->shift().y, 3);
}

TRIAL("Scroll: przewinięcie nie wychodzi poza treść") {
    ui::Scroll one{ui::Axis::both};

    one.children().add(std::make_shared<Sheet>(ui::Size{8, 8}, U'a'));

    one.shift({100, 100});

    (void)one.render({{0, 0}, {5, 5}});

    same(one.shift().x, 3);
    same(one.shift().y, 3);
}

TRIAL("Scroll: napis w przewijaku nie zawija się wcale") {
    ui::Scroll one{ui::Axis::both};

    one.children().add(std::make_shared<ui::Span>("ala ma kota", ui::SpanShape{true}));

    same(one.wants({4, 10}).high, 1);
}

TRIAL("Pasek: bez włączenia nie ma go wcale") {
    ui::Scroll one{ui::Axis::vertical};

    one.children().add(rows(10));

    const View made = one.render({{0, 0}, {10, 3}});

    ought(!one.scrollbar());
    same(read(made, 0), std::string{"wiersz 0"});
}

TRIAL("Pasek: strzałki na końcach, suwak pomiędzy") {
    ui::Scroll one{ui::Axis::vertical, ui::ScrollShape{true}};

    one.children().add(rows(20));

    const View made = one.render({{0, 0}, {10, 5}});

    ought(one.scrollbar());
    same(made.at({9, 0}).letter, U'▲');
    same(made.at({9, 4}).letter, U'▼');
    same(made.at({9, 1}).letter, U'█');
    same(made.at({9, 3}).letter, U'░');
}

TRIAL("Pasek: suwak schodzi w dół razem z przewinięciem") {
    ui::Scroll one{ui::Axis::vertical, ui::ScrollShape{true}};

    one.children().add(rows(20));

    one.shift({0, 100});

    const View made = one.render({{0, 0}, {10, 5}});

    same(made.at({9, 3}).letter, U'█');
    same(made.at({9, 1}).letter, U'░');
}

// Strona liczy pasek w bajtach, więc plik większy niż `int` to zwykły przypadek, nie ciekawostka.
// Trzy miliardy bajtów, przewinięte do połowy: suwak ma stać w połowie toru — dziesięć kratek toru,
// jedna na suwak, więc piąta od góry pod strzałką.
TRIAL("Pasek: trzy miliardy w połowie dają suwak w połowie") {
    const ui::Skin how;
    const std::string role = "talcite.scroll";

    View made{{1, 12}};

    ui::Scrollbar{.at = {0, 0},
                  .much = 12,
                  .shift = 1'500'000'000,
                  .whole = 3'000'000'000,
                  .window = 1000,
                  .skin = how,
                  .role = role}
        .draw(made);

    same(made.at({0, 0}).letter, U'▲');
    same(made.at({0, 11}).letter, U'▼');
    same(made.at({0, 5}).letter, U'░');
    same(made.at({0, 6}).letter, U'█');
    same(made.at({0, 7}).letter, U'░');
}

// Druga połowa tej samej pomyłki: długość suwaka to tor razy okno przez całość, a przy oknie rzędu
// miliarda sam iloczyn nie mieści się w `int`. Okno na pół całości daje suwak na pół toru.
TRIAL("Pasek: okno na pół z trzech miliardów daje suwak na pół toru") {
    const ui::Skin how;
    const std::string role = "talcite.scroll";

    View made{{1, 12}};

    ui::Scrollbar{.at = {0, 0},
                  .much = 12,
                  .shift = 0,
                  .whole = 3'000'000'000,
                  .window = 1'500'000'000,
                  .skin = how,
                  .role = role}
        .draw(made);

    for (int y = 1; y <= 5; ++y) same(made.at({0, y}).letter, U'█');
    for (int y = 6; y <= 10; ++y) same(made.at({0, y}).letter, U'░');
}

TRIAL("Pasek: treść mieszcząca się w oknie daje suwak na cały tor") {
    ui::Scroll one{ui::Axis::vertical, ui::ScrollShape{true}};

    one.children().add(rows(2));

    const View made = one.render({{0, 0}, {10, 5}});

    same(made.at({9, 1}).letter, U'█');
    same(made.at({9, 2}).letter, U'█');
    same(made.at({9, 3}).letter, U'█');
}

// **Pasek „gdy trzeba"** (`needed`): treść, która mieści się w oknie, dostaje całą szerokość i nie ma
// paska; dłuższa ma go jak zawsze. Przewijak chce wtedy tyle, ile treść, bez kolumny na pasek.
TRIAL("Pasek gdy trzeba: treść mieszcząca się nie ma paska, dłuższa ma") {
    ui::Scroll one{ui::Axis::vertical, ui::ScrollShape{true, true}};

    const auto inner = std::make_shared<Sheet>(ui::Size{10, 4}, U'a');

    one.children().add(inner);

    const View fits = one.render({{0, 0}, {10, 5}});

    same(inner->area().size.wide, 10);
    same(fits.at({9, 0}).letter, U'a');
    same(one.wants({80, 5}).wide, 10);

    const View tight = one.render({{0, 0}, {10, 3}});

    same(inner->area().size.wide, 9);
    same(tight.at({9, 0}).letter, U'▲');
    same(tight.at({9, 2}).letter, U'▼');
    same(one.wants({80, 3}).wide, 11);

    // Treść równa oknu też się mieści — ostatnia kolumna należy do treści (wiersz 3 to „d").
    const View exact = one.render({{0, 0}, {10, 4}});

    same(exact.at({9, 3}).letter, U'd');
}

// Oba paski „gdy trzeba": pionowy staje, bo treść jest za wysoka — i zabiera kolumnę, przez co treść
// dokładnie tak szeroka jak okno przestaje się mieścić w poziomie. Drugie przejście to widzi.
TRIAL("Pasek gdy trzeba: pionowy wywołuje poziomy, gdy zabrał kolumnę") {
    ui::Scroll one{ui::Axis::both, ui::ScrollShape{true, true}};

    one.children().add(std::make_shared<Sheet>(ui::Size{6, 20}, U'a'));

    const View made = one.render({{0, 0}, {6, 6}});

    same(made.at({5, 0}).letter, U'▲');
    same(made.at({0, 5}).letter, U'◀');

    ui::Scroll wide{ui::Axis::both, ui::ScrollShape{true, true}};

    wide.children().add(std::make_shared<Sheet>(ui::Size{5, 5}, U'a'));

    const View none = wide.render({{0, 0}, {6, 6}});

    same(none.at({5, 0}).letter, U' ');
    same(none.at({0, 5}).letter, U' ');

    // Treść dokładnie tak szeroka jak okno mieści się w poziomie.
    ui::Scroll exact{ui::Axis::both, ui::ScrollShape{true, true}};

    exact.children().add(std::make_shared<Sheet>(ui::Size{6, 5}, U'a'));

    same(exact.render({{0, 0}, {6, 6}}).at({0, 5}).letter, U' ');

    // Przewijak tylko poziomy nie ma paska pionowego, choćby treść była wyższa od okna.
    ui::Scroll across{ui::Axis::horizontal, ui::ScrollShape{true, true}};

    across.children().add(std::make_shared<Sheet>(ui::Size{20, 5}, U'a'));

    const View flat = across.render({{0, 0}, {6, 3}});

    same(flat.at({5, 0}).letter, U'a');
    same(flat.at({0, 2}).letter, U'◀');
}

TRIAL("Pasek: zabiera treści jedną kolumnę") {
    ui::Scroll one{ui::Axis::vertical};

    const auto inner = std::make_shared<Sheet>(ui::Size{20, 4}, U'a');

    one.children().add(inner);

    (void)one.render({{0, 0}, {10, 4}});

    same(inner->area().size.wide, 10);

    one.scrollbar(true);

    (void)one.render({{0, 0}, {10, 4}});

    same(inner->area().size.wide, 9);
}

TRIAL("Pasek: wchodzi też do tego, ile przewijak chce") {
    ui::Scroll one{ui::Axis::vertical, ui::ScrollShape{true}};

    one.children().add(std::make_shared<ui::Span>("kot"));

    same(one.wants({80, 5}).wide, 4);
}

TRIAL("Pasek poziomy: strzałki w bok i dolny wiersz") {
    ui::Scroll one{ui::Axis::horizontal, ui::ScrollShape{true}};

    one.children().add(std::make_shared<ui::Span>("abcdefghijklmnoprst"));

    const View made = one.render({{0, 0}, {6, 2}});

    same(made.at({0, 1}).letter, U'◀');
    same(made.at({5, 1}).letter, U'▶');
    same(read(made, 0), std::string{"abcdef"});
}

TRIAL("Scroll: dwa paski i pusty narożnik") {
    ui::Scroll one{ui::Axis::both, ui::ScrollShape{true}};

    one.children().add(std::make_shared<Sheet>(ui::Size{20, 20}, U'a'));

    const View made = one.render({{0, 0}, {6, 6}});

    same(made.at({5, 0}).letter, U'▲');
    same(made.at({5, 4}).letter, U'▼');
    same(made.at({0, 5}).letter, U'◀');
    same(made.at({4, 5}).letter, U'▶');
    same(made.at({5, 5}).letter, U' ');
}

TRIAL("Scroll: paski zabierają treści kolumnę i wiersz") {
    ui::Scroll one{ui::Axis::both, ui::ScrollShape{true}};

    const auto inner = std::make_shared<Sheet>(ui::Size{20, 20}, U'a');

    one.children().add(inner);

    (void)one.render({{0, 0}, {6, 6}});

    same(inner->last.size.wide, 5);
    same(inner->last.size.high, 5);
}

TRIAL("Przewijaki biorą jedno dziecko") {
    ui::Scroll first{ui::Axis::vertical};
    ui::Scroll second{ui::Axis::horizontal};
    ui::Scroll third{ui::Axis::both};

    same(first.capacity(), 1);
    same(second.capacity(), 1);
    same(third.capacity(), 1);
}

// Suwak rysują wolne funkcje, które nie mają węzła, przez który dosięgłyby roli — więc rdzeń
// dostają w argumencie. Dzięki temu przewijak, lista, tabela i strona mogą mieć suwak ubrany
// osobno, zamiast jednego na całą bibliotekę.
TRIAL("Scroll w pionie: rdzeń roli suwaka można podmienić") {
    ui::Root top;

    const auto one = std::make_shared<ui::Scroll>(ui::Axis::vertical, ui::ScrollShape{true});
    const auto body = std::make_shared<Sheet>(ui::Size{4, 20}, U'x');

    same(one->scrollbar_role(), std::string{"talcite.scroll"});

    one->children().add(body);
    top.children().add(one);

    ui::Styles these;

    Style look{};

    look.foreground = Color{false, 6, 6, 6};

    these["moj.suwak"] = look;

    top.skin(ui::Skin{these});
    one->scrollbar_role("moj.suwak");

    const View made = top.render({{0, 0}, {6, 5}});

    same(made.at({5, 0}).style.foreground, (Color{false, 6, 6, 6}));
}

// --- Za ogniskiem ---------------------------------------------------------------------------

namespace {

std::shared_ptr<ui::Column> fields(int much, int wide = 6) {
    auto made = std::make_shared<ui::Column>();

    for (int which = 0; which < much; ++which)
        made->children().add(std::make_shared<ui::Field>(wide, std::to_string(which)));

    return made;
}

}

// Ognisko w polu poza oknem dosuwa przewinięcie w następnej klatce — w `paint`, jak w liście, bo
// dopiero po rozstawieniu znany jest prostokąt pola. Do dołu, gdy pole jest niżej; do góry, gdy
// wyżej; pole w oknie niczego nie rusza.
TRIAL("Scroll w pionie: ognisko poza oknem dosuwa przewinięcie, w oknie nie rusza") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::vertical);
    const auto column = fields(30);

    scroll->children().add(column);
    top.children().add(scroll);

    (void)top.render({{0, 0}, {10, 5}});

    same(scroll->shift().y, 0);

    column->children()[19]->focus();

    View shown = top.render({{0, 0}, {10, 5}});

    same(scroll->shift().y, 15);
    same(read(shown, 4), std::string{"19"});

    column->children()[17]->focus();

    (void)top.render({{0, 0}, {10, 5}});

    same(scroll->shift().y, 15);

    column->children()[3]->focus();

    shown = top.render({{0, 0}, {10, 5}});

    same(scroll->shift().y, 3);
    same(read(shown, 0), std::string{"3"});

    column->children()[0]->focus();

    (void)top.render({{0, 0}, {10, 5}});

    same(scroll->shift().y, 0);

    column->children()[29]->focus();

    (void)top.render({{0, 0}, {10, 5}});

    same(scroll->shift().y, 25);
}

// Kółko przesuwa widok, ognisko zostaje — i następna klatka nie cofa kółka: za ogniskiem idzie
// się tylko w klatce po jego zmianie. Ognisko poza przewijakiem niczego nie rusza.
TRIAL("Scroll w pionie: kółko nie kłóci się z ogniskiem, a ognisko obok przewijaka nic nie rusza") {
    ui::Root top;

    const auto whole = std::make_shared<ui::Column>();
    const auto box = std::make_shared<ui::Box>(ui::Size{10, 5});
    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::vertical);
    const auto column = fields(30);
    const auto outside = std::make_shared<ui::Field>(6, "obok");

    scroll->children().add(column);
    box->children().add(scroll);
    whole->children().add(box);
    whole->children().add(outside);
    top.children().add(whole);

    (void)top.render({{0, 0}, {10, 6}});

    column->children()[10]->focus();

    (void)top.render({{0, 0}, {10, 6}});

    same(scroll->shift().y, 6);

    ui::WheelEvent up{{1, 1}, 1};

    ought(top.dispatch(up));
    same(scroll->shift().y, 3);

    (void)top.render({{0, 0}, {10, 6}});

    same(scroll->shift().y, 3);

    outside->focus();

    (void)top.render({{0, 0}, {10, 6}});

    same(scroll->shift().y, 3);
}

// Węzeł wyższy niż okno dosuwa się górą — początek jest ważniejszy niż koniec.
TRIAL("Scroll w pionie: węzeł wyższy niż okno dosuwa się górą") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::vertical);
    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Sheet>(ui::Size{6, 4}, U'a');
    const auto tall = std::make_shared<ui::Field>(6, "x");
    const auto box = std::make_shared<ui::Column>();

    for (int which = 0; which < 6; ++which) box->children().add(std::make_shared<ui::Span>("w"));

    box->children().add(tall);

    column->children().add(first);
    column->children().add(box);
    scroll->children().add(column);
    top.children().add(scroll);

    (void)top.render({{0, 0}, {6, 3}});

    tall->focus();

    (void)top.render({{0, 0}, {6, 3}});

    same(scroll->shift().y, 8);
}

TRIAL("Scroll w poziomie: ognisko poza oknem dosuwa przewinięcie w poziomie") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::horizontal);
    const auto row = std::make_shared<ui::Row>();

    for (int which = 0; which < 10; ++which)
        row->children().add(std::make_shared<ui::Field>(4, std::to_string(which)));

    scroll->children().add(row);
    top.children().add(scroll);

    (void)top.render({{0, 0}, {10, 1}});

    row->children()[5]->focus();

    (void)top.render({{0, 0}, {10, 1}});

    same(scroll->shift().x, 14);

    row->children()[0]->focus();

    (void)top.render({{0, 0}, {10, 1}});

    same(scroll->shift().x, 0);
}

TRIAL("Scroll: ognisko dosuwa przewinięcie w obu osiach naraz") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::both);
    const auto column = std::make_shared<ui::Column>();

    for (int y = 0; y < 8; ++y) {
        const auto row = std::make_shared<ui::Row>();

        for (int x = 0; x < 8; ++x)
            row->children().add(std::make_shared<ui::Field>(4, std::to_string(y) + std::to_string(x)));

        column->children().add(row);
    }

    scroll->children().add(column);
    top.children().add(scroll);

    (void)top.render({{0, 0}, {10, 3}});

    const auto& row = column->children()[6];

    row->children()[5]->focus();

    (void)top.render({{0, 0}, {10, 3}});

    same(scroll->shift().x, 14);
    same(scroll->shift().y, 4);

    column->children()[0]->children()[0]->focus();

    (void)top.render({{0, 0}, {10, 3}});

    same(scroll->shift().x, 0);
    same(scroll->shift().y, 0);
}

// --- Oś przewijania ------------------------------------------------------------------------------

namespace {

// Węzeł, który zapamiętuje, o jaki pokój go pytano.
class Asked : public ui::Node {
public:
    mutable ui::Size room;

    [[nodiscard]] ui::Size wants(ui::Size given) const override {
        room = given;

        return {4, 4};
    }
};

ui::Size asked(ui::Axis axis) {
    ui::Scroll one{axis, ui::ScrollShape{true}};

    const auto probe = std::make_shared<Asked>();

    one.children().add(probe);

    (void)one.wants({20, 10});

    return probe->room;
}

ui::Size window(ui::Axis axis) {
    ui::Scroll one{axis, ui::ScrollShape{true}};

    const auto sheet = std::make_shared<Sheet>(ui::Size{30, 30}, U'a');

    one.children().add(sheet);

    (void)one.render({{0, 0}, {10, 5}});

    return sheet->last.size;
}

}

// Oś otwarta pyta o `plenty` (bez paska), zamknięta o pokój bez paska — dlatego napis w przewijaku
// pionowym zawija się do szerokości okna, a w poziomym zostaje jednym długim wierszem.
TRIAL("Scroll: oś otwarta pyta dziecko o plenty, zamknięta o pokój") {
    const ui::Size vertical = asked(ui::Axis::vertical);
    same(vertical.wide, 19);
    same(vertical.high, ui::plenty);

    const ui::Size horizontal = asked(ui::Axis::horizontal);
    same(horizontal.wide, ui::plenty);
    same(horizontal.high, 9);

    const ui::Size both = asked(ui::Axis::both);
    same(both.wide, ui::plenty - 1);
    same(both.high, ui::plenty - 1);
}

TRIAL("Scroll: pasek stoi tylko przy osi otwartej") {
    same(window(ui::Axis::vertical).wide, 9);
    same(window(ui::Axis::vertical).high, 5);

    same(window(ui::Axis::horizontal).wide, 10);
    same(window(ui::Axis::horizontal).high, 4);

    same(window(ui::Axis::both).wide, 9);
    same(window(ui::Axis::both).high, 4);
}

TRIAL("Scroll: składowa zamkniętej osi jest zawsze zerem") {
    ui::Scroll vertical{ui::Axis::vertical};
    vertical.shift({5, 2});
    same(vertical.shift().x, 0);
    same(vertical.shift().y, 2);

    ui::Scroll horizontal{ui::Axis::horizontal};
    horizontal.shift({5, 2});
    same(horizontal.shift().x, 5);
    same(horizontal.shift().y, 0);

    ui::Scroll both{ui::Axis::both};
    both.shift({5, 2});
    same(both.shift().x, 5);
    same(both.shift().y, 2);

    same(both.axis(), ui::Axis::both);
}

TRIAL("Scroll w poziomie: kółko przewija w poziomie, bez Shifta") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::horizontal);

    scroll->children().add(std::make_shared<Sheet>(ui::Size{30, 2}, U'a'));
    top.children().add(scroll);

    (void)top.render({{0, 0}, {10, 2}});

    ui::WheelEvent down{{1, 1}, -1};

    ought(top.dispatch(down));
    same(scroll->shift().x, 3);
    same(scroll->shift().y, 0);
}

TRIAL("Scroll w pionie: kółko z Shiftem nie przewija w poziomie") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::vertical);

    scroll->children().add(rows(10));
    top.children().add(scroll);

    (void)top.render({{0, 0}, {10, 3}});

    ui::WheelEvent down{{1, 1}, -1, ui::Mods{.shift = true}};

    ought(top.dispatch(down));
    same(scroll->shift().y, 3);
    same(scroll->shift().x, 0);
}
