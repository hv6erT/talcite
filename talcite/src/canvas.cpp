#include "talcite/canvas.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "talcite/encoding.hpp"

// A window onto the one sheet of a frame. A node paints in its own coordinates, (0, 0) being its
// upper left corner, and the canvas shifts what it paints to where the node stands on the sheet and
// cuts it to what may be painted (the clip, in sheet coordinates). The sheet belongs to whoever
// renders the frame; a canvas only points at it, so a copy costs a pointer and two rectangles, and
// a child gets its own by value (`within`).
//
// Painting goes from the back to the front: a parent before its children, an older sibling before a
// younger one, the lower layer before the upper. So whatever a node writes lands on what lies under
// it already, and what used to be resolved when a sheet of its own was laid onto its parent's
// (`View::draw`) is resolved at once: a cell the node does not touch stays as it is, a shade dims
// what is there, a foreground cell takes the background from below. The rules are the same as in
// `View::draw`; only the moment differs.
namespace talcite {

// The whole sheet, with (0, 0) in its corner and nothing cut away.
Canvas::Canvas(View& sheet) : sheet_(&sheet), size_(sheet.size()), clip_{{0, 0}, sheet.size()} {}

// What of the node's rectangle may be painted, in the node's own coordinates. Empty when nothing
// of the node can be seen.
ui::Area Canvas::seen() const { return clip_ - origin_; }

// The canvas of a child standing at `where` in this node's coordinates: its corner moves there,
// and it may paint only where both it and this node may. A rectangle of negative size (a cramped
// window) is an empty one.
Canvas Canvas::within(ui::Area where) const {
    Canvas made = *this;

    made.origin_ = origin_ + where.at;
    made.size_ = {std::max(where.size.wide, 0), std::max(where.size.high, 0)};
    made.clip_ = ui::Area::crossed(clip_, {made.origin_, made.size_});

    return made;
}

// The same canvas, cut down to `room` (in the node's coordinates): the window of a scroller without
// its bars, which the content must not paint over.
Canvas Canvas::clipped(ui::Area room) const {
    Canvas made = *this;

    made.clip_ = ui::Area::crossed(clip_, room + origin_);

    return made;
}

// Whether the sheet cell at `where` may be painted.
bool Canvas::holds(ui::Spot where) const {
    return where.x >= clip_.at.x && where.y >= clip_.at.y && where.x < clip_.at.x + clip_.size.wide &&
           where.y < clip_.at.y + clip_.size.high;
}

// A rectangle in the node's coordinates as the part of the sheet it may paint.
ui::Area Canvas::cut(const ui::Area& where) const { return ui::Area::crossed(where + origin_, clip_); }

// One cell's content at `at`, as `View::put` puts it, cut to the clip. The edge of the clip is an
// edge as the edge of a sheet was: a double-width character whose other half would fall outside
// becomes a space, and a character that joins the glyph to its left (a combining mark) joins only a
// glyph inside. Half of someone else's double-width character just outside the edge is mended by
// the sheet, outside the clip too: that is what `View::draw` did at the ends of a run of cells.
void Canvas::put(ui::Spot at, char32_t what, const Style& how) {
    const ui::Spot here = at + origin_;

    if (what == 0 || !holds(here)) return;

    const int much = width(shown(what));

    if (much == 0) {
        ui::Spot left{here.x - 1, here.y};

        if (holds(left) && sheet_->cell(left).letter == 0) --left.x;

        if (!holds(left)) return;
    }

    if (much == 2 && !holds({here.x + 1, here.y})) {
        sheet_->broken(here);

        sheet_->cell(here) = Cell{U' ', how};

        return;
    }

    sheet_->put(here, what, how);
}

// UTF-8 text from `at`, cluster by cluster, advancing by each glyph's width and stopping at the
// right edge of the clip; a cluster that shows nothing takes no cell, as in `View::put`. A row
// outside the clip is not read at all.
void Canvas::put(ui::Spot at, std::string_view what, const Style& how) {
    const int row = at.y + origin_.y;

    if (row < clip_.at.y || row >= clip_.at.y + clip_.size.high) return;

    const int right = clip_.at.x + clip_.size.wide - origin_.x;

    std::size_t which = 0;

    while (which < what.size() && at.x < right) {
        const std::size_t next = cluster_after(what, which);
        const char32_t one = glyph(what.substr(which, next - which));

        which = next;

        if (one == 0) continue;

        put(at, one, how);

        at.x += width(one);
    }
}

// Fills `where` with spaces in the style `how`, cut to the clip.
void Canvas::fill(const ui::Area& where, const Style& how) {
    const ui::Area part = cut(where);

    if (part.size.wide <= 0 || part.size.high <= 0) return;

    sheet_->fill(part, how);
}

// A shadow over `where`: what lies there keeps its character and takes the style; over nothing at
// all the cell stays a shadow, which the terminal shows as an empty cell in that style. A pixel
// picture cannot be dimmed, so the shadow takes the cell away from it.
void Canvas::shade(const ui::Area& where, const Style& how) {
    const ui::Area part = cut(where);

    for (int y = part.at.y; y < part.at.y + part.size.high; ++y)
        for (int x = part.at.x; x < part.at.x + part.size.wide; ++x) {
            Cell& under = sheet_->cell({x, y});

            under.style = how;

            if (under.shown == Shown::none) under.shown = Shown::shade;

            under.picture = 0;
        }
}

// A character with its own color and, when `how` gives none, the background of what lies below.
// Over nothing at all it stays a foreground cell and takes its background from whatever comes under
// it later, as `View::foreground` keeps it.
void Canvas::foreground(ui::Spot at, char32_t what, const Style& how) {
    const ui::Spot here = at + origin_;

    if (what == 0 || !holds(here)) return;

    const Cell under = sheet_->cell(here);

    put(at, what, how);

    if (!how.background.transparent) return;

    Cell& one = sheet_->cell(here);

    if (under.shown == Shown::none) {
        one.shown = Shown::foreground;
    } else {
        one.style.background = under.style.background;
        one.shown = Shown::own;
    }
}

// A pixel picture over `where`, cut to the clip (`View::picture`).
void Canvas::picture(const ui::Area& where, Picture what) { sheet_->picture(cut(where), std::move(what)); }

// A hyperlink over `where`, cut to the clip (`View::link`).
void Canvas::link(const ui::Area& where, Hyperlink what) { sheet_->link(cut(where), std::move(what)); }

// The cell at `where` in the node's coordinates as it is on the sheet now: the node's own or what
// lies under it. Outside the clip an empty cell, as `View::at` gives outside a sheet.
Cell Canvas::at(ui::Spot where) const {
    const ui::Spot here = where + origin_;

    return holds(here) ? sheet_->at(here) : Cell{};
}

}
