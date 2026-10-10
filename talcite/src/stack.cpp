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
// Where a child lies is said by the child itself, through `placement(room)`. By default it is the
// whole room: a veil takes that much (to catch the mouse everywhere) and so does the content.
// Something that has to stand in one spot over the content takes less, and computes where itself,
// usually from the `standing()` of what it stands under. That is all the positioning in the
// library: one question, in one container, instead of modes on every node and containers that would
// have to understand them.

// A stack takes any number of children.
int Stack::capacity() const { return plenty; }

// A stack wants all the room it is offered.
Size Stack::wants(Size room) const { return room; }

// Paints the children in order, each in its own placement, so later children cover earlier ones.
View Stack::paint(Size given, Area seen) {
    View made{seen.size};

    // Transparent wherever nothing is drawn, the rule described at `Node::paint`. A child that
    // takes less than the whole room does not paint over the rest, so lower layers show around it.
    made.clear({{0, 0}, seen.size});

    for (const std::shared_ptr<Node>& one : children()) {
        Area where = one->placement(given);

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
