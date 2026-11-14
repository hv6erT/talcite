#include "tal/pages.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "tal/key.hpp"
#include "tal/text.hpp"

#include "controlling.hpp"
#include "spooling.hpp"

// A page draws a byte document a screen at a time and never holds a whole line. A row of the screen
// is put together from the bytes it shows, read from the spool in pieces from the row's start until
// the row is full or its line ends; a line of a gigabyte costs a frame what a line of eighty
// characters does. Only stepping past a long line without wrapping, a jump into its middle and End
// walk it, and the spool does that window by window, remembering how far it got.
//
// Two memories serve the walks inside long lines, kept for the lines around the view only: where
// rows of the screen start in a wrapped line (every few dozen rows), and at which byte every few
// hundred columns start (for the view moved sideways).
namespace tal {

using spooling::Offset;
using spooling::Spool;

namespace {

// How many bytes are read from the spool at once while a row is put together.
constexpr std::size_t reading = 4096;

// The longest cluster in bytes, with room to spare: a piece is read again before fewer bytes than
// this are left in it, so that a cluster never breaks at the end of a piece.
constexpr std::size_t margin = 128;

// Row starts are remembered every this many rows of a wrapped line, at first.
constexpr std::size_t sampling = 64;

// Column starts are remembered every this many columns of a line, at first.
constexpr std::size_t columning = 256;

// Past this many samples in one line every other goes and the spacing doubles, so that a line of a
// gigabyte keeps a few thousand of them.
constexpr std::size_t samples = 4096;

// How many lines may have their rows and columns remembered before those far from the view go.
constexpr std::size_t lining = 256;

// The scrollbar counts lines when they are known and no longer than this on average, bytes
// otherwise: in a document of a few very long lines the thumb would stand still along each.
constexpr Offset short_line = 1024;

// How many rows a step of the wheel moves the page, as in Midnight Commander.
constexpr int wheeled = 3;

// The piece of a line on one row of the screen: its text and, for every column, the byte (from the
// line's start) of the character standing there. `at` and `end` are where the row starts and
// where its last character ends in the document, `next` where the row after it starts once known,
// `from` and `upto` the same in the line.
struct Cut {
    std::string text;
    std::vector<std::size_t> bytes;
    std::size_t line = 0;
    Offset at = 0;
    Offset end = 0;
    Offset next = 0;
    std::size_t from = 0;
    std::size_t upto = 0;
    bool last = false;
};

// Reads a line cluster by cluster from a byte, through the spool in pieces, up to the end of the
// line: a \n, a \r before one, or the end of the document.
class Reader {
public:
    Reader(Spool& spool, Offset at, text::Rendering how) : spool_(spool), at_(at), from_(at), how_(how) {}

    // The byte where the next cluster starts.
    [[nodiscard]] Offset at() const { return at_; }

    // Where the next line starts, once `ending` said that this one ends here.
    [[nodiscard]] Offset beyond() const { return beyond_; }

    // Whether the line ends at the reader; none while a window is on its way.
    [[nodiscard]] std::optional<bool> ending() {
        if (!filled()) return std::nullopt;

        const auto off = static_cast<std::size_t>(at_ - from_);

        if (off >= buffer_.size()) {
            beyond_ = at_;

            return true;
        }

        if (buffer_[off] == '\n') {
            beyond_ = at_ + 1;

            return true;
        }

        if (buffer_[off] != '\r') return false;

        if (off + 1 >= buffer_.size()) {
            beyond_ = at_ + 1;

            return true;
        }

        if (buffer_[off + 1] != '\n') return false;

        beyond_ = at_ + 2;

        return true;
    }

    // The next cluster of the line, which `ending` said goes on; it holds until the next call.
    [[nodiscard]] std::string_view next() {
        const std::string_view all{buffer_};
        const auto off = static_cast<std::size_t>(at_ - from_);
        const std::size_t upto = text::after(all, off, how_);

        at_ = from_ + upto;

        return all.substr(off, upto - off);
    }

private:
    // Whether the piece holds enough from the reader on to tell its next cluster: a margin, a line
    // end, or the end of the document.
    [[nodiscard]] bool enough() const {
        const Offset have = from_ + buffer_.size();

        if (have >= spool_.whole() || at_ + margin <= have) return true;

        return std::string_view{buffer_}.find('\n', static_cast<std::size_t>(at_ - from_)) != std::string_view::npos;
    }

    // Makes sure the piece holds enough from the reader on; false while a window is on its way.
    // What the spool has at hand comes first, so that the head of a file shows before the rest.
    bool filled() {
        if (enough()) return true;

        if (const std::optional<std::string_view> got = spool_.read(at_, reading, 1)) {
            buffer_.assign(*got);
            from_ = at_;

            if (enough()) return true;
        }

        const std::optional<std::string_view> got = spool_.read(at_, reading);

        if (!got) return false;

        buffer_.assign(*got);
        from_ = at_;

        return true;
    }

    Spool& spool_;
    Offset at_ = 0;
    Offset from_ = 0;
    Offset beyond_ = 0;
    text::Rendering how_;
    std::string buffer_;
};

// How a row ended: where it starts, where its last character ends, where the next row starts (the
// next line's start when the row ends its line), and whether a window was missing on the way, when
// the rest means nothing.
struct Walked {
    Offset at = 0;
    Offset end = 0;
    Offset next = 0;
    bool last = false;
    bool waiting = false;
};

// Puts a cut together: the text and the byte of every column, also the second column of a wide
// character and every column of a tab.
struct Builder {
    Cut made;
    Offset line_at = 0;

    // Adds `much` spaces of a tab or of the part of a character cut by the left edge, each pointing
    // at that character.
    void spaced(Offset was, int much) {
        for (int step = 0; step < much; ++step) {
            made.text.push_back(' ');
            made.bytes.push_back(static_cast<std::size_t>(was - line_at));
        }
    }

    // Adds a cluster taking `room` columns.
    void lettered(std::string_view sign, Offset was, int room) {
        made.text += sign;

        for (int step = 0; step < room; ++step) made.bytes.push_back(static_cast<std::size_t>(was - line_at));
    }
};

// Walks a row without building it, for stepping and for the memories.
struct Skipper {
    // Nothing.
    void spaced(Offset was, int much) {
        (void)was;
        (void)much;
    }

