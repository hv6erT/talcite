#include "tal/testing.hpp"

#include <algorithm>
#include <memory>

#include "talcite/column.hpp"
#include "talcite/root.hpp"
#include "talcite/stack.hpp"
#include "talcite/row.hpp"
#include "talcite/snapshot.hpp"

using namespace talcite;

namespace {

// A node that wants a fixed size and paints the area it is given with one letter. That is enough to
// check both the layout arithmetic and where the content lands.
class Box : public ui::Node {
public:
    Box(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    // The fixed size, whatever space is offered.
    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

    int hit = 0;

protected:
    // Fills the whole given size with the letter, shifted to the visible part.
    void paint(Canvas& onto) const override {
        const ui::Size given = onto.size();

        onto.fill({{0, 0}, given}, Style{});

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                onto.put({x, y}, what_, Style{});
    }

    // Counts a mouse event that reached this box.
    bool handle(ui::MouseEvent&) override {
        ++hit;

        return true;
    }

    // Counts a wheel event that reached this box.
    bool handle(ui::WheelEvent&) override {
        ++hit;

        return true;
    }

private:
    ui::Size wants_;
    char32_t what_ = U'?';
};

// A root that counts the mouse events that reach it, so a trial can see that nothing below took
// them.
class Sheet : public ui::Root {
public:
    // Room for any number of children.
    [[nodiscard]] int capacity() const override { return ui::plenty; }

