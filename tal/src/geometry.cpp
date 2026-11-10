#include "tal/geometry.hpp"

#include <algorithm>

namespace tal {

// Whether `one` lies in the area; an empty area holds nothing.
bool Area::contains(Spot one) const {
    return one.x >= at.x && one.y >= at.y && one.x < at.x + size.wide && one.y < at.y + size.high;
}

// The area without `less` on its sides. A side larger than what is left leaves the area empty
// there, never of negative size, and its corner stays inside the original.
Area Area::inner(Sides less) const {
    const int wide = std::max(0, size.wide - less.left - less.right);
    const int high = std::max(0, size.high - less.top - less.bottom);

    return Area{{at.x + std::min(less.left, size.wide), at.y + std::min(less.top, size.high)}, {wide, high}};
}

// The part both areas cover. Areas that do not meet give an empty area at the corner where the
// overlap would start.
Area Area::common(Area other) const {
    const int left = std::max(at.x, other.at.x);
    const int top = std::max(at.y, other.at.y);
    const int right = std::min(at.x + size.wide, other.at.x + other.size.wide);
    const int bottom = std::min(at.y + size.high, other.at.y + other.size.high);

    return Area{{left, top}, {std::max(0, right - left), std::max(0, bottom - top)}};
}

}
