#include "talcite/footed.hpp"

#include <algorithm>
#include <cstddef>

namespace talcite::ui {

// A column whose room is counted from the bottom: the first child is the content, the others stand
// under it and get their height first, from the last one up; the content gets what is left. That is
// a window's content over its buttons (`Form`), and a panel's table over its rule and status line:
// whatever stands at the bottom keeps its place when the room is short, and the content gives way.
//
// Asked for its size, the content counts with what it wants in the room the others leave; drawn, it
// gets all of that room, so a table that shows fewer rows than fit still has the status line at the
// bottom edge, not right under its last row. Asking the others first, both when sizing and when
// painting, gives the content the same room in both: content meant to fill the height (a scrolled
// page in a box asking "as much as I want, but leave room for what is below me") would otherwise
// get two different rooms, give two different answers, and leave empty rows below it.
//
// The gap stands only between two children that both take some height, the rule of `Column`: a
// form without content or without anything in its footer has no empty row to separate them.
Footed::Footed(int gap) : gap_(std::max(gap, 0)) {}

// Sets the gap between the parts, in rows; a negative gap counts as zero.
void Footed::gap(int given) {
    gap_ = std::max(given, 0);

    changed();
}

// The content and any number of parts under it.
int Footed::capacity() const { return plenty; }

// The heights and widths of the parts under the content (index 0 stays for the content), the gaps
// above each child and the height left for the content, in the room `room`. Whether a child
// occupies something, and so whether a gap stands above it, is asked with the whole room, before
// any child is cut down by another; a gap stands above a child that occupies something when a child
// before it occupies something too.
Footed::Laid Footed::laid(Size room) const {
    const std::size_t many = children().many();

    Laid made{std::vector<int>(many, 0), std::vector<int>(many, 0), std::vector<int>(many, 0), std::max(room.high, 0)};

    std::vector<bool> takes(many, false);

    for (std::size_t at = 0; at < many; ++at) takes[at] = children()[at]->wants(room).high > 0;

    for (std::size_t at = many; at-- > 1;) {
        if (!takes[at]) continue;

        const bool above = std::any_of(takes.begin(), takes.begin() + static_cast<std::ptrdiff_t>(at), [](bool one) { return one; });

        made.aparts[at] = above ? std::min(gap_, made.left) : 0;

        const int room_left = made.left - made.aparts[at];
        const Size wants = children()[at]->wants({room.wide, room_left});
        const int high = std::clamp(wants.high, 0, room_left);

        // A part that gets no row opens no gap either.
        if (high == 0) made.aparts[at] = 0;

        made.highs[at] = high;
        made.wides[at] = wants.wide;
        made.left -= made.aparts[at] + high;
    }

    return made;
}

// The content's wish in what the parts under it leave, with the parts and gaps; as wide as the
// widest child, within the room.
Size Footed::wants(Size room) const {
    if (children().empty()) return {};

    const Laid parts = laid(room);

    Size made = children().front()->wants({room.wide, parts.left});

    for (std::size_t at = 1; at < children().many(); ++at) {
        made.wide = std::max(made.wide, parts.wides[at]);
        made.high += parts.aparts[at] + parts.highs[at];
    }

    return {std::min(made.wide, room.wide), std::min(made.high, room.high)};
}

// Lays the content out at the top over all the room the parts leave, and the parts under it in
// order, each with the full width.
void Footed::layout(Size given) {
    if (children().empty()) return;

    const Laid parts = laid(given);

    int y = 0;

    for (std::size_t at = 0; at < children().many(); ++at) {
        const int high = at == 0 ? parts.left : parts.highs[at];

        y += parts.aparts[at];

        const Area where{{0, y}, {given.wide, high}};
        children()[at]->arrange(where);

        y += high;
    }
}

// Paints the content and the parts.
void Footed::paint(Canvas& onto) const {
    for (const std::shared_ptr<Node>& one : children()) one->render(onto);
}

}
