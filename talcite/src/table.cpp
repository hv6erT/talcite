#include "talcite/table.hpp"

#include <algorithm>
#include <optional>
#include <utility>

#include "talcite/skin.hpp"

#include "eliding.hpp"
#include "scrollbar.hpp"

namespace talcite::ui {

namespace {

// How many rows are taken from the source at once. The same number and the same reason as in the
// scroll: asking for exactly as many as the window is high would cost a call per frame.
constexpr std::size_t batch = 64;

// How many rows one wheel notch moves. The same constant as in the list and the scroller.
constexpr int stepping = 3;

// The gap between columns. Without it a name would touch the size and the two could not be told
// apart.
constexpr int spacing = 1;

}

// The table's state, kept behind a pointer so the public header stays small.
struct Table::Kept {
    TableSource source;
    std::size_t rows = 0;

    std::vector<TableColumn> columns;

    std::size_t top = 0;
    std::optional<std::size_t> which;

    // What is visible. Recomputed when, and only when, the allotted size or the position changes;
    // otherwise every frame would ask the source again, and with animated neighbors that happens
    // ten times a second.
    std::vector<TableRow> shown;
    bool fresh = true;
    Size sized;

    // The computed column widths for the current window width.
    std::vector<int> spread;

    // The selection (`chosen()`) is kept by row number in a bit vector, not a set: for a hundred
    // thousand rows that is twelve kilobytes, and "select all" is as common in a panel as "select
    // three", so a set would often be larger than the vector.

    TableShape shape;

    std::string scrollbar_role = "talcite.scroll";