    // Nothing.
    void lettered(std::string_view sign, Offset was, int room) {
        (void)sign;
        (void)was;
        (void)room;
    }
};

// The column a tab at the column `much` goes to: the next multiple of `tab`.
std::size_t tabbed(std::size_t much, int tab) {
    const auto step = static_cast<std::size_t>(tab);

    return (much / step + 1) * step;
}

// One row of a wrapped line from the byte `at`, at most `wide` columns. A tab goes to the next
// multiple of `tab` from the row's start, and a character that does not fit goes to the next row;
// a row is never empty unless its line is, so a character wider than the row stands in it alone. A
// line exactly as long as a multiple of the width ends with a full row, not an empty one after it.
template <typename Taker>
Walked wrapped_row(Spool& spool, Offset at, int wide, int tab, text::Rendering how, Taker& taker) {
    Reader reader{spool, at, how};
    Walked made{at, at, at, false, false};
    int much = 0;

    while (true) {
        const std::optional<bool> end = reader.ending();

        if (!end) {
            made.waiting = true;

            return made;
        }

        if (*end) {
            made.end = reader.at();
            made.next = reader.beyond();
            made.last = true;

            return made;
        }

        if (much >= wide) {
            made.end = reader.at();
            made.next = reader.at();

            return made;
        }

        const Offset was = reader.at();
        const std::string_view sign = reader.next();

        if (sign == "\t") {
            const int upto = std::min(static_cast<int>(tabbed(static_cast<std::size_t>(much), tab)), wide);

            taker.spaced(was, upto - much);

            much = upto;

            continue;
        }

        const int room = text::width(sign, how);

        if (much > 0 && much + room > wide) {
            made.end = was;
            made.next = was;

            return made;
        }

        taker.lettered(sign, was, room);

        much += room;
    }
}

// The one row of a line without wrapping: its columns from `left` to `left + wide`. `at` is the
// start of the character in which the column `left` falls and `column` the column where it starts.
// A tab goes to the next multiple of `tab` from the line's start; a tab or a wide character cut by
// the left edge shows its visible part as spaces, and a character that does not fit at the right
// edge is left out. The row ends its line only when the line ends in view.
template <typename Taker>
Walked clipped_row(Spool& spool, Offset at, std::size_t column, std::size_t left, int wide, int tab, text::Rendering how, Taker& taker) {
    Reader reader{spool, at, how};
    Walked made{at, at, at, false, false};
    std::size_t much = column;
    const std::size_t limit = left + static_cast<std::size_t>(wide);

    while (true) {
        const std::optional<bool> end = reader.ending();

        if (!end) {
            made.waiting = true;

            return made;
        }

        if (*end || much >= limit) {
            made.end = reader.at();
            made.last = *end;

            return made;
        }

        const Offset was = reader.at();
        const std::string_view sign = reader.next();
        const std::size_t room = sign == "\t" ? tabbed(much, tab) - much : static_cast<std::size_t>(text::width(sign, how));

        if (sign == "\t" || much < left) {
            const std::size_t from = std::max(much, left);
            const std::size_t upto = std::min(much + room, limit);

            if (upto > from) taker.spaced(was, static_cast<int>(upto - from));

            much += room;

            continue;
        }

        if (much + room > limit) {
            made.end = was;

            return made;
        }

        taker.lettered(sign, was, static_cast<int>(room));

        much += room;
    }
}

// Whether a cut shows the place `where`: a byte from its start up to its end, and on the last row
// of a line also past the end.
bool showing(const Cut& one, Place where) {
    if (one.line != where.line || where.byte < one.from) return false;

    return where.byte < one.upto || one.last;
}

// Where rows of the screen start in one wrapped line, for one width: every `every` rows the row
// and its byte.
struct Rows {
    std::size_t every = sampling;
    std::vector<std::pair<std::size_t, Offset>> starts;
};

// At which byte columns of one line start: every `every` columns the first character starting at
// or past that column (its column and byte).
struct Columns {
    std::size_t every = columning;
    std::vector<std::pair<std::size_t, Offset>> marks;
};

// The order of marks: by start, then by end.
bool sooner(const Stretch& one, const Stretch& two) { return one.from != two.from ? one.from < two.from : one.upto < two.upto; }

// Keeps every other sample of `into` and doubles its spacing, past `samples` of them.
template <typename Samples>
void thinned(Samples& into, std::size_t& every) {
    if (into.size() <= samples) return;

    Samples kept;

    for (std::size_t one = 0; one < into.size(); one += 2) kept.push_back(into[one]);

    into = std::move(kept);
    every *= 2;
}

}

struct Page::Kept {
    explicit Kept(Page& owner) : page(owner) {}

    Page& page;
    std::shared_ptr<Spool> spool;

    // What waits for a window: the end, and the row with this byte.
    bool ending = false;
    std::optional<Offset> sought;

    // The line at the top, where it starts once known, and where its top row starts.
    std::size_t top = 0;
    std::optional<Offset> top_at;
    Offset row_at = 0;

    // Where a row starts: its line, the line's start and the row's; `built` when the row is laid
    // out already and only the next one is sought.
    struct Next {
        std::size_t line = 0;
        Offset line_at = 0;
        Offset at = 0;
        bool built = false;
    };

    // The rows laid out, where laying them out goes on when a window comes, and a place to show
    // once the page has a size.
    std::vector<Cut> shown;
    std::optional<Next> resume;
    std::optional<Place> wish;

    Size sized;
    bool fresh = true;

    std::vector<Stretch> marks;
    std::vector<Range> lit;

    // Whether the document changed since the view was last kept from going past its end.
    bool settling = false;

    // The memories of long lines, by line; the rows for the width `rowed`.
    std::unordered_map<std::size_t, Rows> rows;
    int rowed = -1;
    std::unordered_map<std::size_t, Columns> columns;

    // The width rows are cut to, a column at least.
    [[nodiscard]] int wide() const { return std::max(sized.wide, 1); }

    // Whether lines are cut into rows: with wrapping, once the page has a width.
    [[nodiscard]] bool cutting() const { return *page.wrap && sized.wide > 0; }

    // The width of a tab.
    [[nodiscard]] int tabbing() const { return std::max(*page.tab, 1); }

    // How clusters are drawn.
    [[nodiscard]] text::Rendering how() const { return page.rendering(); }

