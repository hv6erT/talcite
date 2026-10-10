#pragma once

#include <algorithm>
#include <string>

#include "eliding.hpp"
#include "talcite/area.hpp"
#include "talcite/shape.hpp"
#include "talcite/view.hpp"

namespace talcite::ui {

// A label set into a line: in a frame's border or in a separator. It lives in one place because
// both `Frame` and `Line` draw it, and two copies of the same function in two translation units
// violate the one-definition rule without the linker reporting it (the same trap as described at
// `HandPromise`).
//
// The label gets a space on each side so that it does not stick to the line; Midnight Commander
// does this and it simply reads better. We shorten it when it does not fit in the given room.
//
// The style comes from outside, not from a role. This label is drawn inside another widget, so it
// is the host that queried the skin and knows how its title should look.
inline void along(View& into, Spot at, int much, const std::string& said, Justify justify,
                  const Style& look, char32_t mark) {
    if (said.empty() || much <= 2) return;

    const std::string whole = " " + said + " ";

    const int wide = std::min(measured(whole).wide, much);

    const int aside = much - wide;
    const int x = justify == Justify::left    ? 0
                  : justify == Justify::right ? aside
                                                   : aside / 2;

    into.put({at.x + x, at.y}, elided(whole, wide, Elide::right, mark), look);
}

}