    Grip grip;
};

// An empty table with the "talcite.table" role and the default shape.
Table::Table() : kept_(std::make_unique<Kept>()) { role("talcite.table"); }

// An empty table with the given shape.
Table::Table(TableShape shape) : kept_(std::make_unique<Kept>()) {
    role("talcite.table");

    kept_->shape = shape;
}

Table::~Table() = default;

// Sets the source of rows and their number, putting the cursor on the first row and clearing the
// selection. The caller gives the number of rows, because only it knows: the table has no way to
// count a directory, and asking the source "how many" at every frame would be expensive.
void Table::source(TableSource given, std::size_t rows) {
    Kept& kept = *kept_;

    kept.source = std::move(given);
    kept.rows = rows;

    kept.top = 0;
    kept.which = rows > 0 ? std::optional<std::size_t>{0} : std::optional<std::size_t>{};

    chosen().clear();

    kept.shown.clear();
    kept.fresh = true;
}

// Follows rows replaced in the middle of the source: the rows [at, at + gone) give way to `came` new
// ones. The current row, the first visible row and the selection stay with the rows they stood on,
// so a list whose part opens or folds keeps its place on the screen. A current row among the
// replaced ones goes to `at` (or to the last row), and an empty table that gets rows stands on the
// first; only then a `ScrollEvent` is sent, because only then the cursor stands on another row than
// before. The visible rows are fetched again on the
// next frame.
void Table::changed(std::size_t at, std::size_t gone, std::size_t came) {
    Kept& kept = *kept_;

    at = std::min(at, kept.rows);
    gone = std::min(gone, kept.rows - at);

    const std::size_t rows = kept.rows - gone + came;

    // A row before the change keeps its number, one after it moves by the difference, and one
    // inside it is gone.
    const auto moved = [at, gone, came](std::size_t row) -> std::optional<std::size_t> {
        if (row < at) return row;

        if (row >= at + gone) return row - gone + came;

        return std::nullopt;
    };

    bool other = false;

    if (kept.which) {
        if (const std::optional<std::size_t> now = moved(*kept.which)) {
            kept.which = now;
        } else {
            kept.which = rows > 0 ? std::optional<std::size_t>{std::min(at, rows - 1)} : std::nullopt;
            other = true;
        }
    } else if (rows > 0) {
        // An empty table that gets rows stands on the first, as one given a source does.
        kept.which = 0;
        other = true;
    }

    kept.top = rows > 0 ? std::min(moved(kept.top).value_or(at), rows - 1) : 0;
    kept.rows = rows;

    // The current row stays in view: the change may have pushed it below the window.
    if (kept.which) {
        const int high = std::max(area().size.high - (kept.shape.heading ? 1 : 0), 1);

        if (*kept.which < kept.top)
            kept.top = *kept.which;
        else if (*kept.which >= kept.top + static_cast<std::size_t>(high))
            kept.top = *kept.which - static_cast<std::size_t>(high) + 1;
    }

    chosen().moved(at, gone, came);
    chosen().keep(rows);

    kept.shown.clear();
    kept.fresh = true;

    if (other) (void)trigger(ScrollEvent{});
}

// Whether a source has been set.
bool Table::sourced() const { return static_cast<bool>(kept_->source); }

// Drops the visible rows so the next frame asks the source again. The selection is not cleared:
// after re-sorting, row numbers mean something else than before, but only the caller knows whether
// that happened, so it calls `chosen().clear()` when it sees fit.
void Table::refresh() {
    kept_->shown.clear();
    kept_->fresh = true;
}

// Sets the columns.
void Table::columns(std::vector<TableColumn> given) {
    kept_->columns = std::move(given);
    kept_->fresh = true;
}

// The columns as set.
const std::vector<TableColumn>& Table::columns() const { return kept_->columns; }

// The number of rows the caller gave with the source.
std::size_t Table::rows() const { return kept_->rows; }

// The current row, or nothing in an empty table.
std::optional<std::size_t> Table::which() const { return kept_->which; }

// The first visible row.
std::size_t Table::first() const { return kept_->top; }

// How many rows the last frame showed.
std::size_t Table::shown() const { return kept_->shown.size(); }

// Whether the table shows a heading row.
bool Table::heading() const { return kept_->shape.heading; }

// Turns the heading row on or off.
void Table::heading(bool given) {
    if (kept_->shape.heading == given) return;

    kept_->shape.heading = given;
    kept_->fresh = true;
}

// Whether the table keeps a column for the scrollbar.
bool Table::scrollbar() const { return kept_->shape.scrollbar; }

// Turns the scrollbar column on or off.
void Table::scrollbar(bool given) {
    if (kept_->shape.scrollbar == given) return;

    kept_->shape.scrollbar = given;
    kept_->fresh = true;
}

// The skin role of the scrollbar.
const std::string& Table::scrollbar_role() const { return kept_->scrollbar_role; }

// Sets the skin role of the scrollbar.
void Table::scrollbar_role(std::string given) { kept_->scrollbar_role = std::move(given); }

// A table always takes the focus.
bool Table::focusable() const { return true; }

// A table takes no children: rows come from the source, not from the node tree.
int Table::capacity() const { return 0; }

// A table wants all the room it is offered.
Size Table::wants(Size room) const { return room; }

// Makes `given` (clamped to the last row) the current row and sends a `ScrollEvent`. Setting the
// current row pulls the view to it if it has fallen off the screen. The height comes from the
// rectangle of the previous frame: only that knows how much the table really got.
void Table::which(std::size_t given) {
    Kept& kept = *kept_;

    if (kept.rows == 0) return;

    const std::size_t want = std::min(given, kept.rows - 1);

    if (kept.which.has_value() && *kept.which == want) return;

    kept.which = want;

    const int high = std::max(area().size.high - (kept.shape.heading ? 1 : 0), 1);
    const std::size_t was = kept.top;

    if (want < kept.top)
        kept.top = want;
    else if (want >= kept.top + static_cast<std::size_t>(high))
        kept.top = want - static_cast<std::size_t>(high) + 1;

    // The rows are fetched again only when the view moved: a cursor that walks inside the visible
    // rows changes only which row is painted as current, and fetching every row at each arrow press
    // costs the source a row's worth of work per row.
    if (kept.top != was) kept.fresh = true;

    (void)trigger(ScrollEvent{});
}

// Scrolls so that `given` (clamped to the last row) is the first visible row, and sends a
// `ScrollEvent` on a change.
void Table::first(std::size_t given) {
    Kept& kept = *kept_;

    const std::size_t want = kept.rows == 0 ? 0 : std::min(given, kept.rows - 1);

    if (kept.top == want) return;

    kept.top = want;
    kept.fresh = true;

    (void)trigger(ScrollEvent{});
}

// Moves the current row by `rows`, and the view follows it. A negative number goes up, the same
// contract as in `Page::step`, so there are not two names for one motion.
void Table::step(int rows) {
    Kept& kept = *kept_;

    if (kept.rows == 0 || rows == 0) return;

    const auto at = static_cast<std::ptrdiff_t>(kept.which.value_or(0));
    const auto much = static_cast<std::ptrdiff_t>(kept.rows) - 1;

    which(static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(at + rows, 0, much)));
}