    // Makes sure the start of the top line is known; false while a window is on its way or past
    // the end.
    bool placed() {
        if (top_at) return true;

        if (!spool) return false;

        top_at = spool->at(top);

        if (!top_at) return false;

        row_at = *top_at;

        return true;
    }

    // Forgets the memories of lines far from the view once there are too many of them.
    void trimmed() {
        const auto far = [this](std::size_t line) { return line + lining / 2 < top || line > top + lining / 2; };

        if (rows.size() > lining) std::erase_if(rows, [&far](const auto& one) { return far(one.first); });

        if (columns.size() > lining) std::erase_if(columns, [&far](const auto& one) { return far(one.first); });
    }

    // The row memory of `line`, starting at `at`, for the width; one of another width starts anew.
    Rows& rowed_line(std::size_t line, Offset at) {
        if (rowed != wide()) {
            rows.clear();
            rowed = wide();
        }

        trimmed();

        Rows& one = rows[line];

        if (one.starts.empty()) one.starts.push_back({0, at});

        return one;
    }

    // Walks the rows of `line` from the latest start remembered not past the byte `byte` until
    // `done` stops at a row; gives its number and start, or none while a window is on its way. Row
    // starts on the spacing are remembered on the way.
    template <typename Done>
    std::optional<std::pair<std::size_t, Offset>> rows_to(std::size_t line, Offset at, Offset byte, Done done) {
        Rows& memory = rowed_line(line, at);
        const auto past = std::upper_bound(memory.starts.begin(), memory.starts.end(), byte, [](Offset want, const auto& one) { return want < one.second; });
        std::pair<std::size_t, Offset> here = *std::prev(past);

        while (true) {
            Skipper skip;
            const Walked row = wrapped_row(*spool, here.second, wide(), tabbing(), how(), skip);

            if (row.waiting) return std::nullopt;

            if (done(row)) return here;

            here = {here.first + 1, row.next};

            if (here.first % memory.every == 0 && memory.starts.back().first < here.first) {
                memory.starts.push_back(here);
                thinned(memory.starts, memory.every);
            }
        }
    }

    // The start of the row holding the byte `byte` of the wrapped line `line` starting at `at`;
    // past the end of the line, its last row.
    std::optional<Offset> row_holding(std::size_t line, Offset at, Offset byte) {
        const auto got = rows_to(line, at, byte, [byte](const Walked& one) { return one.last || one.next > byte; });

        if (!got) return std::nullopt;

        return got->second;
    }

    // The start of the row before the one starting at `byte` in the wrapped line `line`.
    std::optional<Offset> row_before(std::size_t line, Offset at, Offset byte) {
        const auto got = rows_to(line, at, byte - 1, [byte](const Walked& one) { return one.last || one.next >= byte; });

        if (!got) return std::nullopt;

        return got->second;
    }

    // The start of the last row of the wrapped line `line`.
    std::optional<Offset> last_row(std::size_t line, Offset at) {
        const auto got = rows_to(line, at, UINT64_MAX, [](const Walked& one) { return one.last; });

        if (!got) return std::nullopt;

        return got->second;
    }

    // The column memory of `line`, starting at `at`.
    Columns& columned(std::size_t line, Offset at) {
        trimmed();

        Columns& one = columns[line];

        if (one.marks.empty()) one.marks.push_back({0, at});

        return one;
    }

    // Walks `line` (starting at `at`) by clusters from the latest column remembered that `before`
    // takes, until `stop` stops before a cluster (given its column, width and byte) or the line
    // ends; gives the column and byte where it stopped, or none while a window is on its way.
    template <typename Before, typename Stop>
    std::optional<std::pair<std::size_t, Offset>> columns_to(std::size_t line, Offset at, Before before, Stop stop) {
        Columns& memory = columned(line, at);
        auto from = memory.marks.begin();

        for (auto one = memory.marks.begin(); one != memory.marks.end() && before(*one); ++one) from = one;

        std::size_t much = from->first;
        Reader reader{*spool, from->second, how()};

        while (true) {
            const std::optional<bool> end = reader.ending();

            if (!end) return std::nullopt;

            if (*end) return std::pair{much, reader.at()};

            const Offset was = reader.at();
            const std::string_view sign = reader.next();
            const std::size_t room = sign == "\t" ? tabbed(much, tabbing()) - much : static_cast<std::size_t>(text::width(sign, how()));

            if (stop(much, room, was)) return std::pair{much, was};

            if (much / memory.every > memory.marks.back().first / memory.every) {
                memory.marks.push_back({much, was});
                thinned(memory.marks, memory.every);
            }

            much += room;
        }
    }

    // The column of the byte `byte`, which starts a character, in `line` (starting at `at`); past
    // the end of the line, the column after it.
    std::optional<std::size_t> column_at(std::size_t line, Offset at, Offset byte) {
        const auto got = columns_to(
            line, at, [byte](const auto& one) { return one.second <= byte; },
            [byte](std::size_t much, std::size_t room, Offset was) {
                (void)much;
                (void)room;

                return was >= byte;
            });

        if (!got) return std::nullopt;

        return got->first;
    }

    // The character of `line` (starting at `at`) in which the column `column` falls: its column and
    // byte; past the end of the line, those of the end.
    std::optional<std::pair<std::size_t, Offset>> boundary_at(std::size_t line, Offset at, std::size_t column) {
        return columns_to(
            line, at, [column](const auto& one) { return one.first <= column; },
            [column](std::size_t much, std::size_t room, Offset was) {
                (void)was;

                return much + room > column;
            });
    }

    // Where the row after the one starting at `at` in `line` starts, its line and byte; none at the
    // end of the document or while a window is on its way (`waiting` then).
    std::optional<std::pair<std::size_t, Offset>> after(std::size_t line, Offset at, bool& waiting) {
        waiting = false;

        if (cutting()) {
            Skipper skip;
            const Walked row = wrapped_row(*spool, at, wide(), tabbing(), how(), skip);

            if (row.waiting) {
                waiting = true;

                return std::nullopt;
            }

            if (!row.last) return std::pair{line, row.next};

            if (row.next >= spool->whole()) return std::nullopt;

            return std::pair{line + 1, row.next};
        }

        const std::optional<Spool::Line> got = spool->line(line);

        if (!got) {
            waiting = spool->stalled();

            return std::nullopt;
        }

        if (got->next >= spool->whole()) return std::nullopt;

        return std::pair{line + 1, got->next};
    }

