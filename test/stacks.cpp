#include "tal/testing.hpp"

#include <memory>
#include <optional>

#include "talcite/column.hpp"
#include "talcite/float.hpp"
#include "talcite/root.hpp"
#include "talcite/row.hpp"
#include "talcite/snapshot.hpp"
#include "talcite/stack.hpp"

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
    void paint(Canvas& onto) const override {
        const ui::Size given = onto.size();

        onto.fill({{0, 0}, given}, Style{});

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                onto.put({x, y}, what_, Style{});
    }

private:
    ui::Size wants_;
    char32_t what_;
};

// A stack that lays one child in a fixed place, smaller than the room, the way `Windows` lays a
// window in its seat: the container says where a child lies, through `placement`.
class Board : public ui::Stack {
public:
    // Lays `one` at `where` from now on; every other child takes the whole room.
    void pin(const ui::Node& one, ui::Area where) {
        pinned_ = &one;
        where_ = where;
    }

protected:
    // The pinned child in its place, the rest as the stack lays them.
    [[nodiscard]] ui::Area placement(const ui::Node& one, ui::Size room) const override {
        return &one == pinned_ ? where_ : ui::Stack::placement(one, room);
    }

private:
    const ui::Node* pinned_ = nullptr;
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

// A node in a stack takes the whole room. A veil takes that much to catch the mouse everywhere, and
// so does the application's content; what stands in one spot stands in a float.
TRIAL("Stack: a child takes the whole room") {
    ui::Stack stack;

    const auto one = std::make_shared<Box>(ui::Size{2, 1}, U'a');

    stack.children().add(one);

    (void)stack.render({{0, 0}, {6, 3}});

    SAME(one->area(), (ui::Area{{0, 0}, {6, 3}}));
}

// --- A place of its own --------------------------------------------------------------------------

// This is what `placement` is for. The child stands where its container lays it, and the rest of
// the room stays transparent: what lies below shows from under the overlay.
TRIAL("Stack: a child the container lays in one spot stands there") {
    Board stack;

    const auto under = std::make_shared<Box>(ui::Size{6, 3}, U'a');
    const auto over = std::make_shared<Box>(ui::Size{3, 1}, U'b');

    stack.pin(*over, {{2, 1}, {3, 1}});
    stack.children().add(under);
    stack.children().add(over);

    const Snapshot made = stack.render({{0, 0}, {6, 3}});

    SAME(over->area(), (ui::Area{{2, 1}, {3, 1}}));

    SAME(made.at({0, 0}).letter, U'a');
    SAME(made.at({2, 1}).letter, U'b');
    SAME(made.at({4, 1}).letter, U'b');
    SAME(made.at({5, 1}).letter, U'a');
    SAME(made.at({2, 2}).letter, U'a');
}

// The stack is transparent where no child lies; otherwise it would paint spaces over everything
// standing below it in its parent. That shows only when the stack is itself an overlay, so the trial
// puts something under it, giving `clear` something to paint over.
TRIAL("Stack: outside its children it lets through what lies below") {
    ui::Stack outer;

    const auto under = std::make_shared<Box>(ui::Size{6, 3}, U'a');
    const auto inner = std::make_shared<Board>();
    const auto over = std::make_shared<Box>(ui::Size{2, 1}, U'b');

    inner->pin(*over, {{2, 1}, {2, 1}});
    inner->children().add(over);

    outer.children().add(under);
    outer.children().add(inner);

    const Snapshot made = outer.render({{0, 0}, {6, 3}});

    SAME(made.at({2, 1}).letter, U'b');
    SAME(made.at({0, 0}).letter, U'a');
    SAME(made.at({5, 2}).letter, U'a');
}

// A child may overlap its siblings but may not go outside its parent, and the container enforces
// that, because here it is one place, while in the children it would be as many as there are
// classes.
TRIAL("Stack: a place sticking out of the room is clipped") {
    Board stack;

    const auto one = std::make_shared<Box>(ui::Size{10, 10}, U'b');

    stack.pin(*one, {{4, 2}, {10, 10}});
    stack.children().add(one);

    const Snapshot made = stack.render({{0, 0}, {6, 3}});

    SAME(one->area(), (ui::Area{{4, 2}, {2, 1}}));
    SAME(made.size(), (ui::Size{6, 3}));
}

TRIAL("Stack: a place outside the room gives an empty rectangle, not a negative one") {
    Board stack;

    const auto one = std::make_shared<Box>(ui::Size{4, 4}, U'b');

    stack.pin(*one, {{-3, 8}, {4, 4}});
    stack.children().add(one);

    (void)stack.render({{0, 0}, {6, 3}});

    OUGHT(one->area().size.wide >= 0);
    OUGHT(one->area().size.high >= 0);
    OUGHT(one->area().at.x >= 0);
    OUGHT(one->area().at.y <= 3);
}

// --- Hitting -------------------------------------------------------------------------------------

// The mouse reads the same order and the same rectangles the stack paints with: a click on the
// overlay hits the overlay, a click beside it hits what lies below.
TRIAL("Stack: a click hits the overlay only where it is") {
    Sheet top;

    const auto stack = std::make_shared<Board>();
    const auto under = std::make_shared<Box>(ui::Size{6, 3}, U'a');
    const auto over = std::make_shared<Box>(ui::Size{3, 1}, U'b');

    stack->pin(*over, {{2, 1}, {3, 1}});
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
    SAME(hits, 1);

    ui::ClickEvent outside{{0, 0}, ui::Mouse::left, {}};

    (void)top.dispatch(outside);
    SAME(hits, 1);
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

    SAME(right->area(), (ui::Area{{0, 0}, {6, 3}}));
    SAME(held->area().at.x, 4);

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
    SAME(below, 1);
    SAME(above, 0);

    ui::ClickEvent on{{5, 0}, ui::Mouse::left, {}};

    (void)top.dispatch(on);
    SAME(below, 1);
    SAME(above, 1);
    SAME(floated, 0);
}

// Icons in the top right corner of a window: two floats, one in the other, over the content. A
// click in the empty margin of either goes to the content, as with one float; only the icons
// themselves are hit.
TRIAL("Stack: through nested floats a click hits only what the innermost holds") {
    Sheet top;

    const auto stack = std::make_shared<ui::Stack>();
    const auto under = std::make_shared<Box>(ui::Size{6, 3}, U'a');
    const auto icons = std::make_shared<Box>(ui::Size{2, 1}, U'i');
    const auto upper = std::make_shared<ui::Top>();
    const auto right = std::make_shared<ui::Right>();

    right->children().add(icons);
    upper->children().add(right);
    stack->children().add(under);
    stack->children().add(upper);
    top.children().add(stack);

    const Snapshot made = top.render({{0, 0}, {6, 3}});

    SAME(icons->screen_area()->at, (ui::Spot{4, 0}));
    SAME(made.at({4, 0}).letter, U'i');
    SAME(made.at({0, 0}).letter, U'a');

    int below = 0;
    int hit = 0;

    const ui::Hook heard_below = under->on<ui::ClickEvent>([&below](ui::ClickEvent&) {
        ++below;

        return true;
    });

    const ui::Hook heard_icons = icons->on<ui::ClickEvent>([&hit](ui::ClickEvent&) {
        ++hit;

        return true;
    });

    for (const ui::Spot at : {ui::Spot{1, 0}, ui::Spot{3, 2}}) {
        ui::ClickEvent beside{at, ui::Mouse::left, {}};

        (void)top.dispatch(beside);
    }

    SAME(below, 2);
    SAME(hit, 0);

    ui::ClickEvent on{{5, 0}, ui::Mouse::left, {}};

    (void)top.dispatch(on);
    SAME(below, 2);
    SAME(hit, 1);
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
    SAME(second->area().at, (ui::Spot{0, 1}));

    const std::optional<ui::Area> where = second->screen_area();

    OUGHT(where.has_value());
    SAME(where->at, (ui::Spot{0, 2}));

    // Something above disappeared; the place has to see that without any notification.
    outer->children().remove(head.get());
    outer->children().add(std::make_shared<Box>(ui::Size{6, 2}, U'c'));

    (void)top.render({{0, 0}, {6, 5}});

    SAME(second->screen_area()->at, (ui::Spot{0, 1}));
}

TRIAL("screen_area: a node without a rectangle has no place") {
    const auto one = std::make_shared<Box>(ui::Size{6, 1}, U'a');

    OUGHT(!one->screen_area().has_value());
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

    SAME(third->area().at, (ui::Spot{2, 0}));
    SAME(third->screen_area()->at, (ui::Spot{5, 0}));
    SAME(third->local({6, 0}), (ui::Spot{1, 0}));
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

    SAME(third->area_in(*inner)->at, (ui::Spot{2, 0}));
    SAME(third->area_in(*row)->at, (ui::Spot{5, 0}));
    OUGHT(!third->area_in(*first).has_value());
    OUGHT(!first->area_in(*inner).has_value());
}
