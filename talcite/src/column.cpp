#include "talcite/column.hpp"

#include <algorithm>
#include <cstddef>

#include "talcite/float.hpp"

namespace talcite::ui {

Column::Column(int gap) : gap_(std::max(gap, 0)) {}

// Sets the gap between children, in rows; a negative value becomes zero.
void Column::gap(int given) { gap_ = std::max(given, 0); }

// A column lays out any number of children, one under another.
int Column::capacity() const { return plenty; }

// Where each child stands when every one gets the height it wants, top to bottom; the width is the
// child's wish. One answer for both questions a column hears, as in `Row::placed`, whose rules
// (the gap only between children that occupy something, nobody dropping out when the room runs
// out) hold here with the axes swapped.
std::vector<Area> Column::placed(Size room) const {
    std::vector<Area> made;

    made.reserve(children().many());

    int y = 0;

    for (const std::shared_ptr<Node>& one : children()) {
        const int apart = y > 0 ? gap_ : 0;
        const int left = std::max(room.high - y - apart, 0);
        const Size wants = one->wants({room.wide, left});
        const int high = std::clamp(wants.high, 0, left);
        const int from = high > 0 ? y + apart : y;

        made.push_back({{0, from}, {high > 0 ? std::clamp(wants.wide, 0, std::max(room.wide, 0)) : 0, high}});

        y = from + high;
    }

    return made;
}

// The children's heights add up and the width is that of the widest child, but the column asks
// for at most as much as it was given. It may not tell its parent that it needs a hundred rows,
// because the parent has nowhere to take them from; showing what does not fit is the job of
// `Scroll`. Floats count with the height of what they hold, for the reason given at `Row::wants`.
Size Column::wants(Size room) const {
    Size made;

    for (const Area& one : placed(room)) {
        made.wide = std::max(made.wide, one.size.wide);
        made.high = std::max(made.high, one.at.y + one.size.high);
    }

    return {std::min(made.wide, room.wide), std::min(made.high, room.high)};
}

// Draws the children one under another, each over the full width. No child drops out of the loop,
// even when the room has run out: a node that cannot be seen gets a rectangle of zero height, so
// its area() tells the truth about this frame instead of remembering the previous one.
//
// The height the children do not need goes to the vertical floats among them (`Top`, `Bottom`,
// `Middle`) by the rule of automatic margins described at `Row::paint`, with the axes swapped: a
// top float has its free margin under it, a bottom one above it, a middle one on both sides, and
// the spare rows are split equally between those margins, the odd ones to the last. So a column
// of content followed by a bottom float puts the float at the bottom edge. A horizontal float is an
// ordinary child here, placing its own child in the full width. Without vertical floats the spare
// rows stay under the last child.
View Column::paint(Size given, Area seen) {
    View made{seen.size};

    // Transparent wherever nothing is drawn; the rule is described with `Node::paint`.

    made.clear({{0, 0}, seen.size});

    const std::vector<Area> places = placed(given);
    const std::size_t many = places.size();

    std::vector<const Float*> floats(many, nullptr);

    int end = 0;
    int sides = 0;

    for (std::size_t which = 0; which < many; ++which) {
        end = std::max(end, places[which].at.y + places[which].size.high);

        if (places[which].size.high == 0) continue;

        floats[which] = dynamic_cast<const Float*>(children()[which].get());

        if (floats[which] != nullptr && floats[which]->axis() != Axis::vertical) floats[which] = nullptr;

        if (floats[which] != nullptr) sides += floats[which]->align() == Align::middle ? 2 : 1;
    }

    const int spare = std::max(given.high - end, 0);

    // The height of margin number `k` of `sides`, counted from the top.
    const auto share = [spare, sides](int k) { return spare / sides + (k >= sides - spare % sides ? 1 : 0); };

    int shift = 0;
    int k = 0;

    for (std::size_t which = 0; which < many; ++which) {
        Area where = places[which];

        where.at.y += shift;
        where.size.wide = std::max(given.wide, 0);

        if (const Float* one = floats[which]; one != nullptr) {
            const int before = one->align() != Align::start ? share(k++) : 0;
            const int after = one->align() != Align::end ? share(k++) : 0;

            where.size.high += before + after;
            shift += before + after;
        }

        const Area part = Area::crossed(seen, where);

        made.draw(children()[which]->render(where, part - where.at), part.at - seen.at);
    }

    return made;
}

}