    // One row down from the top; false at the end or while a window is on its way.
    bool down() {
        if (!placed()) return false;

        bool waiting = false;
        const auto next = after(top, row_at, waiting);

        if (!next) return false;

        if (next->first != top) {
            top = next->first;
            top_at = next->second;
        }

        row_at = next->second;

        return true;
    }

    // Where the row before the one starting at `at` in `line` (which starts at `line_at`) starts:
    // its line, the line's start and its own; none at the start of the document or while a window
    // is on its way.
    std::optional<Next> earlier(std::size_t line, Offset line_at, Offset at) {
        if (cutting() && at > line_at) {
            const std::optional<Offset> went = row_before(line, line_at, at);

            if (!went) return std::nullopt;

            return Next{line, line_at, *went, false};
        }

        if (line == 0) return std::nullopt;

        const std::optional<Offset> before = spool->at(line - 1);

        if (!before) return std::nullopt;

        Offset start = *before;

        if (cutting()) {
            const std::optional<Offset> last = last_row(line - 1, *before);

            if (!last) return std::nullopt;

            start = *last;
        }

        return Next{line - 1, *before, start, false};
    }

    // One row up from the top; false at the start or while a window is on its way.
    bool up() {
        if (!placed()) return false;

        const std::optional<Next> went = earlier(top, *top_at, row_at);

        if (!went) return false;

        top = went->line;
        top_at = went->line_at;
        row_at = went->at;

        return true;
    }

    // The byte at the column `column` of the row starting at `at` in `line` (which starts at
    // `line_at`): the start of the character standing there; past the row its end when the row ends
    // its line, otherwise the start of its last character, since the end of a row cut by wrapping
    // shows on the next one. None while a window is on its way.
    std::optional<Offset> at_column(std::size_t line, Offset line_at, Offset at, std::size_t column) {
        if (!cutting()) {
            const auto got = boundary_at(line, line_at, column);

            if (!got) return std::nullopt;

            return got->second;
        }

        Builder builder;

        builder.line_at = line_at;

        const Walked row = wrapped_row(*spool, at, wide(), tabbing(), how(), builder);
        const std::vector<std::size_t>& bytes = builder.made.bytes;

        if (row.waiting) return std::nullopt;

        if (column < bytes.size()) return line_at + bytes[column];

        if (row.last) return row.end;

        return line_at + bytes.back();
    }

    // How many rows there are from the top on, counting `most` at most; none while a window is on
    // its way, since then the end is not known.
    std::optional<int> ahead_of(int most) {
        if (!placed()) return std::nullopt;

        std::size_t line = top;
        Offset at = row_at;
        int made = 1;

        while (made < most) {
            bool waiting = false;
            const auto next = after(line, at, waiting);

            if (waiting) return std::nullopt;

            if (!next) break;

            line = next->first;
            at = next->second;

            ++made;
        }

        return made;
    }

    // Whether the scrollbar counts lines: when they are known and short on average.
    [[nodiscard]] bool lined() const {
        const std::optional<std::size_t> many = spool->counted();

        return many && *many * short_line >= spool->whole();
    }
};


// A page with no document yet, in the class "page". The keys nobody took go to `keyed`: for a
// page the arrows, the page keys, Home and End move the view, and without wrapping the arrows to
// the sides move it sideways; Enter tells `Followed` for the current mark when it has an address,
// as a click on such a mark does (a click with Ctrl is the terminal's, which opens the link
// itself). The wheel moves the view three rows a step, sideways when tilted and not wrapping. Keys
// with Ctrl or Alt are left to the nodes above.
// Wrapping, the shift and the width of a tab lay the rows out again; wrapping turned goes back to
// the start of the top line and the first column, since the same byte means another row in the
// other mode.
Page::Page() : kept_(std::make_unique<Kept>(*this)) {
    classes({"page"});
    focusable(true);

    hooks_.add(on<Changed>([this](Changed& what) {
        Kept& kept = *kept_;

        if (what.of(wrap)) {
            if (kept.top_at) kept.row_at = *kept.top_at;

            shift = 0;
        }

        if (what.of(tab)) {
            kept.rows.clear();
            kept.columns.clear();
        }

        if (!what.of(wrap) && !what.of(shift) && !what.of(tab)) return;

        kept.fresh = true;

        refreshed();
        served();
        scrolled();
    }));

    hooks_.add(otherwise<KeyEvent>([this](KeyEvent& what) { keyed(what); }));

    hooks_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        if (what.press == Press::wheel) {
            const int by = -wheeled * what.steps;

            if (what.axis == Axis::vertical) {
                if (slid(by)) what.take();

                return;
            }

            if (*wrap) return;

            const std::size_t was = *shift;

            shift = by < 0 && static_cast<std::size_t>(-by) > was ? 0 : static_cast<std::size_t>(static_cast<long long>(was) + by);

            if (*shift != was) what.take();

            return;
        }

        if (what.press != Press::click || what.button != MouseButton::left || what.mods.ctrl) return;

        const std::optional<Place> place = under(what.at);

        if (!place) return;

        const auto mark = std::find_if(kept_->marks.begin(), kept_->marks.end(),
                                       [&place](const Stretch& one) { return !one.address.empty() && one.from <= *place && *place < one.upto; });

        if (mark == kept_->marks.end()) return;

        what.take();
        followed(static_cast<std::size_t>(mark - kept_->marks.begin()));
    }));
}

// The keys of a page that nobody took, without Ctrl or Alt: the arrows, the page keys, Home and
// End move the view, the arrows to the sides sideways without wrapping, and Enter follows the
// current mark when it has an address.
void Page::keyed(KeyEvent& what) {
    if (what.key.mods().ctrl || what.key.mods().alt) return;

    const int page = std::max(kept_->sized.high, 1);

    switch (what.key.code()) {
        case Code::up: (void)slid(-1); break;
        case Code::down: (void)slid(1); break;
        case Code::pageup: (void)slid(-page); break;
        case Code::pagedown: (void)slid(page); break;
        case Code::home:
            shift = 0;
            first(0);
            break;
        case Code::end: end(); break;
        case Code::left:
            if (*wrap) return;

            if (*shift > 0) shift = *shift - 1;
            break;
        case Code::right:
            if (*wrap) return;

            shift = *shift + 1;
            break;
        case Code::enter:
            if (!*current || kept_->marks.at(**current).address.empty()) return;

            followed(**current);
            break;
        default: return;
    }

    what.take();
}

