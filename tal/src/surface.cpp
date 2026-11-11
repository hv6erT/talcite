#include "tal/surface.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <utility>

#include "tal/text.hpp"
#include "damage.hpp"
#include "unicode.hpp"

namespace tal {

namespace {

constexpr std::size_t most_damaged = 64;

// The look `over` laid on `under`: a color it does not give stays, the attributes are its own.
Look laid(const Look& under, const Look& over) {
    Look made = over;

    if (!made.ink) made.ink = under.ink;
    if (!made.paper) made.paper = under.paper;

    return made;
}

// The cluster as a cell shows it: a control character becomes its picture, so that no cell ever
// holds a byte the terminal would act on, and a mark with nothing before it goes on a dotted
// circle (U+25CC), as tal::text counts its cell, rather than joining whatever stands in the cell
// to its left.
std::string shown(std::string_view cluster) {
    std::size_t at = 0;
    const std::optional<char32_t> one = text::decoded(cluster, at);
    std::string made;

    if (one && text::width(*one) == 0 && text::unicode::mark(*one)) {
        text::encoded(made, U'\u25CC');
        made += cluster;

        return made;
    }

    if (!one || at != cluster.size() || text::shown(*one) == *one) return std::string{cluster};

    text::encoded(made, text::shown(*one));

    return made;
}

}

// A surface of `size` cells, every one a blank without colors, measuring clusters as a terminal
// that renders them `how` draws them. A negative size counts as none.
Surface::Surface(Size size, text::Rendering how)
    : size_{std::max(0, size.wide), std::max(0, size.high)},
      rendering_(how),
      cells_(static_cast<std::size_t>(size_.wide) * static_cast<std::size_t>(size_.high)) {}

// The number of columns and rows.
Size Surface::size() const { return size_; }

// The cell at `one`; a spot outside the surface throws std::out_of_range, since asking for it is a
// mistake of the caller.
const Cell& Surface::at(Spot one) const {
    if (!inside(one)) throw std::out_of_range{"tal::Surface::at: the spot lies outside the surface"};

    return cells_[static_cast<std::size_t>(one.y) * static_cast<std::size_t>(size_.wide) + static_cast<std::size_t>(one.x)];
}

// A canvas over `part` of the surface, in the part's own coordinates, drawing only where the part
// and the surface meet.
Canvas Surface::canvas(Area part) { return Canvas{*this, part.at, part.size, part.common(Area{{0, 0}, size_})}; }

// The rectangles whose cells changed since the last `forget`, none overlapping another one it
// could join into a rectangle.
std::span<const Area> Surface::damaged() const { return damaged_; }

// Starts counting changes anew, after the changes so far were shown.
void Surface::forget() { damaged_.clear(); }

// The address of the link `link` carries, or nothing for 0 or a link no cell holds any longer.
std::string_view Surface::address(std::uint32_t link) const {
    if (link == 0 || link > addresses_.size()) return {};

    return addresses_[link - 1];
}

// The cell at `one`, which the caller has checked lies inside.
Cell& Surface::cell(Spot one) {
    return cells_[static_cast<std::size_t>(one.y) * static_cast<std::size_t>(size_.wide) + static_cast<std::size_t>(one.x)];
}

// Whether `one` lies on the surface.
bool Surface::inside(Spot one) const { return Area{{0, 0}, size_}.contains(one); }

// Puts the cluster `cluster`, `wide` cells wide (1 or 2), at `here` with `look` laid over what was
// there, mending any wide character it cuts in half; its picture and link go. A wide cluster with
// only one column of `clip` left becomes a space. `from` and `to` widen to the columns that
// changed.
void Surface::placed(Spot here, std::string_view cluster, int wide, const Look& look, Area clip, int& from, int& to) {
    if (wide == 2 && !clip.contains({here.x + 1, here.y})) {
        cluster = " ";
        wide = 1;
    }

    for (int column = here.x; column < here.x + wide; ++column) {
        const Cell& one = cell({column, here.y});

        if (one.cluster.empty() && column > 0 && column == here.x) blanked({column - 1, here.y}, from, to);

        if (column + 1 < size_.wide && column + 1 >= here.x + wide && cell({column + 1, here.y}).cluster.empty())
            blanked({column + 1, here.y}, from, to);
    }

    const Look made = laid(cell(here).look, look);

    for (int column = here.x; column < here.x + wide; ++column) {
        Cell& one = cell({column, here.y});
        const std::string_view letters = column == here.x ? cluster : std::string_view{};

        if (one.cluster == letters && one.look == made && one.picture == 0 && one.link == 0) continue;

        relinked(one, 0);
        one.cluster = std::string{letters};
        one.look = made;
        one.picture = 0;

        from = std::min(from, column);
        to = std::max(to, column);
    }
}

// Turns the half of a wide character at `one` into a space with its look, so that no cell is left
// holding half a character.
void Surface::blanked(Spot one, int& from, int& to) {
    Cell& held = cell(one);

    held.cluster = " ";

    from = std::min(from, one.x);
    to = std::max(to, one.x);
}

// Gives the cell `one` the link `link`, holding the new one and letting go of the old.
void Surface::relinked(Cell& one, std::uint32_t link) {
    if (one.link == link) return;

    if (link != 0) ++holders_[link - 1];

    const std::uint32_t old = one.link;

    one.link = link;

    if (old != 0) {
        --holders_[old - 1];

        released(old);
    }
}

// The number of `address`, made when no cell holds it yet. A new number takes a freed one first.
std::uint32_t Surface::interned(std::string_view address) {
    if (const auto at = known_.find(address); at != known_.end()) return at->second;

    std::uint32_t made = 0;

    if (!spare_.empty()) {
        made = spare_.back();
        spare_.pop_back();
        addresses_[made - 1] = std::string{address};
    } else {
        addresses_.emplace_back(address);
        holders_.push_back(0);
        made = static_cast<std::uint32_t>(addresses_.size());
    }

    known_.emplace(std::string{address}, made);

    return made;
}

// Frees the number `link` when no cell holds it any longer.
void Surface::released(std::uint32_t link) {
    if (holders_[link - 1] != 0) return;

    known_.erase(addresses_[link - 1]);
    addresses_[link - 1].clear();
    spare_.push_back(link);
}

// Notes that the cells of `part` changed (`damage::added`).
void Surface::spoiled(Area part) { damage::added(damaged_, part, most_damaged); }

// A canvas onto `onto` whose own (0, 0) lies at `origin` of the surface, `size` large, drawing only
// inside `clip` (in the surface's coordinates).
Canvas::Canvas(Surface& onto, Spot origin, Size size, Area clip) : onto_(&onto), origin_(origin), size_(size), clip_(clip) {}

// The size the canvas was made with, whether or not all of it lies on the surface.
Size Canvas::size() const { return size_; }

// A canvas over `part` of this one, in the part's own coordinates, drawing only where it meets
// this canvas.
Canvas Canvas::within(Area part) const {
    const Spot origin{origin_.x + part.at.x, origin_.y + part.at.y};

    return Canvas{*onto_, origin, part.size, Area{origin, part.size}.common(clip_)};
}

// The cells of `part` (in the canvas's coordinates) that lie inside the clip, in the surface's
// coordinates.
Area Canvas::cut(Area part) const {
    return Area{{origin_.x + part.at.x, origin_.y + part.at.y}, part.size}.common(clip_);
}

// Writes `text` from `at`, cluster by cluster, each as wide as tal::text measures it for the
// surface's rendering; a cluster that takes no cell is left out and a control character shows as
// its picture. What falls outside the clip is not written. Returns how many columns the whole text
// takes, also the part cut off, so that the caller can go on after it.
int Canvas::put(Spot at, std::string_view text, const Look& look) {
    const int row = origin_.y + at.y;
    int x = origin_.x + at.x;
    int from = onto_->size_.wide;
    int to = -1;

    const text::Rendering how = onto_->rendering_;

    if (row < clip_.at.y || row >= clip_.at.y + clip_.size.high) return text::width(text, how);

    const int right = clip_.at.x + clip_.size.wide;
    int taken = 0;

    for (const std::string_view one : text::clusters(text, how)) {
        const int wide = text::width(one, how);

        if (wide <= 0) continue;

        if (x >= right) {
            taken += wide;

            continue;
        }

        if (x >= clip_.at.x) {
            onto_->placed({x, row}, shown(one), std::min(wide, 2), look, clip_, from, to);
        } else if (x + wide > clip_.at.x) {
            onto_->placed({clip_.at.x, row}, " ", 1, look, clip_, from, to);
        }

        x += wide;
        taken += wide;
    }

    if (to >= from) onto_->spoiled(Area{{from, row}, {to - from + 1, 1}});

    return taken;
}

// Fills `part` with blanks in `look`, mending the wide characters it cuts.
void Canvas::fill(Area part, const Look& look) {
    const Area cut_part = cut(part);

    for (int y = cut_part.at.y; y < cut_part.at.y + cut_part.size.high; ++y) {
        int from = onto_->size_.wide;
        int to = -1;

        for (int x = cut_part.at.x; x < cut_part.at.x + cut_part.size.wide; ++x) onto_->placed({x, y}, " ", 1, look, clip_, from, to);

        if (to >= from) onto_->spoiled(Area{{from, y}, {to - from + 1, 1}});
    }
}

// Draws the lines of a frame around the edge of `part`, with `lines` and in `look`; the inside is
// left as it is. A part narrower or lower than two cells has no frame. Only the stretches of the
// edges inside the clip are walked: a frame repainted for the damage of one row writes the two
// cells of that row, not its whole edge.
void Canvas::frame(Area part, const Lines& lines, const Look& look) {
    if (part.size.wide < 2 || part.size.high < 2) return;

    const int left = part.at.x;
    const int top = part.at.y;
    const int right = part.at.x + part.size.wide - 1;
    const int bottom = part.at.y + part.size.high - 1;

    const int seen_left = clip_.at.x - origin_.x;
    const int seen_top = clip_.at.y - origin_.y;
    const int seen_right = seen_left + clip_.size.wide - 1;
    const int seen_bottom = seen_top + clip_.size.high - 1;

    (void)put({left, top}, lines.upper_left, look);
    (void)put({right, top}, lines.upper_right, look);
    (void)put({left, bottom}, lines.lower_left, look);
    (void)put({right, bottom}, lines.lower_right, look);

    const int from_x = std::max(left + 1, seen_left);
    const int to_x = std::min(right - 1, seen_right);

    for (const int y : {top, bottom})
        if (y >= seen_top && y <= seen_bottom)
            for (int x = from_x; x <= to_x; ++x) (void)put({x, y}, lines.across, look);

    const int from_y = std::max(top + 1, seen_top);
    const int to_y = std::min(bottom - 1, seen_bottom);

    for (const int x : {left, right})
        if (x >= seen_left && x <= seen_right)
            for (int y = from_y; y <= to_y; ++y) (void)put({x, y}, lines.upright, look);
}

// Marks the cells of `part` as showing the picture `id` (0 for none); their letters stay for a
// terminal that shows no pictures.
void Canvas::picture(Area part, std::uint32_t id) {
    const Area cut_part = cut(part);

    for (int y = cut_part.at.y; y < cut_part.at.y + cut_part.size.high; ++y) {
        int from = onto_->size_.wide;
        int to = -1;

        for (int x = cut_part.at.x; x < cut_part.at.x + cut_part.size.wide; ++x) {
            Cell& one = onto_->cell({x, y});

            if (one.picture == id) continue;

            one.picture = id;
            from = std::min(from, x);
            to = std::max(to, x);
        }

        if (to >= from) onto_->spoiled(Area{{from, y}, {to - from + 1, 1}});
    }
}

// Makes the cells of `part` a link to `address`; an empty address takes the link away.
void Canvas::link(Area part, std::string_view address) {
    const Area cut_part = cut(part);
    const std::uint32_t id = address.empty() ? 0 : onto_->interned(address);

    if (id != 0) ++onto_->holders_[id - 1];

    for (int y = cut_part.at.y; y < cut_part.at.y + cut_part.size.high; ++y) {
        int from = onto_->size_.wide;
        int to = -1;

        for (int x = cut_part.at.x; x < cut_part.at.x + cut_part.size.wide; ++x) {
            Cell& one = onto_->cell({x, y});

            if (one.link == id) continue;

            onto_->relinked(one, id);
            from = std::min(from, x);
            to = std::max(to, x);
        }

        if (to >= from) onto_->spoiled(Area{{from, y}, {to - from + 1, 1}});
    }

    if (id != 0) {
        --onto_->holders_[id - 1];
        onto_->released(id);
    }
}

}
