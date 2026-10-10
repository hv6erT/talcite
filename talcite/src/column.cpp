#include "talcite/column.hpp"

#include <algorithm>

namespace talcite::ui {

Column::Column(int gap) : gap_(std::max(gap, 0)) {}

// Sets the gap between children, in rows; a negative value becomes zero.
void Column::gap(int given) { gap_ = std::max(given, 0); }

// A column lays out any number of children, one under another.
int Column::capacity() const { return plenty; }

// The children's heights add up and the width is that of the widest child, but the column asks
// for at most as much as it was given. It may not tell its parent that it needs a hundred rows,
// because the parent has nowhere to take them from; showing what does not fit is the job of
// `Scroll`.
//
// The gap goes only between children that take up space; the rule and its reason are described
// with `Row`.
Size Column::wants(Size room) const {
    Size made;

    for (const std::shared_ptr<Node>& one : children()) {
        const int apart = made.high > 0 ? gap_ : 0;
        const Size wants =
            one->wants({room.wide, std::max(room.high - made.high - apart, 0)});

        if (wants.high == 0) continue;

        made.wide = std::max(made.wide, wants.wide);
        made.high += apart + wants.high;
    }

    return {std::min(made.wide, room.wide), std::min(made.high, room.high)};
}

// Draws the children one under another. No child drops out of the loop, even when the room has
// run out: a node that cannot be seen gets a rectangle of zero height, so its area() tells the
// truth about this frame instead of remembering the previous one. Such a node opens no gap
// before itself.
View Column::paint(Size given, Area seen) {
    View made{seen.size};

    // Transparent wherever nothing is drawn; the rule is described with `Node::paint`.

    made.clear({{0, 0}, seen.size});

    int y = 0;

    for (const std::shared_ptr<Node>& one : children()) {
        const int apart = y > 0 ? gap_ : 0;
        const int left = std::max(given.high - y - apart, 0);
        const Size wants = one->wants({given.wide, left});
        const Size gets{given.wide, std::min(wants.high, left)};

        const int from = gets.high > 0 ? y + apart : y;

        const Area where{{0, from}, gets};
        const Area part = Area::crossed(seen, where);

        made.draw(one->render(where, part - where.at), part.at - seen.at);

        y = from + gets.high;
    }

    return made;
}

}
