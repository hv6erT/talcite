#include "talcite/float.hpp"

#include <algorithm>

namespace talcite::ui {

// A float holds one child and stands it at the left, at the right or in the middle of the width it
// is given; the child gets its wanted width and the full height. Wanting is the child's, as for any
// node with one child: a float asks for no more than what it holds, so a form with a float in it
// does not stretch over the whole screen.
//
// Where the width comes from depends on the parent. A column, a frame or a window gives every child
// its whole width, so a float there places its child across the parent: a centered row of buttons
// under a form's content. A row gives every child only what it wants, so there a float would have
// nothing to place in; that is why `Row` widens a float by its share of the room the row does not
// need (the rule is described at `Row::paint`). The float itself knows nothing of rows: it places
// its child in whatever rectangle it gets.
//
// Three classes rather than one with a parameter, because the class is what a sketch of an
// interface shows (`<Right>` in `docs/ui`), and what the reader of a tree should see is the side.
Float::Float(Justify side) : side_(side) {}

// Draws the child at its wanted width (at most the given width), at the float's side of the given
// width, over the full height; transparent around it, the rule described at `Node::paint`.
View Float::paint(Size given, Area seen) {
    View made{seen.size};

    made.clear({{0, 0}, seen.size});

    if (children().empty()) return made;

    const int wide = std::clamp(children().front()->wants(given).wide, 0, std::max(given.wide, 0));
    const int spare = std::max(given.wide - wide, 0);
    const int x = side_ == Justify::left ? 0 : side_ == Justify::right ? spare : spare / 2;

    const Area where{{x, 0}, {wide, given.high}};
    const Area part = Area::crossed(seen, where);

    made.draw(children().front()->render(where, part - where.at), part.at - seen.at);

    return made;
}

// A float that stands its child at the left edge, with the room it is given to its right.
Left::Left() : Float(Justify::left) {}

// A float that stands its child at the right edge, with the room it is given to its left.
Right::Right() : Float(Justify::right) {}

// A float that stands its child in the middle, with the room it is given on both sides; an odd
// cell of room goes to the right side.
Center::Center() : Float(Justify::center) {}

}
