#include "talcite/table.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "talcite/skin.hpp"

#include "eliding.hpp"
#include "scrollbar.hpp"

namespace talcite::ui {

namespace {

// Whether two rows show the same: their cells' text, role, span, elision and marks.
bool same(const TableRow& one, const TableRow& two) {
    return std::equal(one.begin(), one.end(), two.begin(), two.end(), [](const TableCell& a, const TableCell& b) {
        return a.text == b.text && a.role == b.role && a.spans == b.spans && a.elide == b.elide && a.marks == b.marks;
    });
}

// How many rows are taken from the source at once. The same number and the same reason as in the
// scroll: asking for exactly as many as the window is high would cost a call per frame.
constexpr std::size_t batch = 64;

// The gap between columns. Without it a name would touch the size and the two could not be told
// apart.
constexpr int spacing = 1;

// How many rows the table shows in `seen` under its heading; nothing before the first frame or when
// there is no room for a row, since then no one knows yet where the view should stand.
std::optional<std::size_t> rows_in(Area seen, const TableShape& shape) {
    const int high = seen.size.high - (shape.heading ? 1 : 0);

    if (high <= 0) return std::nullopt;

    return static_cast<std::size_t>(high);
}

}

// The table's state, kept behind a pointer so the public header stays small.
struct Table::Kept {
    TableSource source;
    std::size_t rows = 0;

    std::vector<TableColumn> columns;

    std::size_t top = 0;
    std::optional<std::size_t> which;

    // The cursor moved before the table knew its height: the view goes to it on the first frame
    // that has room for rows (`spread`). Before, the move assumed one row of room, and a cursor put
    // on the second row of a fresh window scrolled the first one out of a list that had room for
    // all of them.
    bool pending = false;

    // Moves the view the least that puts the current row among the `high` visible rows; says
    // whether it moved.
    bool pulled(std::size_t high) {
        if (!which || high == 0) return false;

        const std::size_t was = top;

        if (*which < top)
            top = *which;
        else if (*which >= top + high)
            top = *which - high + 1;

        return top != was;
    }

    // What is visible. Recomputed when, and only when, the allotted size or the position changes;
    // otherwise every frame would ask the source again, and with animated neighbors that happens
    // ten times a second.
    std::vector<TableRow> shown;
    bool fresh = true;
    Size sized;

    // The first row of `shown` when it was taken: a window taken again at the same place and size
    // repaints only the rows that came out different (`spread`).
    std::size_t taken_top = 0;

    // The computed column widths for the current window width.
    std::vector<int> spread;

    // The selection (`chosen()`) is kept by row number in a bit vector, not a set: for a hundred
    // thousand rows that is twelve kilobytes, and "select all" is as common in a panel as "select
    // three", so a set would often be larger than the vector.

    TableShape shape;

    std::string scrollbar_role = "talcite.scroll";

    // The scrollbar as the layout settled it, for the mouse and for painting.
    Grip grip;
    std::optional<Track> bar;
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
    kept.pending = false;

    chosen().clear();

    kept.shown.clear();
    kept.fresh = true;

    Node::changed();
}

// Follows rows replaced in the middle of the source: the rows [at, at + gone) give way to `came`
// new ones. The current row, the first visible row and the selection stay with the rows they stood
// on, so a list whose part opens or folds keeps its place on the screen. A current row among the
// replaced ones goes to `at` (or to the last row), and an empty table that gets rows stands on the
// first; only then a `ScrollEvent` is sent, because only then the cursor stands on another row than
// before. The visible rows are fetched again on the next frame.
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

    // The current row stays in view: the change may have pushed it below the window. Before the
    // first frame that waits for the height.
    if (const std::optional<std::size_t> high = rows_in(area(), kept.shape))
        (void)kept.pulled(*high);
    else
        kept.pending = true;

    chosen().moved(at, gone, came);
    chosen().keep(rows);