    int hit = 0;

protected:
    // Counts a mouse event that reached the root itself.
    bool handle(ui::MouseEvent&) override {
        ++hit;

        return true;
    }
};

}

TRIAL("Layout: the default wants adds up the heights and takes the widest child") {
    ui::Column one;

    one.children().add(std::make_shared<Box>(ui::Size{3, 2}, U'a'));
    one.children().add(std::make_shared<Box>(ui::Size{5, 1}, U'b'));

    const ui::Size wants = one.wants({80, 25});

    SAME(wants.wide, 5);
    SAME(wants.high, 3);
}

TRIAL("Layout: render records the area it was given") {
    ui::Node one;

    (void)one.render({{2, 3}, {10, 4}});

    SAME(one.area().at.x, 2);
    SAME(one.area().at.y, 3);
    SAME(one.area().size.wide, 10);
    SAME(one.area().size.high, 4);
}

TRIAL("Layout: children stand one below another, across the full width") {
    ui::Column one;

    const auto first = std::make_shared<Box>(ui::Size{3, 2}, U'a');
    const auto second = std::make_shared<Box>(ui::Size{5, 1}, U'b');

    one.children().add(first);
    one.children().add(second);

    (void)one.render({{0, 0}, {8, 6}});

    SAME(first->area().at.y, 0);
    SAME(first->area().size.high, 2);
    SAME(first->area().size.wide, 8);

    SAME(second->area().at.y, 2);
    SAME(second->area().size.high, 1);
}

TRIAL("Layout: a child's content lands at its own height") {
    ui::Column one;

    one.children().add(std::make_shared<Box>(ui::Size{4, 2}, U'a'));
    one.children().add(std::make_shared<Box>(ui::Size{4, 1}, U'b'));

    const Snapshot made = one.render({{0, 0}, {4, 4}});

    SAME(made.at({0, 0}).letter, U'a');
    SAME(made.at({3, 1}).letter, U'a');
    SAME(made.at({0, 2}).letter, U'b');
    SAME(made.at({0, 3}).letter, U' ');
}

TRIAL("Layout: a child below the bottom edge gets zero height") {
    ui::Column one;

    const auto first = std::make_shared<Box>(ui::Size{4, 3}, U'a');
    const auto second = std::make_shared<Box>(ui::Size{4, 3}, U'b');

    one.children().add(first);
    one.children().add(second);

    (void)one.render({{0, 0}, {4, 3}});

    SAME(first->area().size.high, 3);
    SAME(second->area().size.high, 0);
    SAME(second->area().at.y, 3);
}

TRIAL("Layout: a child's area is relative to the parent, not the screen") {
    ui::Column one;

    const auto inner = std::make_shared<Box>(ui::Size{4, 1}, U'a');

    one.children().add(inner);

    (void)one.render({{5, 7}, {4, 2}});

    SAME(inner->area().at.x, 0);
    SAME(inner->area().at.y, 0);
}

TRIAL("Hit testing: a click lands in the child under the cursor") {
    Sheet top;

    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Box>(ui::Size{6, 2}, U'a');
    const auto second = std::make_shared<Box>(ui::Size{6, 2}, U'b');

    column->children().add(first);
    column->children().add(second);
    top.children().add(column);

    (void)top.render({{0, 0}, {6, 4}});

    ui::MouseEvent what{{2, 3}, ui::Press::down, ui::Mouse::left};

    OUGHT(top.dispatch(what));

    SAME(first->hit, 0);
    SAME(second->hit, 1);
    SAME(top.hit, 0);
}

TRIAL("Hit testing: a click beside the children stays in the root") {
    Sheet top;

    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Box>(ui::Size{6, 1}, U'a');

    column->children().add(first);
    top.children().add(column);

    (void)top.render({{0, 0}, {6, 4}});

    ui::MouseEvent what{{3, 3}, ui::Press::down, ui::Mouse::left};

    OUGHT(top.dispatch(what));

    SAME(first->hit, 0);
    SAME(top.hit, 1);
}

TRIAL("Hit testing: it descends through several levels") {
    Sheet top;

    const auto middle = std::make_shared<ui::Node>();
    const auto deep = std::make_shared<Box>(ui::Size{6, 1}, U'a');

    middle->children().add(deep);
    top.children().add(middle);

    (void)top.render({{0, 0}, {6, 4}});

    ui::MouseEvent what{{1, 0}, ui::Press::down, ui::Mouse::left};

    OUGHT(top.dispatch(what));

    SAME(deep->hit, 1);
}

TRIAL("Hit testing: of overlapping siblings, the last one wins") {
    Sheet top;

    const auto stack = std::make_shared<ui::Stack>();
    const auto under = std::make_shared<Box>(ui::Size{4, 4}, U'a');
    const auto over = std::make_shared<Box>(ui::Size{4, 4}, U'b');

    stack->children().add(under);
    stack->children().add(over);
    top.children().add(stack);

    (void)top.render({{0, 0}, {4, 4}});

    ui::MouseEvent what{{1, 1}, ui::Press::down, ui::Mouse::left};

    OUGHT(top.dispatch(what));

    SAME(under->hit, 0);
    SAME(over->hit, 1);
}

TRIAL("Hit testing: the wheel aims the same way as a click") {
    Sheet top;

    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Box>(ui::Size{6, 2}, U'a');
    const auto second = std::make_shared<Box>(ui::Size{6, 2}, U'b');

    column->children().add(first);
    column->children().add(second);
    top.children().add(column);

    (void)top.render({{0, 0}, {6, 4}});

    ui::WheelEvent what{{0, 2}, 1};

    OUGHT(top.dispatch(what));

    SAME(second->hit, 1);
}

// On top is the child attached last, and that is the only source of this truth. Painting goes in
// order, so a later child paints over an earlier one; hit testing takes the last match, so the
// mouse aims at what is visible. Both mechanisms read the same order and cannot drift apart.
TRIAL("Stacking: the child attached last wins in hit testing") {
    Sheet top;

    const auto stack = std::make_shared<ui::Stack>();
    const auto under = std::make_shared<Box>(ui::Size{4, 4}, U'a');
    const auto over = std::make_shared<Box>(ui::Size{4, 4}, U'b');

    stack->children().add(under);
    stack->children().add(over);
    top.children().add(stack);

    (void)top.render({{0, 0}, {4, 4}});

    ui::MouseEvent what{{1, 1}, ui::Press::down, ui::Mouse::left};

    OUGHT(top.dispatch(what));

    SAME(under->hit, 0);
    SAME(over->hit, 1);
}

TRIAL("Stacking: the child attached last is painted over its siblings") {
    ui::Stack stack;

    const auto under = std::make_shared<Box>(ui::Size{4, 1}, U'a');
    const auto over = std::make_shared<Box>(ui::Size{4, 1}, U'b');

    stack.children().add(under);
    stack.children().add(over);

    const Snapshot made = stack.render({{0, 0}, {4, 1}});

    SAME(made.at({0, 0}).letter, U'b');
}

TRIAL("Stacking: an overlay does not extend beyond its parent") {
    ui::Node top;

    const auto stack = std::make_shared<ui::Stack>();
    const auto over = std::make_shared<Box>(ui::Size{8, 8}, U'b');

    stack->children().add(over);
    top.children().add(stack);

    const Snapshot made = top.render({{0, 0}, {3, 2}});

    SAME(made.size().wide, 3);
    SAME(made.size().high, 2);
    SAME(made.at({2, 1}).letter, U'b');
}

TRIAL("Hit testing: without a layout pass everything goes to the root") {
    Sheet top;

    const auto first = std::make_shared<Box>(ui::Size{6, 2}, U'a');

    top.children().add(first);

    ui::MouseEvent what{{2, 1}, ui::Press::down, ui::Mouse::left};

    OUGHT(top.dispatch(what));

    SAME(first->hit, 0);
    SAME(top.hit, 1);
}
