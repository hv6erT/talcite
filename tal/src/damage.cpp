#include "damage.hpp"

#include <algorithm>

namespace tal::damage {

namespace {

// The smallest area that holds both.
Area joined(Area one, Area two) {
    const int left = std::min(one.at.x, two.at.x);
    const int top = std::min(one.at.y, two.at.y);
    const int right = std::max(one.at.x + one.size.wide, two.at.x + two.size.wide);
    const int bottom = std::max(one.at.y + one.size.high, two.at.y + two.size.high);

    return Area{{left, top}, {right - left, bottom - top}};
}

// Whether `outer` holds all of `inner`.
bool holds(Area outer, Area inner) {
    return inner.at.x >= outer.at.x && inner.at.y >= outer.at.y &&
           inner.at.x + inner.size.wide <= outer.at.x + outer.size.wide &&
           inner.at.y + inner.size.high <= outer.at.y + outer.size.high;
}

// Whether two areas together make exactly a rectangle: the same columns, touching or overlapping
// rows, or the same rows, touching or overlapping columns.
bool fitting(Area one, Area two) {
    if (one.at.x == two.at.x && one.size.wide == two.size.wide)
        return one.at.y <= two.at.y + two.size.high && two.at.y <= one.at.y + one.size.high;

    if (one.at.y == two.at.y && one.size.high == two.size.high)
        return one.at.x <= two.at.x + two.size.wide && two.at.x <= one.at.x + one.size.wide;

    return false;
}

}

// Adds `part` to the rectangles `into`. A rectangle already there that holds it takes it in; one it
// holds goes; one that makes a rectangle with it joins it. Past `most` rectangles they all become
// the one that holds them. An empty part adds nothing.
void added(std::vector<Area>& into, Area part, std::size_t most) {
    if (part.size.wide <= 0 || part.size.high <= 0) return;

    for (std::size_t at = 0; at < into.size();) {
        if (holds(into[at], part)) return;

        if (holds(part, into[at]) || fitting(part, into[at])) {
            part = joined(part, into[at]);
            into.erase(into.begin() + static_cast<std::ptrdiff_t>(at));
            at = 0;

            continue;
        }

        ++at;
    }

    into.push_back(part);

    if (into.size() <= most) return;

    Area all = into.front();

    for (const Area& one : into) all = joined(all, one);

    into.assign(1, all);
}

// Where the shadow of `box` falls, as in Midnight Commander: two columns to the right from the
// second row down, and a row below from the third column on, so that it does not read as a second
// border.
std::array<Area, 2> shadowed(Area box) {
    return {Area{{box.at.x + box.size.wide, box.at.y + 1}, {2, std::max(box.size.high - 1, 0)}},
            Area{{box.at.x + 2, box.at.y + box.size.high}, {box.size.wide, 1}}};
}

}