    kept.shown.clear();
    kept.fresh = true;

    Node::changed();

    if (other) (void)trigger(ScrollEvent{});
}

// Whether a source has been set.
bool Table::sourced() const { return static_cast<bool>(kept_->source); }

// The next frame asks the source again for the visible rows, and repaints those that came out
// different (`spread`): a spinner in a few rows does not repaint the whole table. The selection is
// not cleared: after re-sorting, row numbers mean something else than before, but only the caller
// knows whether that happened, so it calls `chosen().clear()` when it sees fit.
void Table::refresh() { kept_->fresh = true; }

// Sets the columns.
void Table::columns(std::vector<TableColumn> given) {
    kept_->columns = std::move(given);
    kept_->fresh = true;

    Node::changed();
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

    Node::changed();
}

// Whether the table keeps a column for the scrollbar.
bool Table::scrollbar() const { return kept_->shape.scrollbar; }

// Turns the scrollbar column on or off.
void Table::scrollbar(bool given) {
    if (kept_->shape.scrollbar == given) return;

    kept_->shape.scrollbar = given;
    kept_->fresh = true;

    Node::changed();
}

// The skin role of the scrollbar.
const std::string& Table::scrollbar_role() const { return kept_->scrollbar_role; }

// Sets the skin role of the scrollbar.
void Table::scrollbar_role(std::string given) {
    kept_->scrollbar_role = std::move(given);

    Node::changed();
}

// A table always takes the focus.
bool Table::focusable() const { return true; }

// A table takes no children: rows come from the source, not from the node tree.
int Table::capacity() const { return 0; }

// A table wants all the room it is offered.
Size Table::wants(Size room) const { return room; }

// Makes `given` (clamped to the last row) the current row and sends a `ScrollEvent`. Setting the
// current row pulls the view to it if it has fallen off the screen. The height comes from the
// rectangle of the previous frame: only that knows how much the table really got. Before the first
// frame the view waits for it.
//
// The event says how the move came (`ScrollEvent::by_hand`): the table's own keys, wheel and mouse
// pass `Moved::by_hand`, a call from the program leaves the default. Only the table knows it, and a
// listener needs it: a panel's quick search ends when the person moves in the table, not when the
// search itself puts the cursor on a match or a refresh keeps it on its entry. Before, the panel
// switched its search off around its own moves, which hid the answer in the order of the calls.
// An application that carries out a person's key on the table itself (marking with Shift) may say
// so too.
void Table::which(std::size_t given, Moved how) {
    Kept& kept = *kept_;

    if (kept.rows == 0) return;

    const std::size_t want = std::min(given, kept.rows - 1);

    if (kept.which.has_value() && *kept.which == want) return;

    const std::optional<std::size_t> before = kept.which;

    kept.which = want;

    // The rows are fetched again only when the view moved: a cursor that walks inside the visible
    // rows changes only which row is painted as current, and fetching every row at each arrow press
    // costs the source a row's worth of work per row; then only those two rows are painted again. A
    // view that waits for the height is moved by the first frame with room for rows, which fetches
    // them anyway.
    if (const std::optional<std::size_t> high = rows_in(area(), kept.shape)) {
        if (kept.pulled(*high)) {
            kept.fresh = true;

            Node::changed();
        } else {
            const int head = kept.shape.heading ? 1 : 0;

            for (const std::optional<std::size_t> one : {before, kept.which})
                if (one.has_value() && *one >= kept.top)
                    Node::changed({{0, head + static_cast<int>(*one - kept.top)}, {area().size.wide, 1}});
        }
    } else {
        kept.pending = true;

        Node::changed();
    }

    (void)trigger(ScrollEvent{how});
}

