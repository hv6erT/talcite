#include "talcite/float.hpp"

#include <algorithm>

namespace talcite::ui {

// A float holds one child and stands it at the start, at the end or in the middle of its rectangle
// along one axis: `Left`, `Right` and `Center` across the width, `Top`, `Bottom` and `Middle` down
// the height. Along its axis the child gets what it wants; across it, the full extent. Wanting is
// the child's, as for any node with one child: a float asks for no more than what it holds, so a
// form with a float in it does not stretch over the whole screen.
//
// Where the room comes from depends on the parent. A container that gives every child the whole
// extent of the float's axis (a column the width, a row the height, a stack and a frame both)
// leaves the float the room to place its child in: a centered row of buttons under a form's
// content, icons in the top right corner of a window (`Top{Right{icons}}` in a stack). A container
// that lays its children out along that axis gives each only what it wants, so there a float would
// have nothing to place in; that is why `Row` widens a horizontal float and `Column` lengthens a
// vertical one by its share of the room they do not need (the rule is described at `Row::layout`).
// The float itself knows nothing of rows and columns: it places its child in whatever rectangle it
// gets.
//
// Six classes rather than one with parameters, because the class is what a sketch of an interface
// shows (`<Right>` in `docs/ui`), and what the reader of a tree should see is the side.
Float::Float(Axis axis, Align align) : axis_(axis), align_(align) {}

// Lays the child out at its wanted extent along the float's axis (at most the given one), at the
// float's side, over the full extent across it.
void Float::layout(Size given) {
    if (children().empty()) return;

    const bool across = axis_ == Axis::horizontal;
    const Size wants = children().front()->wants(given);
    const int room = std::max(across ? given.wide : given.high, 0);
    const int length = std::clamp(across ? wants.wide : wants.high, 0, room);
    const int spare = room - length;
    const int from = align_ == Align::start ? 0 : align_ == Align::end ? spare : spare / 2;

    const Area where = across ? Area{{from, 0}, {length, given.high}} : Area{{0, from}, {given.wide, length}};
    children().front()->arrange(where);
}

// Paints the child; around it the float stays transparent.
void Float::paint(Canvas& onto) const {
    for (const std::shared_ptr<Node>& one : children()) one->render(onto);
}

// A float that stands its child at the left edge, with the room it is given to its right.
Left::Left() : Float(Axis::horizontal, Align::start) {}

// A float that stands its child at the right edge, with the room it is given to its left.
Right::Right() : Float(Axis::horizontal, Align::end) {}

// A float that stands its child in the middle of the width, with the room it is given on both
// sides; an odd cell of room goes to the right side.
Center::Center() : Float(Axis::horizontal, Align::middle) {}

// A float that stands its child at the top edge, with the room it is given under it.
Top::Top() : Float(Axis::vertical, Align::start) {}

// A float that stands its child at the bottom edge, with the room it is given above it.
Bottom::Bottom() : Float(Axis::vertical, Align::end) {}

// A float that stands its child in the middle of the height, with the room it is given above and
// under it; an odd row of room goes under it.
Middle::Middle() : Float(Axis::vertical, Align::middle) {}

}
