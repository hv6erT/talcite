#include "talcite/grid.hpp"

#include <algorithm>
#include <cstddef>

namespace talcite::ui {

namespace {

// Whether `one` is a cell over the rest of its row.
bool across(const Node& one) { return dynamic_cast<const Across*>(&one) != nullptr; }

// Whether `one` is a row of cells; any other child of a grid is a row of its own.
const GridRow* ranked(const Node& one) { return dynamic_cast<const GridRow*>(&one); }

// Where each column starts, for columns of the given widths: a gap after every column that
// occupies something, the rule `Row` has for its children.
std::vector<int> starts(const std::vector<int>& columns, int gap) {
    std::vector<int> made(columns.size(), 0);

    for (std::size_t at = 1; at < columns.size(); ++at)
        made[at] = made[at - 1] + columns[at - 1] + (columns[at - 1] > 0 ? gap : 0);

    return made;
}

// Where the columns end: after the last one, without a gap behind it.
int ending(const std::vector<int>& columns, int gap) {
    if (columns.empty()) return 0;

    return starts(columns, gap).back() + columns.back();
}

}

// A grid lays cells out in rows and columns, and a column is as wide as its widest cell. That is
// what a form needs: labels in one column and values in the next, so the eye goes down one edge,
// whatever the language makes the labels; and two columns of switches, with whatever stands under
// them in the same columns. Padding labels with spaces to the widest one did the same by hand, in
// every window anew, and a sketch of the window showed spaces instead of the layout.
//
// The rows are `GridRow`s, and their children are the cells, in the order of the columns. Any
// other child of the grid is a row of its own, over the whole width: a line across, an empty row
// (`Blank`), a hint. A cell that should take the rest of its row (a field under its label, as wide
// as the columns beside it) stands in an `Across`.
//
// The grid measures and lays out its rows' cells itself, because columns are a matter of the whole
// grid, not of any row: a row knows only its own cells. So a `GridRow` draws nothing and lays out
// nothing; the grid gives it its rectangle (so the mouse goes down through it to the cells, the way
// it goes through every node) and then the cells theirs, inside it.
Grid::Grid(int gap) : gap_(std::max(gap, 0)) {}

// Sets the gap between columns; a negative gap counts as zero.
void Grid::gap(int given) {
    gap_ = std::max(given, 0);

    changed();
}

// A grid takes any number of rows.
int Grid::capacity() const { return plenty; }

// The widths of the columns in the room `room`. A column is as wide as the widest of its ordinary
// cells, each asked with what the columns before it leave. A cell over the rest of its row does not
// widen the column it starts in; when it wants more than the columns from there to the end give,
// the last column grows by the difference, so the columns the other rows line up in stay as they
// are.
std::vector<int> Grid::columns(Size room) const {
    std::size_t many = 0;

    for (const std::shared_ptr<Node>& one : children())
        if (ranked(*one) != nullptr) many = std::max(many, one->children().many());

    std::vector<int> made(many, 0);

    int x = 0;

    for (std::size_t at = 0; at < many; ++at) {
        const int left = std::max(room.wide - x, 0);

        for (const std::shared_ptr<Node>& one : children()) {
            if (ranked(*one) == nullptr || at >= one->children().many()) continue;

            const Node& cell = *one->children()[at];

            if (across(cell)) continue;

            made[at] = std::max(made[at], std::min(cell.wants({left, room.high}).wide, left));
        }

        x += made[at] + (made[at] > 0 ? gap_ : 0);
    }

    for (const std::shared_ptr<Node>& one : children()) {
        if (ranked(*one) == nullptr) continue;

        for (std::size_t at = 0; at < one->children().many() && at < many; ++at) {
            const Node& cell = *one->children()[at];

            if (!across(cell)) continue;

            const int from = starts(made, gap_)[at];
            const int left = std::max(room.wide - from, 0);
            const int wants = std::min(cell.wants({left, room.high}).wide, left);
            const int span = ending(made, gap_) - from;

            if (wants > span) made.back() += wants - span;

            break;
        }
    }

    return made;
}

// The rectangles of the cells of `row`, in the row's coordinates, for a row `high` rows tall. An
// ordinary cell gets the width it wants, at most its column's, at the column's start; a cell over
// the rest of its row gets everything from its column to the end of the last one; cells after it,
// and cells beyond the columns, get nothing.
std::vector<Area> Grid::cells(const Node& row, int high, const std::vector<int>& columns) const {
    const std::vector<int> from = starts(columns, gap_);
    const int end = ending(columns, gap_);

    std::vector<Area> made;

    made.reserve(row.children().many());

    bool over = false;

    for (std::size_t at = 0; at < row.children().many(); ++at) {
        const Node& cell = *row.children()[at];

        if (over || at >= columns.size()) {
            made.push_back({{end, 0}, {0, 0}});

            continue;
        }

        if (across(cell)) {
            made.push_back({{from[at], 0}, {std::max(end - from[at], 0), high}});
            over = true;

            continue;
        }

        made.push_back({{from[at], 0}, {std::min(cell.wants({columns[at], high}).wide, columns[at]), high}});
    }

    return made;
}

// The rectangles of the grid's rows, one under another, in the room `room`: a row of cells as tall
// as its tallest cell, any other child as tall as it wants over the whole width.
std::vector<Area> Grid::rows(Size room, const std::vector<int>& columns) const {
    const std::vector<int> from = starts(columns, gap_);
    const int end = ending(columns, gap_);
    const int wide = std::max(room.wide, 0);

    std::vector<Area> made;

    made.reserve(children().many());

    int y = 0;

    for (const std::shared_ptr<Node>& one : children()) {
        const int left = std::max(room.high - y, 0);

        int high = 0;

        if (ranked(*one) != nullptr) {
            bool over = false;

            for (std::size_t at = 0; at < one->children().many() && at < columns.size() && !over; ++at) {
                const Node& cell = *one->children()[at];

                over = across(cell);

                const int width = over ? std::max(end - from[at], 0) : columns[at];

                high = std::max(high, cell.wants({width, left}).high);
            }
        } else {
            high = one->wants({wide, left}).high;
        }

        high = std::clamp(high, 0, left);

        made.push_back({{0, y}, {wide, high}});

        y += high;
    }

    return made;
}

// The width of the columns with their gaps and the height of the rows, within the room.
Size Grid::wants(Size room) const {
    const std::vector<int> laid = columns(room);

    int wide = ending(laid, gap_);

    for (const std::shared_ptr<Node>& one : children())
        if (ranked(*one) == nullptr) wide = std::max(wide, one->wants(room).wide);

    int high = 0;

    for (const Area& one : rows(room, laid)) high += one.size.high;

    return {std::min(wide, room.wide), std::min(high, room.high)};
}

// Lays the rows out one under another and, in every row of cells, the cells in their columns: the
// grid places the cells of its rows itself, because only the grid knows the columns.
void Grid::layout(Size given) {
    const std::vector<int> laid = columns(given);
    const std::vector<Area> places = rows(given, laid);

    for (std::size_t which = 0; which < places.size(); ++which) {
        Node& one = *children()[which];
        const Area& where = places[which];

        one.arrange(where);

        if (ranked(one) == nullptr) continue;

        const std::vector<Area> inside = cells(one, where.size.high, laid);

        for (std::size_t at = 0; at < inside.size(); ++at) one.children()[at]->arrange(inside[at]);
    }
}

// Paints the rows in order; a row of cells paints its cells (`GridRow::paint`).
void Grid::paint(Canvas& onto) const {
    for (const std::shared_ptr<Node>& one : children()) one->render(onto);
}

// A row of a grid takes any number of cells.
int GridRow::capacity() const { return plenty; }

// A row of cells is measured by its grid, which knows the columns; on its own it wants nothing.
Size GridRow::wants(Size) const { return {}; }

// A row of cells lays out nothing: its grid lays its cells out in the grid's columns.
void GridRow::layout(Size) {}

// The cells where the grid laid them; between them the row stays transparent.
void GridRow::paint(Canvas& onto) const {
    for (const std::shared_ptr<Node>& one : children()) one->render(onto);
}

// An empty place of the given size: an empty cell of a grid, or an empty row between parts.
// Negative dimensions become zero.
Blank::Blank(Size size) : size_{std::max(size.wide, 0), std::max(size.high, 0)} {}

// A blank holds nothing.
int Blank::capacity() const { return 0; }

// Its size, within the room.
Size Blank::wants(Size room) const {
    return {std::min(size_.wide, std::max(room.wide, 0)), std::min(size_.high, std::max(room.high, 0))};
}

}