// A page goes with its memories.
Page::~Page() = default;

// The number of lines: none without a document, or until a document read in windows was walked to
// its end.
std::optional<std::size_t> Page::lines() const {
    if (!kept_->spool) return std::nullopt;

    return kept_->spool->counted();
}

// The line of the document at the top.
std::size_t Page::first() const { return kept_->top; }

// Puts the line `line` at the top, the last line when it lies past it, and no further down than the
// last row stays at the bottom.
void Page::first(std::size_t line) {
    Kept& kept = *kept_;

    if (!kept.spool) return;

    const std::optional<std::size_t> many = lines();
    const std::size_t want = many && *many > 0 && line >= *many ? *many - 1 : line;

    if (want == kept.top && kept.top_at && kept.row_at == *kept.top_at) return;

    kept.top = want;
    kept.top_at.reset();
    kept.row_at = 0;
    kept.fresh = true;

    (void)kept.placed();
    (void)moved(0);

    refreshed();
    served();
    scrolled();
}

// Shows the last line at the bottom. The end of a document read in windows is known only after
// walking it: the spool counts the lines on the way, window by window, and the end comes by itself
// once they are counted, and again while the rows above the last are not read yet.
void Page::end() {
    Kept& kept = *kept_;

    if (!kept.spool) return;

    kept.spool->counting();

    const std::optional<std::size_t> many = kept.spool->counted();

    if (!many) {
        kept.ending = true;

        served();

        return;
    }

    if (*many == 0) return;

    const std::optional<Offset> from = kept.spool->at(*many - 1);

    if (!from) {
        kept.ending = true;

        served();

        return;
    }

    Offset start = *from;

    if (kept.cutting()) {
        const std::optional<Offset> last = kept.last_row(*many - 1, *from);

        if (!last) {
            kept.ending = true;

            served();

            return;
        }

        start = *last;
    }

    kept.top = *many - 1;
    kept.top_at = *from;
    kept.row_at = start;
    kept.fresh = true;

    const int high = std::max(kept.sized.high, 1);

    (void)moved(1 - high);

    const std::optional<int> rows = kept.ahead_of(high);

    kept.ending = rows && *rows < high;

    refreshed();
    served();
    scrolled();
}

// Puts the marks `all` on the page, in the order of their starts, with none current.
void Page::marks(std::vector<Stretch> all) {
    std::sort(all.begin(), all.end(), sooner);

    kept_->marks = std::move(all);
    current = std::nullopt;

    repaint();
}

// The marks, in the order of their starts.
const std::vector<Stretch>& Page::marks() const { return kept_->marks; }

// Makes the mark `mark` current and moves the view as little as shows its start: not at all when it
// shows, to the top when it lies above, to the bottom when below. A mark past the last throws
// std::out_of_range.
void Page::reach(std::size_t mark) {
    const Stretch& one = kept_->marks.at(mark);

    current = mark;

    if (!revealed(one.from)) return;

    refreshed();
    served();
    scrolled();
}

// How far the view goes: in lines when they are known and short, in bytes otherwise.
std::size_t Page::extent() const {
    const Kept& kept = *kept_;

    if (!kept.spool) return 0;

    return kept.lined() ? *kept.spool->counted() : static_cast<std::size_t>(kept.spool->whole());
}

// What the view shows: the rows of the screen, or the bytes from its first row to the end of its
// last.
std::size_t Page::window() const {
    const Kept& kept = *kept_;

    if (!kept.spool) return 0;

    if (kept.lined()) return static_cast<std::size_t>(std::max(kept.sized.high, 0));

    return kept.shown.empty() ? 0 : static_cast<std::size_t>(kept.shown.back().end - kept.row_at);
}

// Where the view stands: its top line, or the byte of its first row.
std::size_t Page::top() const {
    const Kept& kept = *kept_;

    if (!kept.spool) return 0;

    return kept.lined() ? kept.top : static_cast<std::size_t>(kept.row_at);
}

// Moves the view to `row` of the units of `extent`, as a scrollbar asks. In bytes a row holds many
// of them, and a move a few bytes down that stays in the top row goes a row down instead, so that
// a step of the scrollbar's wheel moves the view; a move up always leaves the row.
void Page::top(std::size_t row) {
    Kept& kept = *kept_;

    if (!kept.spool) return;

    if (kept.lined()) {
        first(row);

        return;
    }

    const Offset was = kept.row_at;

    jump(row);

    if (kept.row_at == was && row > was) (void)slid(1);
}

// Shows the document of `given`. The view keeps its line, the last when the document is shorter;
// what waited for a window waits on.
void Page::hold(std::shared_ptr<Spool> given) {
    Kept& kept = *kept_;

    kept.spool = std::move(given);
    kept.top_at.reset();
    kept.row_at = 0;
    kept.rows.clear();
    kept.columns.clear();
    kept.fresh = true;

    if (const std::optional<std::size_t> many = lines(); many && kept.top >= *many) kept.top = *many > 0 ? *many - 1 : 0;

    (void)kept.placed();
    (void)moved(0);

    refreshed();
    served();
    scrolled();
}

// How many bytes the document has; none without one.
std::optional<std::uint64_t> Page::whole() const {
    if (!kept_->spool) return std::nullopt;

    return kept_->spool->whole();
}

// The byte where the first row of the view starts, as Midnight Commander tells the place in a file;
// none without a document or while the start of the top line is not known.
std::optional<std::uint64_t> Page::at() const {
    if (!kept_->spool || !kept_->top_at) return std::nullopt;

    return kept_->row_at;
}

