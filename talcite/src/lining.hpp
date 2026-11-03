#pragma once

#include <algorithm>
#include <initializer_list>
#include <string_view>

#include "talcite/canvas.hpp"
#include "talcite/encoding.hpp"
#include "talcite/line.hpp"
#include "talcite/shape.hpp"

namespace talcite::ui {

// The name of the glyph set in the skin. The widget still chooses the weight, since that is a
// choice of shape, not of looks; the skin only says which glyphs draw that weight.
[[nodiscard]] inline std::string_view named(Border how) {
    switch (how) {
        case Border::twin: return "talcite.line.twin";
        case Border::thick: return "talcite.line.thick";
        default: return "talcite.line.thin";
    }
}

// The three sets of frame glyphs in one place, because two copies of the same table would sooner
// or later start to differ. This is not a public header: the glyphs come from the skin, so the
// only reader of this table is the skin itself, which takes its built-in values from here.
// `Border::none` has no entry: whoever draws no line does not ask for glyphs.
[[nodiscard]] inline Lines drawn(Border how) {
    switch (how) {
        case Border::twin: return {U'╔', U'╗', U'╚', U'╝', U'═', U'║', U'╠', U'╣'};
        case Border::thick: return {U'┏', U'┓', U'┗', U'┛', U'━', U'┃', U'┣', U'┫'};
        default: return {U'┌', U'┐', U'└', U'┘', U'─', U'│', U'├', U'┤'};
    }
}

// How many cells the widest of the glyphs takes (at least one); a side on which any of them stands
// must be that wide.
[[nodiscard]] inline int widest(std::initializer_list<char32_t> ones) {
    int made = 1;

    for (const char32_t one : ones) made = std::max(made, width(one));

    return made;
}

// The widths of the left and right sides of a frame in cells. A double-width glyph takes two: a
// skin may draw the frame with CJK characters, and then a side is no longer one cell and the
// inside starts further in. A side is measured by the widest glyph that stands on it (the
// vertical, the corners and the joint), because a narrower glyph on a wider side just leaves
// background beside it, while a wider one on a narrower side would stick out of the frame.
struct Sides {
    int left = 1;
    int right = 1;
};

// The side widths for the given glyph set.
[[nodiscard]] inline Sides sided(const Lines& one) {
    return {widest({one.upright, one.upper_left, one.lower_left, one.left_joint}),
            widest({one.upright, one.upper_right, one.lower_right, one.right_joint})};
}

// A horizontal line from `from` over `much` cells. It steps by the glyph's width, because a
// double-width glyph put in every column would overwrite its own neighbor. A remainder into which
// a whole glyph no longer fits stays what it was: background, not half a glyph.
inline void across(Canvas& into, Spot from, int much, char32_t glyph, const Style& look) {
    const int step = width(glyph);

    for (int x = 0; x + step <= much; x += step) into.put({from.x + x, from.y}, glyph, look);
}

// Whether a horizontal line reaches the right cell `last`. For a single-width glyph this asks
// about that cell alone. For a double-width one the line may have ended one cell earlier, because
// the last glyph did not fit in an odd width, and that also counts as reaching, because the line
// could not get any further.
[[nodiscard]] inline bool reaching(const Canvas& seen, Spot last, char32_t glyph) {
    const int step = width(glyph);

    for (int x = step == 1 ? last.x : last.x - step; x <= last.x; ++x)
        if (seen.at({x, last.y}).letter == glyph) return true;

    return false;
}

}
