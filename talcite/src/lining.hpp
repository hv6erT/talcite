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

// Which sides of a box are drawn and how much each takes: the top and the bottom one row or none,
// the left and the right as many cells as `sided` measured, or none.
struct Rim {
    int top = 0;
    int bottom = 0;
    int left = 0;
    int right = 0;
};

// The border of a box of size `box` standing at the origin: the sides that are drawn, and a corner
// only where two drawn sides meet. The top and bottom lines run between the sides, and across the
// whole width where there is no side. The right side starts where the interior ends, not on the
// last column, because a side made of a double-width character starts one cell earlier. A frame
// draws the sides it was asked for, a menu all four, by this one rule.
inline void bordered(Canvas& into, Size box, const Rim& rim, const Lines& lines, const Style& look) {
    if (box.wide <= 0 || box.high <= 0) return;

    const int right_x = box.wide - rim.right;
    const int last_y = box.high - 1;
    const int room = box.wide - rim.left - rim.right;

    if (rim.top != 0) across(into, {rim.left, 0}, room, lines.across, look);

    if (rim.bottom != 0) across(into, {rim.left, last_y}, room, lines.across, look);

    if (rim.left != 0)
        for (int y = rim.top; y <= last_y - rim.bottom; ++y) into.put({0, y}, lines.upright, look);

    if (rim.right != 0)
        for (int y = rim.top; y <= last_y - rim.bottom; ++y) into.put({right_x, y}, lines.upright, look);

    if (rim.top != 0 && rim.left != 0) into.put({0, 0}, lines.upper_left, look);

    if (rim.top != 0 && rim.right != 0) into.put({right_x, 0}, lines.upper_right, look);

    if (rim.bottom != 0 && rim.left != 0) into.put({0, last_y}, lines.lower_left, look);

    if (rim.bottom != 0 && rim.right != 0) into.put({right_x, last_y}, lines.lower_right, look);
}

// The shadow of the box at `box`: to the right and down, offset by one row and two columns, so that
// it does not look like a second border. Menus and dialogs cast the same shadow, because in
// Midnight Commander it is the same shadow. The letters underneath stay and are only dimmed.
inline void shadowed(Canvas& into, const Area& box, const Style& dark) {
    if (box.size.wide <= 0 || box.size.high <= 0) return;

    const int last_x = box.at.x + box.size.wide - 1;
    const int last_y = box.at.y + box.size.high - 1;

    into.shade({{last_x + 1, box.at.y + 1}, {2, box.size.high - 1}}, dark);
    into.shade({{box.at.x + 2, last_y + 1}, {box.size.wide, 1}}, dark);
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