// Moves the view to the row holding the byte `byte`. While the spool has not reached it, the jump
// waits and goes on as windows come, the walk taking up where it stopped.
void Page::jump(std::uint64_t byte) {
    Kept& kept = *kept_;

    if (!kept.spool) return;

    kept.sought.reset();

    const std::size_t which = kept.spool->found(byte);

    if (kept.spool->stalled()) {
        kept.sought = byte;

        served();

        return;
    }

    const std::optional<Offset> from = kept.spool->at(which);

    if (!from) {
        kept.sought = byte;

        served();

        return;
    }

    Offset start = *from;

    if (kept.cutting()) {
        const std::optional<Offset> row = kept.row_holding(which, *from, byte);

        if (!row) {
            kept.sought = byte;

            served();

            return;
        }

        start = *row;
    }

    kept.top = which;
    kept.top_at = *from;
    kept.row_at = start;
    kept.fresh = true;

    (void)moved(0);

    refreshed();
    served();
    scrolled();
}

// Reads the window from `from` of `most` bytes and gives it to `arrived`: nothing for a document at
// hand, which orders nothing.
void Page::order(std::uint64_t from, std::size_t most) {
    (void)from;
    (void)most;
}

// The window read from `from` has come: laying the rows out goes on, and so does the end or the
// jump that waited for it.
void Page::arrived(std::uint64_t from, Bytes got) {
    Kept& kept = *kept_;

    kept.spool->arrived(from, std::move(got));

    if (kept.sought) jump(*kept.sought);

    if (kept.ending) end();

    refreshed();
    served();
    scrolled();
}

// The document changed from the line `line` on, at once and whole, as an editor tells after its
// spool followed the text: the memories of those lines go, the top line stays where it was (the
// last when fewer are left) and so does its top row, found again in the line, and the rows are laid
// out with the next frame. Laying out at every change would cost a frame per change when many come
// at once.
void Page::edited(std::size_t line) {
    Kept& kept = *kept_;

    if (!kept.spool) return;

    std::erase_if(kept.rows, [line](const auto& one) { return one.first >= line; });
    std::erase_if(kept.columns, [line](const auto& one) { return one.first >= line; });

    Offset inside = kept.top_at ? kept.row_at - *kept.top_at : 0;

    if (const std::optional<std::size_t> many = lines(); many && *many > 0 && kept.top >= *many) {
        kept.top = *many - 1;
        inside = 0;
    }

    if (kept.top >= line) {
        kept.top_at.reset();

        if (kept.placed() && kept.cutting())
            if (const std::optional<Offset> row = kept.row_holding(kept.top, *kept.top_at, *kept.top_at + inside)) kept.row_at = *row;
    }

    kept.fresh = true;
    kept.settling = true;

    relayout();
}

// Moves the view as little as shows the place `where`, as `reach` does for a mark.
void Page::show(Place where) {
    if (!revealed(where)) return;

    refreshed();
    served();
    scrolled();
}

// Paints the stretches `all` over the rows and the marks in the look of the page in the state
// `select`, as an editor shows its selection and its block.
void Page::lit(std::vector<Range> all) {
    if (all == kept_->lit) return;

    kept_->lit = std::move(all);

    repaint();
}

// The place of the document under `at` (in the box): the line and byte of the character in that
// cell, past the end of a row the end of the row; none off the rows. Rows changed since the last
// frame are laid out first.
std::optional<Place> Page::under(Spot at) {
    const Style& style = this->style();
    const int line = style.border == Border::none ? 0 : 1;
    const int x = at.x - line - style.padding.left;
    const int y = at.y - line - style.padding.top;
    Kept& kept = *kept_;

    if (kept.fresh) spread();

    if (x < 0 || y < 0 || static_cast<std::size_t>(y) >= kept.shown.size()) return std::nullopt;

    const Cut& cut = kept.shown[static_cast<std::size_t>(y)];

    if (static_cast<std::size_t>(x) >= cut.bytes.size()) return Place{cut.line, cut.upto};

    return Place{cut.line, cut.bytes[static_cast<std::size_t>(x)]};
}

// The cell of the content where the place `where` shows, as laid out: the first column of the
// character starting there, or the one after the row when it ends its line there; none when it
// does not show.
std::optional<Spot> Page::spot(Place where) const {
    const Kept& kept = *kept_;

    for (std::size_t row = 0; row < kept.shown.size(); ++row) {
        const Cut& cut = kept.shown[row];

        if (!showing(cut, where)) continue;

        std::size_t x = 0;

        while (x < cut.bytes.size() && cut.bytes[x] < where.byte) ++x;

        return Spot{static_cast<int>(x), static_cast<int>(row)};
    }

    return std::nullopt;
}

// The column where the place `where` stands in its row: from the row's start with wrapping, from
// the line's start without it; none without a document or while a window is on its way.
std::optional<std::size_t> Page::across(Place where) {
    Kept& kept = *kept_;

    if (!kept.spool) return std::nullopt;

    const std::optional<Offset> from = kept.spool->at(where.line);

    if (!from) return std::nullopt;

    const Offset byte = *from + where.byte;

    if (!kept.cutting()) return kept.column_at(where.line, *from, byte);

    const std::optional<Offset> row = kept.row_holding(where.line, *from, byte);

    if (!row) return std::nullopt;

    Builder builder;

    builder.line_at = *from;

    if (wrapped_row(*kept.spool, *row, kept.wide(), kept.tabbing(), kept.how(), builder).waiting) return std::nullopt;

    const std::vector<std::size_t>& bytes = builder.made.bytes;
    const auto at = std::find_if(bytes.begin(), bytes.end(), [&where](std::size_t one) { return one >= where.byte; });

    return static_cast<std::size_t>(at - bytes.begin());
}

// The place at the column `column` of the row `rows` rows of the screen below the one showing
// `from` (above when negative), as `across` counts columns: a character's start, the end of a row
// that ends its line, or the last character of a row that wrapping cut. Near the start or the end
// of the document it goes as far as there is; none when it cannot move a row, without a document or
// while a window is on its way.
std::optional<Place> Page::beyond(Place from, int rows, std::size_t column) {
    Kept& kept = *kept_;

    if (!kept.spool) return std::nullopt;

    const std::optional<Offset> start = kept.spool->at(from.line);

    if (!start) return std::nullopt;

    Kept::Next here{from.line, *start, *start, false};

    if (kept.cutting()) {
        const std::optional<Offset> row = kept.row_holding(from.line, *start, *start + from.byte);

        if (!row) return std::nullopt;

        here.at = *row;
    }

    int went = 0;

    for (; went < rows; ++went) {
        bool waiting = false;
        const auto next = kept.after(here.line, here.at, waiting);

        if (!next) break;

        if (next->first != here.line) here.line_at = next->second;

        here = Kept::Next{next->first, here.line_at, next->second, false};
    }

    for (; went > rows; --went) {
        const std::optional<Kept::Next> before = kept.earlier(here.line, here.line_at, here.at);

        if (!before) break;

        here = *before;
    }

    if (went == 0) return std::nullopt;

    const std::optional<Offset> byte = kept.at_column(here.line, here.line_at, here.at, column);

    if (!byte) return std::nullopt;

    return Place{here.line, static_cast<std::size_t>(*byte - here.line_at)};
}