// Moves the current row by whole screens of rows.
void Table::page(int pages) {
    const int high = std::max(area().size.high - (kept_->shape.heading ? 1 : 0), 1);

    step(pages * high);
}

// Makes the last row current.
void Table::end() {
    if (kept_->rows > 0) which(kept_->rows - 1);
}



namespace {

// The column widths for a given window width. A column with a positive `wide` takes its own, and
// those with zero share the rest equally. When space runs out, we cut from the right: a panel does
// not scroll horizontally, it clips, and neither do we.
std::vector<int> parted(const std::vector<TableColumn>& columns, int wide) {
    std::vector<int> made(columns.size(), 0);

    if (columns.empty() || wide <= 0) return made;

    const int between = spacing * static_cast<int>(columns.size() - 1);

    int fixed = 0;
    int loose = 0;

    for (const TableColumn& one : columns) {
        if (one.wide > 0)
            fixed += one.wide;
        else
            ++loose;
    }

    int rest = wide - between - fixed;

    for (std::size_t at = 0; at < columns.size(); ++at) {
        if (columns[at].wide > 0) {
            made[at] = columns[at].wide;

            continue;
        }

        // A flexible column gets at least one cell. Otherwise a fixed-width column that does not
        // fit in the window by itself would take everything and the file name would vanish
        // entirely, and in a panel the name is the content, not the size.
        const int part = loose > 0 ? std::max(rest / loose, 1) : 0;

        made[at] = part;

        rest -= part;
        --loose;
    }

    // Cutting from the right when the sum still does not fit. The gap counts only between columns,
    // not after the last one; otherwise the last column would lose a cell nobody occupies, and a
    // right-aligned number would stand one place too early.
    int much = 0;

    for (std::size_t at = 0; at < made.size(); ++at) {
        const int gap = at + 1 < made.size() ? spacing : 0;

        if (much >= wide) {
            made[at] = 0;

            continue;
        }

        made[at] = std::min(made[at], wide - much);
        much += made[at] + gap;
    }

    return made;
}

// The text padded to the column width according to the justification. It is cut with `cut` from
// `eliding.hpp`, that is by columns and character widths, not by bytes.
std::string ranged(const std::string& what, int wide, Justify justify) {
    if (wide <= 0) return {};

    const std::string got = cut(what, wide);
    const int much = columns(got, got.size());
    const int rest = std::max(wide - much, 0);

    if (rest == 0) return got;

    if (justify == Justify::right) return std::string(static_cast<std::size_t>(rest), ' ') + got;

    if (justify == Justify::center) {
        const int left = rest / 2;

        return std::string(static_cast<std::size_t>(left), ' ') + got +
               std::string(static_cast<std::size_t>(rest - left), ' ');
    }

    return got + std::string(static_cast<std::size_t>(rest), ' ');
}

}

// Computes the column widths and fetches the visible rows from the source, only when the allotted
// size or the position has changed. The same rule as in `Page::spread`, for the same reason.
void Table::spread(Size given) {
    Kept& kept = *kept_;

    if (!kept.fresh && kept.sized == given) return;

    kept.sized = given;
    kept.fresh = false;
    kept.shown.clear();

    const int aside = kept.shape.scrollbar && given.wide > 0 ? 1 : 0;
    const int high = given.high - (kept.shape.heading ? 1 : 0);

    kept.spread = parted(kept.columns, given.wide - aside);

    if (!kept.source || given.wide <= 0 || high <= 0) return;

    const std::size_t want = static_cast<std::size_t>(high);

    while (kept.shown.size() < want) {
        const std::vector<TableRow> got = kept.source(kept.top + kept.shown.size(), batch);

        if (got.empty()) break;

        for (const TableRow& one : got) {
            if (kept.shown.size() >= want) break;

            kept.shown.push_back(one);
        }
    }
}

