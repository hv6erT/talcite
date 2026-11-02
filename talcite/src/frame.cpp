#include "talcite/frame.hpp"

#include <algorithm>

#include "talcite/encoding.hpp"
#include "talcite/line.hpp"
#include "talcite/skin.hpp"
#include "lining.hpp"
#include "titling.hpp"

namespace talcite::ui {

// A frame with a title, drawn in the `talcite.frame` role.
Frame::Frame(std::string title, FrameShape shape) : title_(std::move(title)), shape_(shape) {
    role("talcite.frame");
}

// Which edges are drawn and how much each takes. Without a line there are none, and a switched-off
// edge takes not a single cell from the content. Top and bottom are always one row, but the sides
// are as wide as the characters the skin draws them with: for double-width characters that is two
// cells, and so the interior starts one cell further in. For ordinary box-drawing characters it all
// comes out as one.
Frame::Edges Frame::edges() const {
    if (shape_.border == Border::none) return {};

    const Sides side = sided(skin().lines(shape_.border));

    return {shape_.top ? 1 : 0, shape_.bottom ? 1 : 0, shape_.left ? side.left : 0,
            shape_.right ? side.right : 0};
}

// How much of the right side the content lies over (`overlaid`): the whole side when it is one cell
// wide, so the content's last column stands on it and the side shows wherever that column is
// clear, the way a table's scrollbar column turns into the side with the bar on it. A side of a
// double-width character is not split, so there the content stays inside, as without `overlaid`.
int Frame::covered(const Edges& edge) const { return shape_.overlaid && edge.right == 1 ? 1 : 0; }

// The frame takes a cell from every side it draws, just like the bar in a scroller and for the same
// reason: it is part of the frame, not something painted over the content. A switched-off edge
// takes nothing, and a side the content lies over (`covered`) is the content's too.
//
// The title takes room too: the frame draws it, so the frame measures it. Without that, a frame
// computes its width from its content and draws the title in whatever is left, so a narrow window
// with a long title would cut it off. The interior is therefore no narrower than the title with a
// cell of padding on each side and one more beyond it (four cells more than the text), within the
// room. Only where the title shows, on a drawn top edge. The footer is not measured: it says how
// things stand (a count, a total) and gives way to the content, as it always did.
Size Frame::wants(Size room) const {
    const Edges edge = edges();

    const int aside = edge.left + edge.right - covered(edge);
    const int apart = edge.top + edge.bottom;

    const Size inside{std::max(room.wide - aside, 0), std::max(room.high - apart, 0)};

    Size wants = children().empty() ? Size{} : children().front()->wants(inside);

    if (edge.top != 0 && !title_.empty())
        wants.wide = std::max(wants.wide, std::min(measured(title_).wide + 4, inside.wide));

    return {std::min(wants.wide + aside, room.wide), std::min(wants.high + apart, room.high)};
}

// Paints the background, the edges with the title and the footer, the child inside, and the joints
// where a divider in the content reaches the sides. A content that lies over the right side
// (`covered`) is drawn after the side, so the side shows where the content leaves its cells clear;
// the corners and the joints are the frame's still.
View Frame::paint(Size given, Area seen) {
    View made{seen.size};

    const Skin& how = skin();

    // The stem of the role name is set from outside, and the title follows the stem:
    // "talcite.frame.title", and for a panel with the role tw.panel.focus, tw.panel.focus.title. A
    // shared title role is not enough, because Midnight Commander has three different title colors:
    // the active panel (reverse), a dialog (dtitle) and an error dialog (errdtitle). The divider
    // keeps its own.
    //
    // The bottom text is drawn in the frame's style, not the title's. MC paints the status line
    // under the file list in the plain panel color (display_mini_info calls set_colors) and the
    // title of the active panel reversed; if the footer followed the title, it would be reversed
    // along with it. Its alignment is separate too (`footing`), because in MC the title is centered
    // and the status line is left-aligned.
    const Style look = how.style(role());
    const Style told = how.style(role() + ".title");

    const char32_t mark = how.glyph("talcite.elide");

    // The frame paints its own background. The gap between the content and the edge, the space next
    // to a shorter text and the row under the last child must be in the window's color, not in the
    // color of whatever lay underneath; no window in Midnight Commander is transparent there.
    //
    // Under the built-in skin `talcite.frame` is an empty style, and a fresh sheet is spaces in the
    // empty style anyway, so the fill writes exactly what was already there and the frame does not
    // change by a single cell.
    made.fill({{-seen.at.x, -seen.at.y}, given}, look);

    const Edges edge = edges();

    if (given.wide > 0 && given.high > 0) {
        const Lines lines = how.lines(shape_.border);

        // The right side starts where the interior ends, not on the last column, because a side
        // made of a double-width character starts one cell earlier.
        const int right_x = given.wide - edge.right;
        const int last_y = given.high - 1;

        // There is no text without an edge to stand in.
        const int room = given.wide - edge.left - edge.right;

        // The line runs across the whole width when there is no side; a corner exists only where
        // two drawn edges meet.
        if (edge.top != 0)
            across(made, {edge.left - seen.at.x, -seen.at.y}, room, lines.across, look);

        if (edge.bottom != 0)
            across(made, {edge.left - seen.at.x, last_y - seen.at.y}, room, lines.across, look);

        if (edge.left != 0)
            for (int y = edge.top; y <= last_y - edge.bottom; ++y)
                made.put({-seen.at.x, y - seen.at.y}, lines.upright, look);

        if (edge.right != 0)
            for (int y = edge.top; y <= last_y - edge.bottom; ++y)
                made.put({right_x - seen.at.x, y - seen.at.y}, lines.upright, look);

        if (edge.top != 0 && edge.left != 0)
            made.put({-seen.at.x, -seen.at.y}, lines.upper_left, look);

        if (edge.top != 0 && edge.right != 0)
            made.put({right_x - seen.at.x, -seen.at.y}, lines.upper_right, look);

        if (edge.bottom != 0 && edge.left != 0)
            made.put({-seen.at.x, last_y - seen.at.y}, lines.lower_left, look);

        if (edge.bottom != 0 && edge.right != 0)
            made.put({right_x - seen.at.x, last_y - seen.at.y}, lines.lower_right, look);

        if (edge.top != 0)
            along(made, {edge.left - seen.at.x, -seen.at.y}, room, title_, shape_.justify, told,
                  mark);

        if (edge.bottom != 0 && last_y > 0)
            along(made, {edge.left - seen.at.x, last_y - seen.at.y}, room, footer_,
                  shape_.footing, look, mark);
    }

    if (children().empty()) return made;

    const Area where{{edge.left, edge.top},
                     {std::max(given.wide - edge.left - edge.right + covered(edge), 0),
                      std::max(given.high - edge.top - edge.bottom, 0)}};

    const Area part = Area::crossed(seen, where);

    made.draw(children().front()->render(where, part - where.at), part.at - seen.at);

    // A divider reaches the frame. A child never paints outside its rectangle, so the joint is
    // placed by the frame, which recognizes it from the composed image, not from the kind of node
    // inside. Where the content touches both sides with the same horizontal character the frame
    // draws itself with, the edge turns into a joint.
    //
    // Recognizing by image rather than by type works through any nesting and does not require the
    // frame to know the divider. The price is that a text made only of such dashes across the whole
    // width also gets joints; that is rare, and it looks the way it should.
    if (shape_.border != Border::none && (edge.left != 0 || edge.right != 0)) {
        const Lines lines = how.lines(shape_.border);

        const int right_x = given.wide - edge.right;
        const int first = edge.left;
        const int last = right_x - 1;

        for (int y = edge.top; y <= given.high - 1 - edge.bottom && first <= last; ++y) {
            if (made.at({first - seen.at.x, y - seen.at.y}).letter != lines.across) continue;

            if (!reaching(made, {last - seen.at.x, y - seen.at.y}, lines.across)) continue;

            if (edge.left != 0) made.put({-seen.at.x, y - seen.at.y}, lines.left_joint, look);

            if (edge.right != 0)
                made.put({right_x - seen.at.x, y - seen.at.y}, lines.right_joint, look);
        }
    }

    return made;
}

}
