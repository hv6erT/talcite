#include "lineup.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>

#include "talcite/float.hpp"

namespace talcite::ui::lineup {

// Children in one line, side by side in a `Row` (`Axis::horizontal`) or one under another in a
// `Column` (`Axis::vertical`). Both are the same layout with the axes swapped, so it is written
// once, here, in the terms of a row: "wide" and `x` run along the line, "high" and `y` across it.
// For a column every size and place is turned on the way in and turned back on the way out.
namespace {

// The size with its two lengths swapped when the line runs down, as it is otherwise. Turning twice
// gives the size back, so the same function goes in both directions.
Size turned(Size one, Axis along) { return along == Axis::vertical ? Size{one.high, one.wide} : one; }

// The point with its coordinates swapped when the line runs down, as `turned` does for a size.
Spot turned(Spot one, Axis along) { return along == Axis::vertical ? Spot{one.y, one.x} : one; }

// The rectangle turned as a whole: its corner and its size.
Area turned(Area one, Axis along) { return {turned(one.at, along), turned(one.size, along)}; }

}

// Where each child stands when every one gets the length it wants along the line, in turn from the
// start; across the line the child gets what it wishes. One answer for both questions a row or a
// column hears: `wanted` measures it and `laid` widens it, so the size reported and the places
// drawn in cannot drift apart.
//
// A gap is due only between children that occupy something. A child of zero length (an empty
// label, a disabled button, a node without content) opens no gap, because it would leave a hole
// where nothing is visible. That is why we remember not "which child in turn this is" but "whether
// something already stands here". This is a matter of layout, not of content: otherwise every
// window would have to separate its buttons with empty nodes and fix them at every removal.
//
// Nobody drops out, even when the room has run out: a node that is not visible gets a rectangle of
// zero length, so that its area() tells the truth about this frame. But it opens no gap before
// itself.
std::vector<Area> placed(const Children& all, Size given, int gap, Axis along) {
    const Size room = turned(given, along);

    std::vector<Area> made;

    made.reserve(all.many());

    int x = 0;

    for (const std::shared_ptr<Node>& one : all) {
        const int apart = x > 0 ? gap : 0;
        const int left = std::max(room.wide - x - apart, 0);
        const Size wants = turned(one->wants(turned(Size{left, room.high}, along)), along);
        const int wide = std::clamp(wants.wide, 0, left);
        const int from = wide > 0 ? x + apart : x;

        made.push_back(turned(Area{{from, 0}, {wide, wide > 0 ? std::clamp(wants.high, 0, std::max(room.high, 0)) : 0}}, along));

        x = from + wide;
    }

    return made;
}

// The size the children want in a line, with gaps, clamped to `room`: along the line the end of the
// last one, across it the largest. Every child stands at the start across the line, so the corner
// opposite the origin of the places is this size whichever way the line runs. A line may not tell
// its parent that it needs a hundred rows, because the parent has nowhere to take them from;
// showing what does not fit is the job of `Scroll`. Floats count with the size of what they hold
// and nothing more: the room they stand in is what the line has to spare, and a line that asked for
// it would take the whole of every window it stands in.
Size wanted(const std::vector<Area>& places, Size room) {
    Size made;

    for (const Area& one : places) {
        made.wide = std::max(made.wide, one.at.x + one.size.wide);
        made.high = std::max(made.high, one.at.y + one.size.high);
    }

    return {std::min(made.wide, room.wide), std::min(made.high, room.high)};
}

// Lays the children out in a line, each over the full size across it.
//
// The length the children do not need goes to the floats along the line among them (`Left`,
// `Right` and `Center` in a row, `Top`, `Bottom` and `Middle` in a column; a float of the other
// axis is an ordinary child, placing its own child across the line), by the rule of automatic
// margins, as `margin: auto` in a flexible box: a float has an empty margin on its free side (a
// left float on its right, a right float on its left, a centered one on both), and the spare
// length is split equally between all those margins. The odd cells of the split go to the last
// margins, so a centered child with an odd cell to spare stands where `Center` would put it on its
// own. A float widened so stands its child in its side of the widened rectangle; the children
// after it move along.
//
// So a counter followed by a right float with buttons puts the counter at the left edge and the
// buttons at the right one; a centered float between two labels stands in the middle between them;
// content followed by a bottom float puts the float at the bottom edge. Without floats the spare
// length stays after the last child. A float that occupies nothing gets no margin, for the reason a
// gap is not due before it.
//
// Only the children themselves count: a float deeper in the tree is placed by its own parent.
void laid(const Children& all, Size given_as_is, int gap, Axis along) {
    const std::vector<Area> laid_as_is = placed(all, given_as_is, gap, along);
    const Size given = turned(given_as_is, along);
    const std::size_t many = laid_as_is.size();

    std::vector<Area> places;
    std::vector<const Float*> floats(many, nullptr);

    places.reserve(many);

    int end = 0;
    int sides = 0;

    for (std::size_t which = 0; which < many; ++which) {
        places.push_back(turned(laid_as_is[which], along));

        end = std::max(end, places[which].at.x + places[which].size.wide);

        if (places[which].size.wide == 0) continue;

        floats[which] = dynamic_cast<const Float*>(all[which].get());

        if (floats[which] != nullptr && floats[which]->axis() != along) floats[which] = nullptr;

        if (floats[which] != nullptr) sides += floats[which]->align() == Align::middle ? 2 : 1;
    }

    const int spare = std::max(given.wide - end, 0);

    // The length of margin number `k` of `sides`, counted from the start.
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

        all[which]->arrange(turned(where, along));
    }
}

}