// Draws the heading, the visible rows in their roles (current, marked, both) and the scrollbar.
View Table::paint(Size given, Area seen) {
    Kept& kept = *kept_;

    View made{seen.size};

    made.clear({{0, 0}, seen.size});

    // The selection is trimmed to the number of rows here, because the set has no pointer to the
    // table and no way to learn of the change: the same way the history trims its list and the
    // page clears its cursor for walking through tags.
    chosen_.keep(kept.rows);

    // Rows may have vanished behind the table's back; we clamp here, because everything is
    // recomputed here anyway.
    if (kept.which.has_value() && *kept.which >= kept.rows)
        kept.which = kept.rows > 0 ? std::optional<std::size_t>{kept.rows - 1}
                                   : std::optional<std::size_t>{};

    if (kept.rows > 0 && kept.top >= kept.rows) {
        kept.top = kept.rows - 1;
        kept.fresh = true;
    }

    spread(given);

    const Skin& how = skin();

    const int aside = kept.shape.scrollbar && given.wide > 0 ? 1 : 0;
    const int wide = given.wide - aside;
    const int head = kept.shape.heading ? 1 : 0;

    // The heading. Column titles stand in the same widths as the content, so a column and its
    // title never drift apart.
    if (head != 0 && wide > 0) {
        const Style look = how.style(role() + ".head");

        made.fill({{-seen.at.x, -seen.at.y}, {wide, 1}}, look);

        int at = -seen.at.x;

        for (std::size_t one = 0; one < kept.columns.size() && one < kept.spread.size(); ++one) {
            const int room = kept.spread[one];

            if (room > 0)
                made.put({at, -seen.at.y}, ranged(kept.columns[one].title, room,
                                                     kept.columns[one].justify),
                            look);

            at += room + spacing;
        }
    }

    for (std::size_t row = 0; row < kept.shown.size(); ++row) {
        const std::size_t number = kept.top + row;
        const int y = head + static_cast<int>(row) - seen.at.y;

        const bool here = kept.which.has_value() && *kept.which == number;
        const bool taken = chosen().contains(number);

        const Style look = how.style(here && taken ? role() + ".mark.focus"
                                          : here       ? role() + ".focus"
                                          : taken      ? role() + ".mark"
                                                       : role());

        made.fill({{-seen.at.x, y}, {wide, 1}}, look);

        int at = -seen.at.x;

        for (std::size_t one = 0; one < kept.spread.size(); ++one) {
            int room = kept.spread[one];

            if (room <= 0) {
                at += room + spacing;

                continue;
            }

            const TableCell& cell = one < kept.shown[row].size() ? kept.shown[row][one] : TableCell{};

            // A spanning cell takes the rest of the row: its own column, and every column after it
            // that has room, with the gaps between them.
            if (cell.spans)
                for (std::size_t next = one + 1; next < kept.spread.size(); ++next)
                    if (kept.spread[next] > 0) room += spacing + kept.spread[next];

            // The cell's role is laid over the role of the row's state, so the color of a file
            // type does not cancel the highlight: a marked row still turns yellow as a whole.
            const Style look_one =
                cell.role.empty() ? look : Style::merge(look, how.style(cell.role));

            const Justify justify =
                one < kept.columns.size() ? kept.columns[one].justify : Justify::left;

            made.put({at, y}, ranged(cell.text, room, justify), look_one);

            if (cell.spans) break;

            at += room + spacing;
        }
    }

    // The bar only when there are more rows than can be seen; its column stays reserved, so the
    // table's columns do not jump when the list changes.
    if (aside != 0 && given.high > 0 && kept.rows > static_cast<std::size_t>(std::max(given.high - head, 0))) {
        const Scrollbar bar{.at = Scrollbar::edge(given, seen, true),
                            .much = given.high,
                            .shift = kept.top,
                            .whole = kept.rows,
                            .window = static_cast<std::size_t>(std::max(given.high - head, 1)),
                            .skin = how,
                            .role = kept.scrollbar_role};

        bar.draw(made);

        Track mine = bar.track();

        mine.at = Scrollbar::edge(given, {{0, 0}, given}, true);

        kept.grip.laid(mine);
    } else {
        kept.grip.laid(std::nullopt);
    }

    return made;
}

// Arrows, page keys, Home and End. Enter and letters go up the tree: "a row was chosen" is the
// application's business, because in a panel Enter enters a directory, and in a dialog it presses
// the default button.
bool Table::handle(KeyEvent& what) {
    Kept& kept = *kept_;

    if (what.mods().ctrl || what.mods().alt) return false;

    if (kept.rows == 0) return false;

    switch (what.key().code()) {
        case Code::up: step(-1); return true;

        case Code::down: step(1); return true;

        case Code::pageup: page(-1); return true;

        case Code::pagedown: page(1); return true;

        case Code::home: which(0); return true;

        case Code::end: end(); return true;

        // The table does not take Insert. Marking and stepping one row down, a habit from Midnight
        // Commander's panel, would be the only key in the whole library at which `handle` changes
        // data instead of just moving the cursor, and the caller could not undo it: intercepting
        // it higher up does not work, because the table lies closer to the focus. Whoever wants
        // that habit hangs on<KeyEvent> on the table and marks rows through `chosen()` itself.

        default: break;
    }

    return false;
}

