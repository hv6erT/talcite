#include "talcite/inset.hpp"

#include <algorithm>

namespace talcite::ui {

// One child with room around it: `sides` cells on the left and on the right, `top` rows above. A
// window's content keeps off its frame so, as the windows of Midnight Commander do (a frame has no
// room of its own inside), and a message's text keeps a cell off the frame when it has no box of
// its own. An empty child takes no height, so a gap beside it in a column does not open either.
Inset::Inset(int sides, int top) : sides_(std::max(sides, 0)), top_(std::max(top, 0)) {}

// One child.
int Inset::capacity() const { return 1; }

// What the child wants in the room left inside, with the room around it added, within the room.
Size Inset::wants(Size room) const {
    if (children().empty()) return {std::min(2 * sides_, room.wide), std::min(top_, room.high)};

    const Size inner = children().front()->wants({std::max(room.wide - 2 * sides_, 0), std::max(room.high - top_, 0)});

    return {std::min(inner.wide + 2 * sides_, room.wide), std::min(inner.high + top_, room.high)};
}

// Draws the child inside the room around it; the room itself stays transparent, the rule described
// at `Node::paint`.
View Inset::paint(Size given, Area seen) {
    View made{seen.size};

    made.clear({{0, 0}, seen.size});

    if (children().empty()) return made;

    const Area where{{sides_, top_}, {std::max(given.wide - 2 * sides_, 0), std::max(given.high - top_, 0)}};
    const Area part = Area::crossed(seen, where);

    made.draw(children().front()->render(where, part - where.at), part.at - seen.at);

    return made;
}

}
