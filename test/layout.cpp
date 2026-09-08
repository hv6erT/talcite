#include "proving.hpp"

#include <algorithm>
#include <memory>

#include "talcite/column.hpp"
#include "talcite/root.hpp"
#include "talcite/row.hpp"
#include "talcite/view.hpp"

using namespace talcite;

namespace {

// Węzeł, który chce z góry ustalonego rozmiaru i zamalowuje przydzieloną kartkę jedną literą.
// Tyle wystarczy, żeby sprawdzić i rachunek układu, i to, gdzie trafia treść.
class Box : public ui::Node {
public:
    Box(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

    int hit = 0;

protected:
    View paint(ui::Size given, ui::Area seen) override {
        View made{seen.size};

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                made.letter({x - seen.at.x, y - seen.at.y}, what_, Style{});

        return made;
    }

    bool react(ui::MouseEvent&) override {
        ++hit;

        return true;
    }

    bool react(ui::WheelEvent&) override {
        ++hit;

        return true;
    }

private:
    ui::Size wants_;
    char32_t what_ = U'?';
};

// Pojemnik, który kładzie wszystkie dzieci na sobie, w tym samym miejscu. Do sprawdzenia, kto
// wygrywa przy trafianiu myszą.
class Stack : public ui::Node {
public:
    [[nodiscard]] int takes() const override { return ui::plenty; }

protected:
    View paint(ui::Size given, ui::Area seen) override {
        View made{seen.size};

        int highest = 0;

        for (const std::shared_ptr<Node>& one : children())
            highest = std::max(highest, one->layer());

        // Dzieci leżą na sobie, więc kolejność malowania jest tu wszystkim: najpierw niższe
        // warstwy, potem wyższe, a w obrębie warstwy — kolejność drzewa.
        for (int layer = 0; layer <= highest; ++layer)
            for (const std::shared_ptr<Node>& one : children()) {
                if (one->layer() != layer) continue;

                const ui::Area where{{0, 0}, given};
                const ui::Area part = ui::Area::crossed(seen, where);

                made.within(one->render(where, part - where.at), part.at - seen.at);
            }

        return made;
    }
};

// To samo pudełko, ale leżące na warstwie wyżej — czyli na rodzeństwie.
class Over : public Box {
public:
    Over(ui::Size wants, char32_t what) : Box(wants, what) {}

    [[nodiscard]] int layer() const override { return 1; }
};

class Sheet : public ui::Root {
public:
    [[nodiscard]] int takes() const override { return ui::plenty; }

