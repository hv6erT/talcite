#include "tal/testing.hpp"

#include <algorithm>
#include <memory>
#include <string>

#include "talcite/box.hpp"
#include "talcite/dialog.hpp"
#include "talcite/root.hpp"
#include "talcite/modals.hpp"

using namespace talcite;

#include "doing.hpp"

namespace {

// A child with a known wish, smaller or larger than the room, so that a trial can show that
// `Box` does not ask it. Without such a child, "does not ask" and "asks, but the result is the
// same" would be indistinguishable.
class Wanting : public ui::Node {
public:
    explicit Wanting(ui::Size given) : wants_(given) {}

    // Always the size it was made with, whatever the room.
    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

private:
    ui::Size wants_;
};

}

// --- Size ---------------------------------------------------------------------------------------

// A size that is not set means the maximum available, not zero and not the child's wish.
// `plenty` is the same infinity that `List` uses, not a second one beside it.
TRIAL("Box: without a size it asks for the whole room") {
    const auto one = std::make_shared<ui::Box>();

    SAME(one->wants({20, 10}).wide, 20);
    SAME(one->wants({20, 10}).high, 10);
}

TRIAL("Box: the given size is what it asks for") {
    const auto one = std::make_shared<ui::Box>(ui::Size{12, 4});

    SAME(one->wants({20, 10}).wide, 12);
    SAME(one->wants({20, 10}).high, 4);
}

// Clamped to the room, because a wish larger than the space is not a wish but a lie to the parent,
// which then divides the space.
TRIAL("Box: a size larger than the room comes down to the room") {
    const auto one = std::make_shared<ui::Box>(ui::Size{200, 50});

    SAME(one->wants({20, 10}).wide, 20);
    SAME(one->wants({20, 10}).high, 10);
}

// The axes are independent: `plenty` on one means "the whole room on this axis", and the other
// keeps its limit. This is how a bar, a column and every full-height window are sized.
TRIAL("Box: plenty on one axis does not touch the other") {
    const auto one = std::make_shared<ui::Box>(ui::Size{12, ui::plenty});

    SAME(one->wants({20, 10}).wide, 12);
    SAME(one->wants({20, 10}).high, 10);
}

// A room can be negative: the parent subtracts edges and margins from what it has, and on a
// cramped screen this goes below zero. A negative wish must stop here, not travel up into the
// layout's arithmetic.
TRIAL("Box: a negative room gives zero, not a negative number") {
    const auto one = std::make_shared<ui::Box>(ui::Size{12, 4});

    SAME(one->wants({-3, -1}).wide, 0);
    SAME(one->wants({-3, -1}).high, 0);
}

// --- Child --------------------------------------------------------------------------------------

// `Box` does not ask its child at all, and that is its point, because otherwise it would be a
// `Frame` without a frame, and a given size would stop being given. The trial shows this from both
// sides: with a child greedier and with a child more modest than the given number.
TRIAL("Box: the child's wish changes nothing") {
    const auto small = std::make_shared<ui::Box>(ui::Size{12, 4});
    const auto big = std::make_shared<ui::Box>(ui::Size{12, 4});

    small->children().add(std::make_shared<Wanting>(ui::Size{2, 1}));
    big->children().add(std::make_shared<Wanting>(ui::Size{40, 40}));

    SAME(small->wants({20, 10}).wide, 12);
    SAME(small->wants({20, 10}).high, 4);

    SAME(big->wants({20, 10}).wide, 12);
    SAME(big->wants({20, 10}).high, 4);
}

// There is no second mode, because the absence of a `Box` provides it: a plain `Node` follows the
// child's wish. The trial stands here and not with `Node`, because this is the half of the
// decision that spares `Box` an enumeration of modes.
TRIAL("Box: a plain Node still follows the child's wish") {
    const auto one = std::make_shared<ui::Node>();

    one->children().add(std::make_shared<Wanting>(ui::Size{2, 1}));

    SAME(one->wants({20, 10}).wide, 2);
    SAME(one->wants({20, 10}).high, 1);
}