// The least room: the layout sizes a page, not its document.
Size Page::measure(Limits room) const { return room.least; }

// Lays out the rows the inside shows: after a change of the document the view first goes no
// further down than the last row stays at the bottom, and a place waiting for a size is shown.
void Page::arrange(Area inside) {
    Kept& kept = *kept_;

    if (kept.sized != inside.size) kept.fresh = true;

    kept.sized = inside.size;

    if (kept.settling) {
        kept.settling = false;

        (void)moved(0);
    }

    if (kept.wish) {
        const Place where = *kept.wish;

        kept.wish.reset();

        (void)revealed(where);
    }

    spread();
    served();
    scrolled();
}

// The rows, the marks over them in the looks of their parts (the current one in the state
// `current`, and `focus` too while the page has the focus), the links of the marks with an
// address, and the stretches lit over all of it in the state `select`, with the cell after a line
// lit too when they go on past its end.
void Page::paint(Canvas& onto) const {
    const Kept& kept = *kept_;
    const Look plain = look();
    const bool lit = states().has(State::focus);

    onto.fill(onto.seen(), plain);

    for (std::size_t row = 0; row < kept.shown.size(); ++row) {
        const Cut& cut = kept.shown[row];
        const int y = static_cast<int>(row);

        (void)onto.put({0, y}, cut.text, plain);

        const Place last{cut.line, cut.upto};

        for (std::size_t at = 0; at < kept.marks.size() && kept.marks[at].from < last; ++at) {
            const Stretch& one = kept.marks[at];
            const bool here = *current && **current == at;
            States made = states();

            made.set(State::current, here);
            made.set(State::focus, here && lit);

            const Look marked = look(one.part, made);

            for (std::size_t x = 0; x < cut.bytes.size(); ++x) {
                const Place where{cut.line, cut.bytes[x]};

                if (where < one.from || !(where < one.upto)) continue;

                const Area cell{{static_cast<int>(x), y}, {1, 1}};

                onto.shade(cell, marked);

                onto.link(cell, one.address);
            }
        }
    }

    if (kept.lit.empty()) return;

    States chosen = states();

    chosen.set(State::select, true);

    const Look selected = look({}, chosen);
    const auto inside = [&kept](Place where) {
        return std::any_of(kept.lit.begin(), kept.lit.end(), [where](const Range& one) { return one.from <= where && where < one.upto; });
    };

    for (std::size_t row = 0; row < kept.shown.size(); ++row) {
        const Cut& cut = kept.shown[row];
        const int y = static_cast<int>(row);

        for (std::size_t x = 0; x < cut.bytes.size(); ++x)
            if (inside(Place{cut.line, cut.bytes[x]})) onto.shade(Area{{static_cast<int>(x), y}, {1, 1}}, selected);

        if (cut.last && inside(Place{cut.line, cut.upto}))
            onto.shade(Area{{static_cast<int>(cut.bytes.size()), y}, {1, 1}}, selected);
    }
}

// Lays out the rows the view shows: all of them anew when the view or the size changed, otherwise
// on from where a window was missing the last time, so that the rows laid out already are not read
// again (a line walked to its end window by window would otherwise read its first row again at
// every window). Each row reads only the bytes it shows, and a full screen seeks no row after its
// last, which without wrapping would walk a long line to its end. Rows reaching the end of the
// document from a known line tell the spool the count of lines.
void Page::spread() {
    Kept& kept = *kept_;
    const Size given = kept.sized;
    Kept::Next next;

    if (kept.fresh) {
        kept.shown.clear();
        kept.resume.reset();

        if (!kept.spool || given.wide <= 0 || given.high <= 0) {
            kept.fresh = false;

            return;
        }

        kept.spool->screen(given.high);

        if (!kept.placed()) {
            kept.fresh = kept.spool->stalled();

            return;
        }

        kept.fresh = false;
        next = Kept::Next{kept.top, *kept.top_at, kept.row_at, false};
    } else if (kept.resume) {
        next = *kept.resume;
        kept.resume.reset();
    } else {
        return;
    }

    while (true) {
        if (!next.built) {
            Builder builder;
            Walked row;

            builder.line_at = next.line_at;

            if (*wrap) {
                row = wrapped_row(*kept.spool, next.at, given.wide, kept.tabbing(), kept.how(), builder);
            } else if (const auto from = kept.boundary_at(next.line, next.line_at, *shift)) {
                row = clipped_row(*kept.spool, from->second, from->first, *shift, given.wide, kept.tabbing(), kept.how(), builder);
            } else {
                row.waiting = true;
            }

            if (row.waiting) {
                kept.resume = next;

                return;
            }

            Cut made = std::move(builder.made);

            made.line = next.line;
            made.at = next.at;
            made.end = row.end;
            made.next = row.next;
            made.from = static_cast<std::size_t>(next.at - next.line_at);
            made.upto = static_cast<std::size_t>(row.end - next.line_at);
            made.last = row.last;

            kept.shown.push_back(std::move(made));

            if (kept.shown.size() >= static_cast<std::size_t>(given.high)) return;
        }

        const Cut& row = kept.shown.back();
        bool waiting = false;
        std::optional<std::pair<std::size_t, Offset>> after;

        if (!*wrap) after = kept.after(next.line, next.at, waiting);
        else if (!row.last) after = std::pair{next.line, row.next};
        else if (row.next < kept.spool->whole()) after = std::pair{next.line + 1, row.next};

        if (waiting) {
            kept.resume = Kept::Next{next.line, next.line_at, next.at, true};

            return;
        }

        if (!after) {
            if (row.last) kept.spool->counted(next.line + 1);

            return;
        }

        if (after->first != next.line) next.line_at = after->second;

        next = Kept::Next{after->first, next.line_at, after->second, false};
    }
}