    int hit = 0;

protected:
    bool react(ui::MouseEvent&) override {
        ++hit;

        return true;
    }
};

}

TRIAL("Układ: domyślne wants sumuje wysokości i bierze najszersze dziecko") {
    ui::Column one;

    one.children().add(std::make_shared<Box>(ui::Size{3, 2}, U'a'));
    one.children().add(std::make_shared<Box>(ui::Size{5, 1}, U'b'));

    const ui::Size wants = one.wants({80, 25});

    same(wants.wide, 5);
    same(wants.high, 3);
}

TRIAL("Układ: render zapisuje przydzielony prostokąt") {
    ui::Node one;

    (void)one.render({{2, 3}, {10, 4}});

    same(one.area().at.x, 2);
    same(one.area().at.y, 3);
    same(one.area().size.wide, 10);
    same(one.area().size.high, 4);
}

TRIAL("Układ: dzieci stają jedno pod drugim, na całą szerokość") {
    ui::Column one;

    const auto first = std::make_shared<Box>(ui::Size{3, 2}, U'a');
    const auto second = std::make_shared<Box>(ui::Size{5, 1}, U'b');

    one.children().add(first);
    one.children().add(second);

    (void)one.render({{0, 0}, {8, 6}});

    same(first->area().at.y, 0);
    same(first->area().size.high, 2);
    same(first->area().size.wide, 8);

    same(second->area().at.y, 2);
    same(second->area().size.high, 1);
}

TRIAL("Układ: treść dziecka ląduje na swojej wysokości") {
    ui::Column one;

    one.children().add(std::make_shared<Box>(ui::Size{4, 2}, U'a'));
    one.children().add(std::make_shared<Box>(ui::Size{4, 1}, U'b'));

    const View made = one.render({{0, 0}, {4, 4}});

    same(made.at({0, 0}).letter, U'a');
    same(made.at({3, 1}).letter, U'a');
    same(made.at({0, 2}).letter, U'b');
    same(made.at({0, 3}).letter, U' ');
}

TRIAL("Układ: dziecko poza dolną krawędzią dostaje zerową wysokość") {
    ui::Column one;

    const auto first = std::make_shared<Box>(ui::Size{4, 3}, U'a');
    const auto second = std::make_shared<Box>(ui::Size{4, 3}, U'b');

    one.children().add(first);
    one.children().add(second);

    (void)one.render({{0, 0}, {4, 3}});

    same(first->area().size.high, 3);
    same(second->area().size.high, 0);
    same(second->area().at.y, 3);
}

TRIAL("Układ: prostokąt dziecka jest względem rodzica, nie ekranu") {
    ui::Column one;

    const auto inner = std::make_shared<Box>(ui::Size{4, 1}, U'a');

    one.children().add(inner);

    (void)one.render({{5, 7}, {4, 2}});

    same(inner->area().at.x, 0);
    same(inner->area().at.y, 0);
}

TRIAL("Trafianie: klik ląduje w dziecku pod kursorem") {
    Sheet top;

    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Box>(ui::Size{6, 2}, U'a');
    const auto second = std::make_shared<Box>(ui::Size{6, 2}, U'b');

    column->children().add(first);
    column->children().add(second);
    top.children().add(column);

    (void)top.render({{0, 0}, {6, 4}});

    ui::MouseEvent what{{2, 3}, ui::Press::down, ui::Mouse::left};

    ought(top.deal(what));

    same(first->hit, 0);
    same(second->hit, 1);
    same(top.hit, 0);
}

TRIAL("Trafianie: klik obok dzieci zostaje w korzeniu") {
    Sheet top;

    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Box>(ui::Size{6, 1}, U'a');

    column->children().add(first);
    top.children().add(column);

    (void)top.render({{0, 0}, {6, 4}});

    ui::MouseEvent what{{3, 3}, ui::Press::down, ui::Mouse::left};

    ought(top.deal(what));

    same(first->hit, 0);
    same(top.hit, 1);
}

TRIAL("Trafianie: schodzi przez kilka pięter") {
    Sheet top;

    const auto middle = std::make_shared<ui::Node>();
    const auto deep = std::make_shared<Box>(ui::Size{6, 1}, U'a');

    middle->children().add(deep);
    top.children().add(middle);

    (void)top.render({{0, 0}, {6, 4}});

    ui::MouseEvent what{{1, 0}, ui::Press::down, ui::Mouse::left};

    ought(top.deal(what));

    same(deep->hit, 1);
}

TRIAL("Trafianie: z nakładających się rodzeństw wygrywa ostatnie") {
    Sheet top;

    const auto stack = std::make_shared<Stack>();
    const auto under = std::make_shared<Box>(ui::Size{4, 4}, U'a');
    const auto over = std::make_shared<Box>(ui::Size{4, 4}, U'b');

    stack->children().add(under);
    stack->children().add(over);
    top.children().add(stack);

    (void)top.render({{0, 0}, {4, 4}});

    ui::MouseEvent what{{1, 1}, ui::Press::down, ui::Mouse::left};

    ought(top.deal(what));

    same(under->hit, 0);
    same(over->hit, 1);
}

TRIAL("Trafianie: kółko celuje tak samo jak klik") {
    Sheet top;

    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Box>(ui::Size{6, 2}, U'a');
    const auto second = std::make_shared<Box>(ui::Size{6, 2}, U'b');

    column->children().add(first);
    column->children().add(second);
    top.children().add(column);

    (void)top.render({{0, 0}, {6, 4}});

    ui::WheelEvent what{{0, 2}, 1};

    ought(top.deal(what));

    same(second->hit, 1);
}

TRIAL("Warstwy: wyższa wygrywa z rodzeństwem przy trafianiu") {
    Sheet top;

    const auto stack = std::make_shared<Stack>();
    const auto over = std::make_shared<Over>(ui::Size{4, 4}, U'b');
    const auto under = std::make_shared<Box>(ui::Size{4, 4}, U'a');

    // Wyższe dopięte **pierwsze**, więc przy równych warstwach wygrałoby drugie.
    stack->children().add(over);
    stack->children().add(under);
    top.children().add(stack);

    (void)top.render({{0, 0}, {4, 4}});

    ui::MouseEvent what{{1, 1}, ui::Press::down, ui::Mouse::left};

    ought(top.deal(what));

    same(under->hit, 0);
    same(over->hit, 1);
}

TRIAL("Warstwy: wyższa kładzie się na rodzeństwie przy rysowaniu") {
    Stack stack;

    const auto over = std::make_shared<Over>(ui::Size{4, 1}, U'b');
    const auto under = std::make_shared<Box>(ui::Size{4, 1}, U'a');

    stack.children().add(over);
    stack.children().add(under);

    const View made = stack.render({{0, 0}, {4, 1}});

    same(made.at({0, 0}).letter, U'b');
}

TRIAL("Warstwy: wyższa nie wychodzi poza rodzica") {
    ui::Node top;

    const auto stack = std::make_shared<Stack>();
    const auto over = std::make_shared<Over>(ui::Size{8, 8}, U'b');

    stack->children().add(over);
    top.children().add(stack);

    const View made = top.render({{0, 0}, {3, 2}});

    same(made.size().wide, 3);
    same(made.size().high, 2);
    same(made.at({2, 1}).letter, U'b');
}

TRIAL("Trafianie: bez rozstawienia wszystko idzie do korzenia") {
    Sheet top;

    const auto first = std::make_shared<Box>(ui::Size{6, 2}, U'a');

    top.children().add(first);

    ui::MouseEvent what{{2, 1}, ui::Press::down, ui::Mouse::left};

    ought(top.deal(what));

    same(first->hit, 0);
    same(top.hit, 1);
}