// --- Wish as a function -------------------------------------------------------------------------

// This is why a wish is a function and not a number. "At most eighty columns, but leave eight on
// each side" is an ordinary way to size a window, and a number cannot express it, because the
// second half depends on how much space there is at all. The computation from help.c of Midnight
// Commander, carried over directly.
TRIAL("Box: a wish computed from the room") {
    const auto one = std::make_shared<ui::Box>(
        ui::Box::Wish{[](ui::Size room) { return ui::Size{std::min(80, room.wide - 16), 20}; }});

    // A wide room: the upper limit holds.
    SAME(one->wants({200, 40}).wide, 80);

    // A cramped one: the gap from the edge stays, which a constant number could not keep.
    SAME(one->wants({60, 40}).wide, 44);
    SAME(one->wants({30, 40}).wide, 14);
}

// A constant is a special case of a function, which is why both constructors fill one field and
// `wants` has nothing to branch on. The trial guards that the two forms are interchangeable.
TRIAL("Box: a constant and a function returning the same constant are indistinguishable") {
    const auto flat = std::make_shared<ui::Box>(ui::Size{12, 4});
    const auto deep = std::make_shared<ui::Box>(
        ui::Box::Wish{[](ui::Size) { return ui::Size{12, 4}; }});

    for (const ui::Size room : {ui::Size{20, 10}, ui::Size{6, 2}, ui::Size{-3, -1}}) {
        SAME(flat->wants(room).wide, deep->wants(room).wide);
        SAME(flat->wants(room).high, deep->wants(room).high);
    }
}

// The clamping stays in `Box`, not in the function, because otherwise everyone who writes one
// would have to remember it, and forgetting would hand the parent a wish larger than the space.
TRIAL("Box: a greedy function still does not go beyond the room") {
    const auto one = std::make_shared<ui::Box>(
        ui::Box::Wish{[](ui::Size) { return ui::Size{1000, 1000}; }});

    SAME(one->wants({20, 10}).wide, 20);
    SAME(one->wants({20, 10}).high, 10);
}

// An empty function would throw bad_function_call at the first frame, from inside the layout and
// at a place that tells nothing. It is rejected where it is made.
TRIAL("Box: an empty wish is not accepted") {
    THREW((void)std::make_shared<ui::Box>(ui::Box::Wish{}));
}

// --- In the tree --------------------------------------------------------------------------------

// This is what it is for. A modal computes its box from the wish of its only child and centers
// it, so greedy content (and every page, table and panel is greedy) blows the window up to the
// whole screen. The trials show both sides of the same thing: without a `Box` the window takes
// everything, with one it stays at the given size and the modal has something to center.
TRIAL("Box: in a modal it holds greedy content at the given size") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Dialog>();
    const auto box = std::make_shared<ui::Box>(ui::Size{10, 4});

    box->children().add(std::make_shared<Wanting>(ui::Size{ui::plenty, ui::plenty}));
    one->children().add(box);

    top.children().add(window);
    window->open(one);

    (void)top.render({{0, 0}, {30, 12}});

    // The room for the shadow (two columns and a row) is subtracted before centering, so the box
    // is centered in 28 by 11: (28−10)/2 = 9 and (11−4)/2 = 3.
    SAME(one->children().front()->area(), (ui::Area{{9, 3}, {10, 4}}));
}

TRIAL("Box: without it the same content blows the window up to the whole screen") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Dialog>();

    one->children().add(std::make_shared<Wanting>(ui::Size{ui::plenty, ui::plenty}));

    top.children().add(window);
    window->open(one);

    (void)top.render({{0, 0}, {30, 12}});

    SAME(one->children().front()->area(), (ui::Area{{0, 0}, {28, 11}}));
}