// Lays the rows out again after the view moved, and paints.
void Page::refreshed() {
    spread();
    repaint();
}

// Moves the view by `rows` rows of the screen, up when negative; gives whether it moved. A step
// counts rows of the screen, as far as the picture moves. The view goes no further down than the
// last row stands at the bottom; that holds for no step too, and puts a jump to the last page in
// order. A row the spool does not have yet is not the end: the step stops at what there is. It lays
// nothing out and tells nothing, so that a move in two steps tells once.
bool Page::moved(int rows) {
    Kept& kept = *kept_;

    if (!kept.spool) return false;

    const std::size_t was = kept.top;
    const Offset was_at = kept.row_at;

    for (int one = 0; one < rows; ++one)
        if (!kept.down()) break;

    for (int one = 0; one > rows; --one)
        if (!kept.up()) break;

    if (const int high = kept.sized.high; high > 0)
        if (const std::optional<int> many = kept.ahead_of(high))
            for (int missing = high - *many; missing > 0; --missing)
                if (!kept.up()) break;

    if (kept.top == was && kept.row_at == was_at) return false;

    kept.fresh = true;

    return true;
}

// Moves the view by `rows` rows of the screen as `moved` does, and when it moved lays the rows out
// and tells `Scrolled`; gives whether it moved.
bool Page::slid(int rows) {
    if (!moved(rows)) return false;

    refreshed();
    served();
    scrolled();

    return true;
}

// Moves the view as little as shows the place `where`: not at all when it shows, to the top when it
// lies above, to the bottom when below, and without wrapping sideways as far as needed. Gives
// whether the view moved. Before the page has a size, the place waits for it.
bool Page::revealed(Place where) {
    Kept& kept = *kept_;

    if (kept.sized.high <= 0 || kept.sized.wide <= 0) {
        kept.wish = where;

        return false;
    }

    if (!kept.spool) return false;

    const std::optional<Offset> from = kept.spool->at(where.line);

    if (!from) return false;

    bool shifted = false;

    if (!*wrap) {
        if (const std::optional<std::size_t> column = kept.column_at(where.line, *from, *from + where.byte)) {
            const auto wide = static_cast<std::size_t>(kept.sized.wide);

            if (*column < *shift) {
                shift = *column;
                shifted = true;
            } else if (*column >= *shift + wide) {
                shift = *column - wide + 1;
                shifted = true;
            }
        }
    }

    spread();

    if (std::any_of(kept.shown.begin(), kept.shown.end(), [where](const Cut& one) { return showing(one, where); })) return shifted;

    Offset start = *from;

    if (*wrap) {
        if (const std::optional<Offset> row = kept.row_holding(where.line, *from, *from + where.byte)) start = *row;
    }

    const bool above = where.line < kept.top || (where.line == kept.top && start < kept.row_at);

    kept.top = where.line;
    kept.top_at = *from;
    kept.row_at = start;
    kept.fresh = true;

    if (!above) (void)moved(-std::max(kept.sized.high - 1, 0));

    return true;
}

// Hands the spool's order of a window to `order`.
void Page::served() {
    if (!kept_->spool) return;

    if (const std::optional<Spool::Order> one = kept_->spool->ordered()) order(one->from, one->most);
}

// Tells `Scrolled`, for a scrollbar beside the page.
void Page::scrolled() {
    Scrolled one;

    notified(one);
}

// Tells `Followed` with the address of the mark `mark`; when nobody takes it, the document opens
// the address (`Document::follow`).
void Page::followed(std::size_t mark) {
    Followed one{kept_->marks.at(mark).address};

    notified(one);
    controlling::unheeded(*this, one);
}

// A pager of `text`, in the class "pager", wrapping, following its changes with the view on its
// line. A text that is none throws std::invalid_argument.
Pager::Pager(std::shared_ptr<Text> text) : text_(std::move(text)) {
    if (text_ == nullptr) throw std::invalid_argument{"tal::Pager: no text"};

    add_class("pager");
    wrap = true;
    reread();

    told_ = text_->on<Changes>([this](Changes& what) {
        (void)what;

        reread();
    });
}

// A pager of `file`, in the class "pager", wrapping, read in windows; `head` holds its first bytes,
// read already, which show at once. A file not open throws std::invalid_argument. Until its size is
// known the pager shows nothing, and a file that cannot tell it shows nothing and tells `Unread`.
// The reading runs on the program's loop.
Pager::Pager(fs::File file, Bytes head) : file_(std::move(file)) {
    if (!file_.held()) throw std::invalid_argument{"tal::Pager: no file"};

    add_class("pager");
    wrap = true;

    scope_.emplace();
    scope_->run(opened(std::move(head)));
}

// The size of the document in bytes, once known.
std::optional<std::uint64_t> Pager::size() const { return whole(); }

// The byte where the first row of the view starts; 0 while it is not known.
std::uint64_t Pager::at() const { return Page::at().value_or(0); }

// Moves the view to the row holding the byte `byte`; past the end, to the last line.
void Pager::at(std::uint64_t byte) { jump(byte); }

// Reads the window in the pager's scope; only a pager of a file orders one.
void Pager::order(std::uint64_t from, std::size_t most) { scope_->run(fetched(from, most)); }

// Learns the size of the file and shows it, its head at hand.
Task<void> Pager::opened(Bytes head) {
    std::uint64_t much = 0;
    std::optional<Trouble> trouble;

    try {
        much = co_await file_.size();
    } catch (const Trouble& why) {
        trouble = why;
    }

    hold(std::make_shared<spooling::Reel>(much, std::move(head)));

    if (!trouble) co_return;

    Unread one{*trouble};

    notified(one);
}

// Reads the window from `from` and gives it to the page; a read that fails ends the document where
// it got and tells `Unread`.
Task<void> Pager::fetched(std::uint64_t from, std::size_t most) {
    Bytes got;
    std::optional<Trouble> trouble;

    try {
        got = co_await file_.read(from, most);
    } catch (const Trouble& why) {
        trouble = why;
    }

    arrived(from, std::move(got));

    if (!trouble) co_return;

    Unread one{*trouble};

    notified(one);
}

// Shows the text as it is now.
void Pager::reread() { hold(std::make_shared<spooling::Reel>(bytes(text_->whole()))); }

}
