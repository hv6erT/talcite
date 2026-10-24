#include "proving.hpp"

#include <memory>
#include <optional>

#include "talcite/column.hpp"
#include "talcite/float.hpp"
#include "talcite/root.hpp"
#include "talcite/row.hpp"
#include "talcite/stack.hpp"
#include "talcite/view.hpp"

using namespace talcite;

#include "doing.hpp"

namespace {

// A box that paints one character over its whole allotment, so that it is visible who lies on top
// and where it stands.
class Box : public ui::Node {
public:
    Box(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    // Wants the size it was built with, whatever the room.
    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

protected:
    // Fills the given size with the box's character.
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

// A child that says where it stands by itself: in one place, smaller than the room. That is the
// whole difference between an overlay in a corner and a veil over everything.
class Pinned : public Box {
public:
    Pinned(ui::Area where, char32_t what) : Box(where.size, what), where_(where) {}

    // The fixed place the child was built with.
    [[nodiscard]] ui::Area placement(ui::Size) const override { return where_; }

private:
    ui::Area where_;
};

// A root that takes any number of children, so that trials can hang a stack or a row on it.
class Sheet : public ui::Root {
public:
    // Takes any number of children.
    [[nodiscard]] int capacity() const override { return ui::plenty; }
};

}

// --- Default place -------------------------------------------------------------------------------

// A plain node in a stack takes the whole room. A veil takes that much to catch the mouse
// everywhere, and so does the application's content, so this is the default answer rather than a
// separate setting.
TRIAL("Stack: a child without a place of its own takes the whole room") {
    ui::Stack stack;

    const auto one = std::make_shared<Box>(ui::Size{2, 1}, U'a');

    stack.children().add(one);

    (void)stack.render({{0, 0}, {6, 3}});

    same(one->area(), (ui::Area{{0, 0}, {6, 3}}));
}

// --- A place of its own --------------------------------------------------------------------------

// This is what `placement` is for. The child stands where it said, and the rest of the room stays
// transparent: what lies below shows from under the overlay.
TRIAL("Stack: a child with a place of its own stands where it said") {
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

// The stack is transparent where no child lies; otherwise it would paint spaces over everything
// standing below it in its parent. That shows only when the stack is itself an overlay, so the trial
// puts something under it, giving `clear` something to paint over.
TRIAL("Stack: outside its children it lets through what lies below") {
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

// A child may overlap its siblings but may not go outside its parent, and the container enforces
// that, because here it is one place, while in the children it would be as many as there are
// classes.
TRIAL("Stack: a place sticking out of the room is clipped") {
    ui::Stack stack;

    const auto one = std::make_shared<Pinned>(ui::Area{{4, 2}, {10, 10}}, U'b');

    stack.children().add(one);

    const View made = stack.render({{0, 0}, {6, 3}});

    same(one->area(), (ui::Area{{4, 2}, {2, 1}}));
    same(made.size(), (ui::Size{6, 3}));
}

TRIAL("Stack: a place outside the room gives an empty rectangle, not a negative one") {
    ui::Stack stack;

    const auto one = std::make_shared<Pinned>(ui::Area{{-3, 8}, {4, 4}}, U'b');

    stack.children().add(one);

    (void)stack.render({{0, 0}, {6, 3}});

    ought(one->area().size.wide >= 0);
    ought(one->area().size.high >= 0);
    ought(one->area().at.x >= 0);
    ought(one->area().at.y <= 3);
}

// --- Hitting -------------------------------------------------------------------------------------

// The mouse reads the same order and the same rectangles the stack paints with: a click on the
// overlay hits the overlay, a click beside it hits what lies below.
TRIAL("Stack: a click hits the overlay only where it is") {
    Sheet top;

    const auto stack = std::make_shared<ui::Stack>();
    const auto under = std::make_shared<Box>(ui::Size{6, 3}, U'a');
    const auto over = std::make_shared<Pinned>(ui::Area{{2, 1}, {3, 1}}, U'b');

    stack->children().add(under);
    stack->children().add(over);
    top.children().add(stack);

    (void)top.render({{0, 0}, {6, 3}});

    int hits = 0;

    const ui::Hook heard_click = over->on<ui::ClickEvent>([&hits](ui::ClickEvent&) {
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

// A float occupies only what it holds: over the whole room of a stack, a click beside its child
// goes to the layer below, as if the float were not there, and a click on the child hits the child.
// The float itself is never the target.
TRIAL("Stack: a click beside what a float holds goes to the layer below") {
    Sheet top;

    const auto stack = std::make_shared<ui::Stack>();
    const auto under = std::make_shared<Box>(ui::Size{6, 3}, U'a');
    const auto held = std::make_shared<Box>(ui::Size{2, 1}, U'b');
    const auto right = std::make_shared<ui::Right>();

    right->children().add(held);
    stack->children().add(under);
    stack->children().add(right);
    top.children().add(stack);

    (void)top.render({{0, 0}, {6, 3}});

    same(right->area(), (ui::Area{{0, 0}, {6, 3}}));
    same(held->area().at.x, 4);

    int below = 0;
    int above = 0;
    int floated = 0;

    const ui::Hook heard_below = under->on<ui::ClickEvent>([&below](ui::ClickEvent&) {
        ++below;

        return true;
    });

    const ui::Hook heard_above = held->on<ui::ClickEvent>([&above](ui::ClickEvent&) {
        ++above;

        return true;
    });

    const ui::Hook heard_float = right->on<ui::ClickEvent>([&floated](ui::ClickEvent&) {
        ++floated;

        return true;
    });

    ui::ClickEvent beside{{1, 0}, ui::Mouse::left, {}};

    (void)top.dispatch(beside);
    same(below, 1);
    same(above, 0);

    ui::ClickEvent on{{5, 0}, ui::Mouse::left, {}};

    (void)top.dispatch(on);
    same(below, 1);
    same(above, 1);
    same(floated, 0);
}

// --- screen_area, area_in -----------------------------------------------------------------------

// The place on the screen is assembled from rectangles up the tree and is computed on every
// question: a coordinate remembered when something was rolled out would go stale at the first move
// of anything above.
//
// The node stands two levels down with an offset at each one. With a column at (0, 0) the node's
// own rectangle would equal its place on the screen, and the trial could not tell whether the
// levels are composed at all; a question has to be asked where the mechanism leaves a trace.
TRIAL("screen_area: assembles the place on the screen through the levels and sees the offset") {
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

    // The own rectangle says "second row of the inner column"; the place on the screen says "third".
    same(second->area().at, (ui::Spot{0, 1}));

    const std::optional<ui::Area> where = second->screen_area();

    ought(where.has_value());
    same(where->at, (ui::Spot{0, 2}));

    // Something above disappeared; the place has to see that without any notification.
    outer->children().remove(head.get());
    outer->children().add(std::make_shared<Box>(ui::Size{6, 2}, U'c'));

    (void)top.render({{0, 0}, {6, 5}});

    same(second->screen_area()->at, (ui::Spot{0, 1}));
}

TRIAL("screen_area: a node without a rectangle has no place") {
    const auto one = std::make_shared<Box>(ui::Size{6, 1}, U'a');

    ought(!one->screen_area().has_value());
}

// The same in the other axis: the trial above stands entirely at the left edge, so it could not
// see whether the ancestors' horizontal offsets add up at all.
TRIAL("screen_area: adds up the ancestors' offsets horizontally too") {
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

// A rectangle in an ancestor's coordinates: without the ancestor's own offset, and for a node
// outside it, nothing. The scroller asks this about the focused node, so an answer of "somewhere"
// for a node beside it would scroll the content to a place that is not in it.
TRIAL("area_in: counts from the ancestor and does not answer for nodes outside it") {
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