// The wheel moves only the view and leaves the current row alone, the same as in the list and in
// Midnight Commander's panel. At the end of the range we return false, so that a scroller higher
// up gets the motion; so does the tilted wheel, since the table does not scroll sideways.
bool Table::handle(WheelEvent& what) {
    if (what.axis() != Axis::vertical) return false;

    Kept& kept = *kept_;

    const int high = std::max(area().size.high - (kept.shape.heading ? 1 : 0), 1);

    const auto much = static_cast<std::ptrdiff_t>(kept.rows);
    const auto was = static_cast<std::ptrdiff_t>(kept.top);

    const std::ptrdiff_t want = std::clamp<std::ptrdiff_t>(
        was - what.steps() * stepping, 0,
        std::max<std::ptrdiff_t>(much - static_cast<std::ptrdiff_t>(high), 0));

    if (want == was) return false;

    first(static_cast<std::size_t>(want));

    return true;
}

// The data row under a screen cell; the heading and the empty space below the last row give
// nothing.
std::optional<std::size_t> Table::row_under(Spot where) const {
    const Kept& kept = *kept_;

    const int head = kept.shape.heading ? 1 : 0;
    const int y = local(where).y - head;

    if (y < 0) return std::nullopt;

    const std::size_t at = kept.top + static_cast<std::size_t>(y);

    if (at >= kept.rows) return std::nullopt;

    return at;
}

// The column under a heading cell, by the widths from the last frame, with the gap between columns
// counted to the left column. Outside the heading, on the bar and past the last column: nothing.
std::optional<std::size_t> Table::column_under(Spot where) const {
    const Kept& kept = *kept_;

    const Spot here = local(where);

    if (!kept.shape.heading || here.y != 0 || here.x < 0 || kept.grip.over(here)) return std::nullopt;

    int at = 0;

    for (std::size_t one = 0; one < kept.spread.size() && one < kept.columns.size(); ++one) {
        const int room = kept.spread[one];

        if (room > 0 && here.x < at + room + spacing) return one;

        at += room + spacing;
    }

    return std::nullopt;
}

// The mouse on the scrollbar: an arrow moves one row, the track one page, the thumb is dragged.
// Only the view moves, as with the wheel, and the current row does not: the same as in MC and in
// the scroller.
bool Table::handle(MouseEvent& what) {
    Kept& kept = *kept_;

    const Asked asked = kept.grip.mouse(what, local(what.where()), *this);

    const auto high = static_cast<std::size_t>(std::max(area().size.high - (kept.shape.heading ? 1 : 0), 1));
    const std::size_t most = kept.rows > high ? kept.rows - high : 0;

    switch (asked.part) {
        case Part::none: return false;
        case Part::back: first(kept.top > 0 ? kept.top - 1 : 0); break;
        case Part::forth: first(std::min(kept.top + 1, most)); break;
        case Part::before: first(kept.top > high ? kept.top - high : 0); break;
        case Part::after: first(std::min(kept.top + high, most)); break;
        case Part::thumb:
            if (asked.to.has_value()) first(std::min(*asked.to, most));
            break;
    }

    return true;
}

// A click sets the current row and takes the focus. A click on the bar has already done its work at
// the press, so here it does not set the row that happens to be at that height.
bool Table::handle(ClickEvent& what) {
    if (kept_->grip.over(local(what.where()))) return true;

    if (root() != nullptr) focus();

    if (const std::optional<std::size_t> at = row_under(what.where())) which(*at);

    return true;
}

// A double click on a row is passed up, for the same reason as Enter: the application knows what
// choosing a row means. The row becomes current (the click before the double click already set it,
// but the list may have moved in between). A double click on the heading or below the last row
// means nothing and goes no further.
bool Table::handle(DoubleClickEvent& what) {
    if (kept_->grip.over(local(what.where()))) return true;

    const std::optional<std::size_t> at = row_under(what.where());

    if (!at) return true;

    which(*at);

    return false;
}

}
