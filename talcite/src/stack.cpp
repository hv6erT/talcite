#include "talcite/stack.hpp"

#include <algorithm>

namespace talcite::ui {

// The only container whose children lie on top of each other. `Row` and `Column` place them side by
// side, `Frame` wraps; everything that has to lie on something (a veil over the content, an opened
// list over the panels) lies here.
//
// A layer is the order of the children and nothing more. The last one attached is on top: this is
// how the container paints and how the mouse hits in `Root::under`, from the same list. There is no
// layer number, because it would be a second source of the same truth.
//
// Where a child lies is said by the container, not by the child: a stack gives every child the
// whole room. A veil takes that much (to catch the mouse everywhere) and so does the content.
// Something that stands in one spot over the content says so with a float around it
// (`Top{Right{icons}}`): the float takes the room, places its child in it and lets the mouse
// through its empty margins (`Root::under`). The two containers whose children stand where their
// own state says override `placement`: `Windows` puts a window in its seat, which the user moves,
// and `Menubar` puts a menu under its title or a submenu's list beside its item, which only the bar
// knows. Earlier the child answered the question itself, through a virtual on every node for the
// use of this one container, so the knowledge of where a thing lies sat in the thing laid.

// A stack takes any number of children.
int Stack::capacity() const { return plenty; }

// A stack wants all the room it is offered.
Size Stack::wants(Size room) const { return room; }

// Where the stack lays `one`: the whole room. A container built on the stack whose children stand
// in a place of their own says where, in the stack's coordinates.
Area Stack::placement(const Node&, Size room) const { return {{0, 0}, room}; }

// Paints the children in order, each in its placement, so later children cover earlier ones.
View Stack::paint(Size given, Area seen) {
    View made{seen.size};

    // Transparent wherever nothing is drawn, the rule described at `Node::paint`. A child that
    // takes less than the whole room does not paint over the rest, so lower layers show around it.
    made.clear({{0, 0}, seen.size});

    for (const std::shared_ptr<Node>& one : children()) {
        Area where = placement(*one, given);

        // A child may cover its siblings but may not leave its parent. The container guards that
        // boundary, not every child on its own, because here it is one place, and there it would be
        // as many places as there are classes.
        where.at.x = std::clamp(where.at.x, 0, std::max(given.wide, 0));
        where.at.y = std::clamp(where.at.y, 0, std::max(given.high, 0));
        where.size.wide = std::clamp(where.size.wide, 0, given.wide - where.at.x);
        where.size.high = std::clamp(where.size.high, 0, given.high - where.at.y);

        const Area part = Area::crossed(seen, where);

        made.draw(one->render(where, part - where.at), part.at - seen.at);
    }

    return made;
}

}
