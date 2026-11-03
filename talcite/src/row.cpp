#include "talcite/row.hpp"

#include <algorithm>
#include <cstddef>

#include "talcite/float.hpp"

namespace talcite::ui {

Row::Row(int gap) : gap_(std::max(gap, 0)) {}

// Sets the gap between children, in columns; a negative gap counts as zero.
void Row::gap(int given) {
    gap_ = std::max(given, 0);

    changed();
}

// A row takes any number of children.
int Row::capacity() const { return plenty; }

// Where each child stands when every one gets the width it wants, left to right, in one line; the
// height is the child's wish. One answer for both questions a row hears: `wants` measures it and
// `paint` widens it, so the size a row reports and the places it draws in cannot drift apart.
//
// A gap is due only between children that occupy something. A child of zero width (an empty label,
// a disabled button, a node without content) opens no gap, because it would leave a hole where
// nothing is visible. That is why we remember not "which child in turn this is" but "whether
// something already stands here". This is a matter of layout, not of content: otherwise every
// window would have to separate its buttons with empty nodes and fix them at every removal.
//
// Nobody drops out, even when the room has run out: a node that is not visible gets a rectangle of
// zero width, so that its area() tells the truth about this frame. But it opens no gap before
// itself.
std::vector<Area> Row::placed(Size room) const {
    std::vector<Area> made;

    made.reserve(children().many());

    int x = 0;

    for (const std::shared_ptr<Node>& one : children()) {
        const int apart = x > 0 ? gap_ : 0;
        const int left = std::max(room.wide - x - apart, 0);
        const Size wants = one->wants({left, room.high});
        const int wide = std::clamp(wants.wide, 0, left);
        const int from = wide > 0 ? x + apart : x;

        made.push_back({{from, 0}, {wide, wide > 0 ? std::clamp(wants.high, 0, std::max(room.high, 0)) : 0}});

        x = from + wide;
    }

    return made;
}

// The size the children want side by side, with gaps, clamped to `room`. Floats count with the
// width of what they hold and nothing more: the room they stand in is what the row has to spare,
// and a row that asked for it would take the whole width of every window it stands in.
Size Row::wants(Size room) const {
    Size made;

    for (const Area& one : placed(room)) {
        made.wide = std::max(made.wide, one.at.x + one.size.wide);
        made.high = std::max(made.high, one.size.high);
    }

    return {std::min(made.wide, room.wide), std::min(made.high, room.high)};
}

// Lays the children out left to right, each over the full height.
//
// The width the children do not need goes to the horizontal floats among them (`Left`, `Right`,
// `Center`; a vertical float is an ordinary child here, placing its own child in the full height),
// by the rule of automatic margins, as `margin: auto` in a flexible box: a float has an empty
// margin on its free side (a left float on its right, a right float on its left, a centered one on
// both), and the spare width is split equally between all those margins. The odd cells of the
// split go to the last margins, so a centered child with an odd cell to spare stands where `Center`
// would put it on its own. A float widened so stands its child in its side of the widened
// rectangle; the children after it move along.
//
// So a counter followed by a right float with buttons puts the counter at the left edge and the
// buttons at the right one; a centered float between two labels stands in the middle between them.
// Without floats the spare width stays after the last child, as it always did. A float that
// occupies nothing gets no margin, for the reason a gap is not due before it.
//
// Only the children themselves count: a float deeper in the tree is placed by its own parent.
void Row::layout(Size given) {
    const std::vector<Area> places = placed(given);
    const std::size_t many = places.size();

    std::vector<const Float*> floats(many, nullptr);

    int end = 0;
    int sides = 0;

    for (std::size_t which = 0; which < many; ++which) {
        end = std::max(end, places[which].at.x + places[which].size.wide);

        if (places[which].size.wide == 0) continue;

        floats[which] = dynamic_cast<const Float*>(children()[which].get());

        if (floats[which] != nullptr && floats[which]->axis() != Axis::horizontal) floats[which] = nullptr;

        if (floats[which] != nullptr) sides += floats[which]->align() == Align::middle ? 2 : 1;
    }

    const int spare = std::max(given.wide - end, 0);

    // The width of margin number `k` of `sides`, counted from the left.
    const auto share = [spare, sides](int k) { return spare / sides + (k >= sides - spare % sides ? 1 : 0); };

    int shift = 0;
    int k = 0;

    for (std::size_t which = 0; which < many; ++which) {
        Area where = places[which];

        where.at.x += shift;
        where.size.high = std::max(given.high, 0);

        if (const Float* one = floats[which]; one != nullptr) {
            const int before = one->align() != Align::start ? share(k++) : 0;
            const int after = one->align() != Align::end ? share(k++) : 0;

            where.size.wide += before + after;
            shift += before + after;
        }

        children()[which]->arrange(where);
    }
}

// Paints the children side by side, where the layout put them.
void Row::paint(Canvas& onto) const {
    for (const std::shared_ptr<Node>& one : children()) one->render(onto);
}

}