// Scrolls so that `given` (clamped to the last row) is the first visible row, and sends a
// `ScrollEvent` on a change, saying how the move came, as at `which`.
void Table::first(std::size_t given, Moved how) {
    Kept& kept = *kept_;

    const std::size_t want = kept.rows == 0 ? 0 : std::min(given, kept.rows - 1);

    // A view asked for by name wins over one still waiting to follow the cursor.
    kept.pending = false;

    if (kept.top == want) return;

    kept.top = want;
    kept.fresh = true;

    Node::changed();

    (void)trigger(ScrollEvent{how});
}

// Moves the current row by `rows`, and the view follows it. A negative number goes up, the same
// contract as in `Page::step`, so there are not two names for one motion.
void Table::step(int rows, Moved how) {
    Kept& kept = *kept_;

    if (kept.rows == 0 || rows == 0) return;

    const auto at = static_cast<std::ptrdiff_t>(kept.which.value_or(0));
    const auto much = static_cast<std::ptrdiff_t>(kept.rows) - 1;

    which(static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(at + rows, 0, much)), how);
}

// Moves the current row by whole screens of rows.
void Table::page(int pages, Moved how) {
    const int high = std::max(area().size.high - (kept_->shape.heading ? 1 : 0), 1);

    step(pages * high, how);
}

// Makes the last row current.
void Table::end(Moved how) {
    if (kept_->rows > 0) which(kept_->rows - 1, how);
}



