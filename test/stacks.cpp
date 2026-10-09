#include "proving.hpp"

#include <memory>
#include <optional>

#include "talcite/column.hpp"
#include "talcite/root.hpp"
#include "talcite/row.hpp"
#include "talcite/stack.hpp"
#include "talcite/view.hpp"

using namespace talcite;

#include "doing.hpp"

namespace {

// Pudełko malujące jeden znak na całym przydziale — po to, żeby widać było, kto leży na wierzchu
// i gdzie stoi.
class Box : public ui::Node {
public:
    Box(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

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
    char32_t what_;
};

// Dziecko, które **samo mówi, gdzie stoi** — w jednym miejscu, mniejszym niż pokój. To jest cała
// różnica między nakładką w rogu a zasłoną na całości.
class Pinned : public Box {
public:
    Pinned(ui::Area where, char32_t what) : Box(where.size, what), where_(where) {}

    [[nodiscard]] ui::Area placement(ui::Size) const override { return where_; }

private:
    ui::Area where_;
};

class Sheet : public ui::Root {
public:
    [[nodiscard]] int capacity() const override { return ui::plenty; }
};

}

// --- Domyślne miejsce ----------------------------------------------------------------------------

// Zwykły węzeł bierze w stosie cały pokój. Tyle bierze zasłona, żeby łapać mysz wszędzie, i tyle
// bierze treść aplikacji — więc to jest domyślna odpowiedź, a nie osobne ustawienie.
TRIAL("Stack: dziecko bez własnego miejsca bierze cały pokój") {
    ui::Stack stack;

    const auto one = std::make_shared<Box>(ui::Size{2, 1}, U'a');

    stack.children().add(one);

    (void)stack.render({{0, 0}, {6, 3}});

    same(one->area(), (ui::Area{{0, 0}, {6, 3}}));
}

// --- Własne miejsce ------------------------------------------------------------------------------

// **Po to jest `placement`.** Dziecko staje tam, gdzie powiedziało, a reszta pokoju zostaje
// przezroczysta — spod nakładki widać to, co leży niżej.
TRIAL("Stack: dziecko z własnym miejscem staje tam, gdzie powiedziało") {
    ui::Stack stack;

    const auto under = std::make_shared<Box>(ui::Size{6, 3}, U'a');
    const auto over = std::make_shared<Pinned>(ui::Area{{2, 1}, {3, 1}}, U'b');

    stack.children().add(under);
    stack.children().add(over);

    const View made = stack.render({{0, 0}, {6, 3}});

    same(over->area(), (ui::Area{{2, 1}, {3, 1}}));

    same(made.at({0, 0}).letter, U'a');
    same(made.at({2, 1}).letter, U'b');
    same(made.at({4, 1}).letter, U'b');
    same(made.at({5, 1}).letter, U'a');
    same(made.at({2, 2}).letter, U'a');
}

// **Stos jest przezroczysty tam, gdzie żadne dziecko nie leży** — inaczej zamalowywałby spacjami
// wszystko, co stoi pod nim w rodzicu. Widać to dopiero wtedy, gdy stos sam jest nakładką:
// pierwsza wersja tej próby nie miała nic pod stosem i przeszła zdjęcie `clear`, bo nie było
// czego zamalować.
TRIAL("Stack: poza dziećmi przepuszcza to, co leży pod nim") {
    ui::Stack outer;

    const auto under = std::make_shared<Box>(ui::Size{6, 3}, U'a');
    const auto inner = std::make_shared<ui::Stack>();

    inner->children().add(std::make_shared<Pinned>(ui::Area{{2, 1}, {2, 1}}, U'b'));

    outer.children().add(under);
    outer.children().add(inner);

    const View made = outer.render({{0, 0}, {6, 3}});

    same(made.at({2, 1}).letter, U'b');
    same(made.at({0, 0}).letter, U'a');
    same(made.at({5, 2}).letter, U'a');
}

// Dziecko może wejść na rodzeństwo, ale nie może wyjść poza rodzica — i pilnuje tego **pojemnik**,
// bo tu jest jedno miejsce, a w dzieciach byłoby ich tyle, ile klas.
TRIAL("Stack: miejsce wystające poza pokój zostaje przycięte") {
    ui::Stack stack;

    const auto one = std::make_shared<Pinned>(ui::Area{{4, 2}, {10, 10}}, U'b');

    stack.children().add(one);

    const View made = stack.render({{0, 0}, {6, 3}});

    same(one->area(), (ui::Area{{4, 2}, {2, 1}}));
    same(made.size(), (ui::Size{6, 3}));
}

TRIAL("Stack: miejsce poza pokojem daje pusty prostokąt, nie ujemny") {
    ui::Stack stack;

    const auto one = std::make_shared<Pinned>(ui::Area{{-3, 8}, {4, 4}}, U'b');

    stack.children().add(one);

    (void)stack.render({{0, 0}, {6, 3}});

    ought(one->area().size.wide >= 0);
    ought(one->area().size.high >= 0);
    ought(one->area().at.x >= 0);
    ought(one->area().at.y <= 3);
}

// --- Trafianie -----------------------------------------------------------------------------------

// Mysz czyta tę samą kolejność i te same prostokąty, którymi stos maluje: klik w nakładkę trafia
// w nakładkę, klik obok niej — w to, co pod spodem.
TRIAL("Stack: klik trafia w nakładkę tylko tam, gdzie ona jest") {
    Sheet top;

    const auto stack = std::make_shared<ui::Stack>();
    const auto under = std::make_shared<Box>(ui::Size{6, 3}, U'a');
    const auto over = std::make_shared<Pinned>(ui::Area{{2, 1}, {3, 1}}, U'b');

    stack->children().add(under);
    stack->children().add(over);
    top.children().add(stack);

    (void)top.render({{0, 0}, {6, 3}});

    int hits = 0;

    (void)over->on<ui::ClickEvent>([&hits](ui::ClickEvent&) {
        ++hits;

        return true;
    });

    ui::ClickEvent inside{{3, 1}, ui::Mouse::left, {}};

    (void)top.dispatch(inside);
    same(hits, 1);

    ui::ClickEvent outside{{0, 0}, ui::Mouse::left, {}};

    (void)top.dispatch(outside);
    same(hits, 1);
}

// --- screen_area, area_in -----------------------------------------------------------------------

// Miejsce na ekranie składa się z prostokątów w górę drzewa i **liczy się przy każdym pytaniu**:
// współrzędna zapamiętana przy rozwinięciu zestarzałaby się przy pierwszym przesunięciu czegokolwiek
// powyżej.
//
// Węzeł stoi **dwa piętra w dół i z przesunięciem na każdym** — bo pierwsza wersja tej próby
// miała kolumnę w (0, 0) i przeszła zdjęcie składania pięter: własny prostokąt węzła był wtedy
// równy miejscu na ekranie i próba nie widziała różnicy. Piąty raz ta sama pomyłka: pytanie
// o miejsce, na którym mechanizm nie zostawia śladu.
TRIAL("screen_area: składa miejsce na ekranie przez piętra i widzi przesunięcie") {
    Sheet top;

    const auto outer = std::make_shared<ui::Column>();
    const auto head = std::make_shared<Box>(ui::Size{6, 1}, U'h');
    const auto inner = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Box>(ui::Size{6, 1}, U'a');
    const auto second = std::make_shared<Box>(ui::Size{6, 1}, U'b');

    inner->children().add(first);
    inner->children().add(second);
    outer->children().add(head);
    outer->children().add(inner);
    top.children().add(outer);

    (void)top.render({{0, 0}, {6, 4}});

    // Własny prostokąt mówi „drugi wiersz kolumny wewnętrznej"; miejsce na ekranie — „trzeci".
    same(second->area().at, (ui::Spot{0, 1}));

    const std::optional<ui::Area> where = second->screen_area();

    ought(where.has_value());
    same(where->at, (ui::Spot{0, 2}));

    // Coś powyżej zniknęło — miejsce ma to zobaczyć bez żadnego zawiadomienia.
    outer->children().remove(head.get());
    outer->children().add(std::make_shared<Box>(ui::Size{6, 2}, U'c'));

    (void)top.render({{0, 0}, {6, 5}});

    same(second->screen_area()->at, (ui::Spot{0, 1}));
}

TRIAL("screen_area: węzeł bez prostokąta nie ma miejsca") {
    const auto one = std::make_shared<Box>(ui::Size{6, 1}, U'a');

    ought(!one->screen_area().has_value());
}

// Ta sama pomyłka w drugiej osi: próba wyżej stoi cała przy lewej krawędzi, więc nie widziała,
// czy przesunięcia przodków w poziomie w ogóle się sumują.
TRIAL("screen_area: sumuje przesunięcia przodków także w poziomie") {
    Sheet top;

    const auto row = std::make_shared<ui::Row>();
    const auto inner = std::make_shared<ui::Row>();
    const auto first = std::make_shared<Box>(ui::Size{3, 1}, U'a');
    const auto second = std::make_shared<Box>(ui::Size{2, 1}, U'b');
    const auto third = std::make_shared<Box>(ui::Size{2, 1}, U'c');

    inner->children().add(second);
    inner->children().add(third);
    row->children().add(first);
    row->children().add(inner);
    top.children().add(row);

    (void)top.render({{0, 0}, {10, 1}});

    same(third->area().at, (ui::Spot{2, 0}));
    same(third->screen_area()->at, (ui::Spot{5, 0}));
    same(third->local({6, 0}), (ui::Spot{1, 0}));
}

// Prostokąt w układzie przodka: bez przesunięcia samego przodka, a dla węzła spoza niego — pustka.
// Przewijak pyta tak o węzeł z ogniskiem, więc odpowiedź „jest gdzieś" dla węzła obok
// przewinęłaby treść do miejsca, którego w niej nie ma.
TRIAL("area_in: liczy od przodka i nie odpowiada za węzły spoza niego") {
    Sheet top;

    const auto row = std::make_shared<ui::Row>();
    const auto inner = std::make_shared<ui::Row>();
    const auto first = std::make_shared<Box>(ui::Size{3, 1}, U'a');
    const auto second = std::make_shared<Box>(ui::Size{2, 1}, U'b');
    const auto third = std::make_shared<Box>(ui::Size{2, 1}, U'c');

    inner->children().add(second);
    inner->children().add(third);
    row->children().add(first);
    row->children().add(inner);
    top.children().add(row);

    (void)top.render({{0, 0}, {10, 1}});

    same(third->area_in(*inner)->at, (ui::Spot{2, 0}));
    same(third->area_in(*row)->at, (ui::Spot{5, 0}));
    ought(!third->area_in(*first).has_value());
    ought(!first->area_in(*inner).has_value());
}