namespace {

// The column widths for a given window width. A column with a positive `wide` takes its own, and
// those with zero share the rest by their `share`, as the stretch factors of a layout in Qt: two
// columns of share 2 and 3 take two fifths and three fifths of it, and equal shares take equal
// parts. A share below one counts as one. When space runs out, we cut from the right: a panel does
// not scroll horizontally, it clips, and neither do we.
std::vector<int> parted(const std::vector<TableColumn>& columns, int wide) {
    std::vector<int> made(columns.size(), 0);

    if (columns.empty() || wide <= 0) return made;

    const int between = spacing * static_cast<int>(columns.size() - 1);

    const auto weight = [](const TableColumn& one) { return std::max(one.share, 1); };

    int fixed = 0;
    int loose = 0;

    for (const TableColumn& one : columns) {
        if (one.wide > 0)
            fixed += one.wide;
        else
            loose += weight(one);
    }

    int rest = wide - between - fixed;

    for (std::size_t at = 0; at < columns.size(); ++at) {
        if (columns[at].wide > 0) {
            made[at] = columns[at].wide;

            continue;
        }

        // A flexible column gets at least one cell. Otherwise a fixed-width column that does not
        // fit in the window by itself would take everything and the file name would vanish
        // entirely, and in a panel the name is the content, not the size. Each column takes its
        // part of what is left after the ones before it, so the last one gets what rounding left.
        const int share = weight(columns[at]);
        const int part = loose > 0 ? std::max(static_cast<int>(static_cast<long long>(rest) * share / loose), 1) : 0;

        made[at] = part;

        rest -= part;
        loose -= share;
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

// The marks of a cell laid over it as it was drawn at `at` in `room` columns: the letters stay, and
// each mark's role is merged over the cell's look, as a label does with its marks (`Span`), so a
// meter of colored segments or one emphasized word stands in a single cell. The cell is traced the
// way it was drawn, elided and then cut and justified as `placed` does, so a mark follows its
// bytes wherever eliding moved them, and a mark over the ellipsis covers nothing. A mark with an
// address lays its hyperlink too. A cell holds one line, so only marks of line 0 count.
void marked(Canvas& made, const TableCell& cell, Spot at, int room, Justify justify, const Style& look, const Skin& how) {
    const Traced whole = elided(traced(cell.text, 0), room, cell.elide, how.glyph("talcite.elide"));
    const auto [bytes, much] = fitting(whole.text, room);

    Traced kept;

    kept.text = whole.text.substr(0, bytes);
    kept.bytes.assign(whole.bytes.begin(), whole.bytes.begin() + static_cast<std::ptrdiff_t>(bytes));

    const int rest = std::max(room - much, 0);
    const int x = at.x + (justify == Justify::right ? rest : justify == Justify::center ? rest / 2 : 0);

    for (const Mark& one : cell.marks) {
        const Style over = Style::merge(look, how.style(one.role));
        const Hyperlink target = one.address.empty() ? Hyperlink{} : hyperlink(one.address);

        walked(kept, x, [&](int column, int much, char32_t letter, std::size_t byte) {
            if (!covers(one, 0, byte)) return;

            made.put({column, at.y}, letter, over);

            if (target.mark != 0) made.link({{column, at.y}, {much, 1}}, target);
        });
    }
}

// The text in a column `wide` cells wide at `at`, justified: the head that fits (`fitting`, by
// character widths, not by bytes) and spaces in the same look on the side the justification leaves
// free. Painted straight onto the canvas, without composing a string of the padded text for every
// cell of every row. A tab is not expanded here as in a viewer's line (`Page`): in a cell it is a
// stray character of a name, shown as its picture in one cell (`shown`), and the justification
// counts it as the painting does.
void placed(Canvas& onto, Spot at, std::string_view what, int wide, Justify justify, const Style& look) {
    if (wide <= 0) return;

    const auto [bytes, much] = fitting(what, wide);
    const std::string_view got = what.substr(0, bytes);

    const int rest = std::max(wide - much, 0);
    const int left = justify == Justify::right ? rest : justify == Justify::center ? rest / 2 : 0;

    if (left > 0) onto.fill({at, {left, 1}}, look);

    onto.put({at.x + left, at.y}, got, look);

    if (rest > left) onto.fill({{at.x + left + much, at.y}, {rest - left, 1}}, look);
}

}

// Computes the column widths and fetches the visible rows from the source, only when the allotted
// size or the position has changed. The same rule as in `Page::spread`, for the same reason.
void Table::spread(Size given) {
    Kept& kept = *kept_;

    if (!kept.fresh && kept.sized == given) return;

    // The window as it was: taken again at the same place and size, only the rows that differ are
    // a change; otherwise the whole table is.
    const bool again = kept.sized == given && kept.taken_top == kept.top;
    const std::vector<TableRow> before = std::move(kept.shown);

    kept.sized = given;
    kept.fresh = false;
    kept.shown.clear();
    kept.taken_top = kept.top;

    const int aside = kept.shape.scrollbar && given.wide > 0 ? 1 : 0;
    const int high = given.high - (kept.shape.heading ? 1 : 0);

    kept.spread = parted(kept.columns, given.wide - aside);

    if (!kept.source || given.wide <= 0 || high <= 0) {
        if (!before.empty()) Node::changed();

        return;
    }

    const std::size_t want = static_cast<std::size_t>(high);

    // The cursor moved before this first frame with room for rows: now the view knows where to go.
    if (kept.pending) {
        kept.pending = false;
        (void)kept.pulled(want);
    }

    while (kept.shown.size() < want) {
        std::vector<TableRow> got = kept.source(kept.top + kept.shown.size(), batch);

        if (got.empty()) break;

        for (TableRow& one : got) {
            if (kept.shown.size() >= want) break;

            kept.shown.push_back(std::move(one));
        }
    }

    if (!again || before.size() != kept.shown.size()) {
        Node::changed();

        return;
    }

    const int head = kept.shape.heading ? 1 : 0;

    for (std::size_t row = 0; row < kept.shown.size(); ++row)
        if (!same(before[row], kept.shown[row])) Node::changed({{0, head + static_cast<int>(row)}, {given.wide, 1}});
}

// Settles what the table shows before it is painted: the selection and the cursor within the rows,
// the top row, the visible rows themselves (`spread`) and the scrollbar.
void Table::layout(Size given) {
    Kept& kept = *kept_;

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

    // The bar only when there are more rows than can be seen; its column stays reserved, so the
    // table's columns do not jump when the list changes. It runs beside the rows only: the heading
    // does not scroll, so the bar starts under it.
    const int aside = kept.shape.scrollbar && given.wide > 0 ? 1 : 0;
    const int head = kept.shape.heading ? 1 : 0;

    if (aside != 0 && given.high > head && kept.rows > static_cast<std::size_t>(std::max(given.high - head, 0))) {
        const Spot top = Scrollbar::edge(given, {{0, 0}, given}, true);

        kept.bar = Track{{top.x, top.y + head},
                         true,
                         given.high - head,
                         kept.top,
                         kept.rows,
                         static_cast<std::size_t>(std::max(given.high - head, 1))};
    } else {
        kept.bar.reset();
    }

    kept.grip.laid(kept.bar);
}

// Draws the heading, the visible rows in their roles (current, marked, both) and the scrollbar.
void Table::paint(Canvas& onto) const {
    const Kept& kept = *kept_;

    const Size given = onto.size();

    const Skin& how = skin();

    const int aside = kept.shape.scrollbar && given.wide > 0 ? 1 : 0;
    const int wide = given.wide - aside;
    const int head = kept.shape.heading ? 1 : 0;

    // The rows that may be painted: a frame that repaints two rows of the table cuts the canvas to
    // them, and the rest is not composed at all.
    const Area seen = onto.seen();
    const auto visible = [&seen](int y) { return y >= seen.at.y && y < seen.at.y + seen.size.high; };

    // The heading. Column titles stand in the same widths as the content, so a column and its
    // title never drift apart.
    if (head != 0 && wide > 0 && visible(0)) {
        const Style look = how.style(role() + ".head");

        onto.fill({{0, 0}, {wide, 1}}, look);

        int at = 0;

        for (std::size_t one = 0; one < kept.columns.size() && one < kept.spread.size(); ++one) {
            const int room = kept.spread[one];

            if (room > 0)
                placed(onto, {at, 0}, kept.columns[one].title, room, kept.columns[one].justify, look);

            at += room + spacing;
        }
    }

    // The looks of a row's states and the elision mark are asked of the skin once a frame, not for
    // every row and cell; a missing cell is an empty one, shown by reference, not copied.
    const Style plain = how.style(role());
    const Style focused = how.style(role() + ".focus");
    const Style marked_look = how.style(role() + ".mark");
    const Style both = how.style(role() + ".mark.focus");
    const char32_t mark = how.glyph("talcite.elide");

    static const TableCell blank{};

    for (std::size_t row = 0; row < kept.shown.size(); ++row) {
        const std::size_t number = kept.top + row;
        const int y = head + static_cast<int>(row);

        if (!visible(y)) continue;

        const bool here = kept.which.has_value() && *kept.which == number;
        const bool taken = chosen().contains(number);

        const Style look = here && taken ? both : here ? focused : taken ? marked_look : plain;

        onto.fill({{0, y}, {wide, 1}}, look);

        int at = 0;

        for (std::size_t one = 0; one < kept.spread.size(); ++one) {
            const TableCell& cell = one < kept.shown[row].size() ? kept.shown[row][one] : blank;

            // A spanning cell takes the rest of the row: from where its column starts to the edge
            // of the content, whatever room the columns after it were given, its own included.
            const int room = cell.spans ? wide - at : kept.spread[one];

            if (room <= 0) {
                if (cell.spans) break;

                at += room + spacing;

                continue;
            }

            // The cell's role is laid over the role of the row's state, so the color of a file
            // type does not cancel the highlight: a marked row still turns yellow as a whole.
            const Style look_one =
                cell.role.empty() ? look : Style::merge(look, how.style(cell.role));

            const Justify justify =
                one < kept.columns.size() ? kept.columns[one].justify : Justify::left;

            // A cell that asks for it is elided with the skin's mark, at the end it names (a path
            // keeps its file name with `Elide::left`); otherwise it is cut at the right edge.
            const std::string cutting = cell.elide == Elide::none ? std::string{} : elided(cell.text, room, cell.elide, mark);
            const std::string_view shown = cell.elide == Elide::none ? std::string_view{cell.text} : std::string_view{cutting};

            placed(onto, {at, y}, shown, room, justify, look_one);

            if (!cell.marks.empty()) marked(onto, cell, {at, y}, room, justify, look_one, how);

            if (cell.spans) break;

            at += room + spacing;
        }
    }

    if (!kept.bar.has_value()) return;

    const Scrollbar bar{.track = *kept.bar,
                        .focused = Scrollbar::focused_in(*this),
                        .skin = how,
                        .role = kept.scrollbar_role};

    bar.draw(onto);
}

// Arrows, page keys, Home and End. Enter and letters go up the tree: "a row was chosen" is the
// application's business, because in a panel Enter enters a directory, and in a dialog it presses
// the default button. At its ends the table keeps its keys, for the reason given at `List::handle`.
bool Table::handle(KeyEvent& what) {
    Kept& kept = *kept_;

    if (what.mods().ctrl || what.mods().alt) return false;

    if (kept.rows == 0) return false;

    switch (what.key().code()) {
        case Code::up: step(-1, Moved::by_hand); return true;

        case Code::down: step(1, Moved::by_hand); return true;

        case Code::pageup: page(-1, Moved::by_hand); return true;

        case Code::pagedown: page(1, Moved::by_hand); return true;

        case Code::home: which(0, Moved::by_hand); return true;

        case Code::end: end(Moved::by_hand); return true;

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
        was - wheeled(what.steps()), 0,
        std::max<std::ptrdiff_t>(much - static_cast<std::ptrdiff_t>(high), 0));

    if (want == was) return false;

    first(static_cast<std::size_t>(want), Moved::by_hand);

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
// counted to the left column. Outside the heading, in the bar's column (which the bar leaves clear
// beside the heading) and past the last column: nothing.
std::optional<std::size_t> Table::column_under(Spot where) const {
    const Kept& kept = *kept_;

    const Spot here = local(where);
    const int aside = kept.shape.scrollbar ? 1 : 0;

    if (!kept.shape.heading || here.y != 0 || here.x < 0 || here.x >= area().size.wide - aside) return std::nullopt;

    int at = 0;

    for (std::size_t one = 0; one < kept.spread.size() && one < kept.columns.size(); ++one) {
        const int room = kept.spread[one];

        if (room > 0 && here.x < at + room + spacing) return one;

        at += room + spacing;
    }

    return std::nullopt;
}

// The mouse on the scrollbar: the track moves one page, the thumb is dragged.
// Only the view moves, as with the wheel, and the current row does not: the same as in MC and in
// the scroller.
bool Table::handle(MouseEvent& what) {
    Kept& kept = *kept_;

    const Asked asked = kept.grip.mouse(what, local(what.where()), *this);

    const auto high = static_cast<std::size_t>(std::max(area().size.high - (kept.shape.heading ? 1 : 0), 1));
    const std::size_t most = kept.rows > high ? kept.rows - high : 0;

    switch (asked.part) {
        case Part::none: return false;
        case Part::before: first(kept.top > high ? kept.top - high : 0, Moved::by_hand); break;
        case Part::after: first(std::min(kept.top + high, most), Moved::by_hand); break;
        case Part::thumb:
            if (asked.to.has_value()) first(std::min(*asked.to, most), Moved::by_hand);
            break;
    }

    return true;
}

// A click sets the current row and takes the focus. A click on the bar has already done its work at
// the press, so here it does not set the row that happens to be at that height.
bool Table::handle(ClickEvent& what) {
    if (kept_->grip.over(local(what.where()))) return true;

    if (root() != nullptr) focus();

    if (const std::optional<std::size_t> at = row_under(what.where())) which(*at, Moved::by_hand);

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

    which(*at, Moved::by_hand);

    return false;
}

}
