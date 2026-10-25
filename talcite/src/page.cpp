#include "talcite/page.hpp"

#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "eliding.hpp"
#include "scrollbar.hpp"
#include "spool.hpp"

namespace talcite::ui {

// A page draws a byte document a screen at a time and never holds a whole line. A screen row is
// put together from the bytes it shows, read from the spool in pieces from the row's start until
// the row is full or its line ends; a line of a gigabyte costs a frame what a line of eighty
// characters does. Where a line ends, the page learns by reading up to its \n; only stepping past
// a long line without wrapping, a jump into its middle and End need to walk it, and the spool does
// that window by window, remembering how far it got.
//
// Two memories serve the walks inside long lines, both kept only for the lines around the view:
// where screen rows start in a wrapped line (every few dozen rows, for stepping up and for a jump
// to a byte), and at which byte every few hundred columns start (for scrolling sideways without
// wrapping, and for the column of the editor's cursor).

namespace {

// How many lines one wheel step scrolls. The same number as in the scrollers.
constexpr int stepping = 3;

// How many bytes are read from the spool at once while a row is put together.
constexpr std::size_t reading = 4096;

// The longest cluster in bytes: thirty characters after the first (the Stream-Safe limit in
// `encoding.cpp`), four bytes each, with room to spare. A piece is read again before fewer bytes
// than this are left in it, so a cluster never breaks at the end of a piece.
constexpr std::size_t margin = 128;

// Row starts are remembered every this many rows of a wrapped line, at first.
constexpr std::size_t sampling = 64;

// Column bytes are remembered every this many columns of a line, at first.
constexpr std::size_t columning = 256;

// Past this many samples in one line, every other is dropped and the spacing doubles, so that a
// line of a gigabyte keeps a few thousand of them, not millions.
constexpr std::size_t samples = 4096;

// How many lines may have their rows and columns remembered before those far from the view are
// forgotten.
constexpr std::size_t lining = 256;

// The scrollbar counts lines when they are known and average no more than this many bytes;
// otherwise it counts bytes, because in a document of a few very long lines the thumb would stand
// still for the whole length of each of them.
constexpr std::size_t short_line = 1024;

// The piece of a document line that occupies one screen row: the finished text and a column to
// byte map, along which the marks are overlaid. The map counts bytes from the start of the line,
// as marks and places do.
struct Cut {
    std::string text;
    std::vector<std::size_t> bytes;
    std::size_t line = 0;

    // Where the row starts and ends in the document; `from` and `upto` are the same counted from
    // the start of the line. The end is the byte just past the last character of the row: the column
    // map says where each character starts but not where it ends, and a click past the end of a
    // line has to land exactly there.
    std::size_t at = 0;
    std::size_t end = 0;
    std::size_t from = 0;
    std::size_t upto = 0;

    // Whether the row ends its line.
    bool last = false;
};

// Reads a line cluster by cluster from a byte, through the spool in pieces, and stops at the end
// of the line: a \n, a \r right before one, or the end of the document with a \r before it or not.
// Nothing is kept beyond the piece at hand.
class Reader {
public:
    Reader(Spool& spool, std::size_t at) : spool_(spool), at_(at), from_(at) {}

    // The byte where the next cluster starts.
    [[nodiscard]] std::size_t at() const { return at_; }

    // Where the next line starts, once `ending()` said that this one ends here.
    [[nodiscard]] std::size_t beyond() const { return beyond_; }

    // Whether the line ends at the reader; nothing while a window is on its way.
    [[nodiscard]] std::optional<bool> ending() {
        if (!filled()) return {};

        const std::size_t off = at_ - from_;

        if (off >= buffer_.size()) {
            beyond_ = at_;

            return true;
        }

        const char one = buffer_[off];

        if (one == '\n') {
            beyond_ = at_ + 1;

            return true;
        }

        if (one == '\r') {
            if (off + 1 >= buffer_.size()) {
                beyond_ = at_ + 1;

                return true;
            }

            if (buffer_[off + 1] == '\n') {
                beyond_ = at_ + 2;

                return true;
            }
        }

        return false;
    }

    // The next cluster of the line; nothing at its end or while a window is on its way. The view is
    // valid until the next call.
    [[nodiscard]] std::optional<std::string_view> next() {
        const std::optional<bool> end = ending();

        if (!end.has_value() || *end) return {};

        const std::string_view all{buffer_};
        const std::size_t off = at_ - from_;
        const std::size_t upto = cluster_after(all, off);

        at_ = from_ + upto;

        return all.substr(off, upto - off);
    }

private:
    // Whether the piece holds enough from the reader on to tell the next cluster: `margin` bytes, a
    // line end (a cluster never goes past one), or the end of the document.
    [[nodiscard]] bool enough() const {
        const std::size_t have = from_ + buffer_.size();

        if (at_ < from_ || at_ > have) return false;

        if (have >= spool_.whole() || at_ + margin <= have) return true;

        return std::string_view{buffer_}.find('\n', at_ - from_) != std::string_view::npos;
    }

    // Makes sure the piece holds enough from the reader on; returns false while a window is on its
    // way. What the spool has at hand is taken first, so the head of a file is shown before the rest
    // arrives; only when that is not enough is a whole piece asked for.
    bool filled() {
        if (enough()) return true;

        if (const std::optional<std::string_view> got = spool_.read(at_, reading, 1); got.has_value()) {
            buffer_.assign(got->data(), got->size());
            from_ = at_;

            if (enough()) return true;
        }

        const std::optional<std::string_view> got = spool_.read(at_, reading);

        if (!got.has_value()) return false;

        buffer_.assign(got->data(), got->size());
        from_ = at_;

        return true;
    }

    Spool& spool_;
    std::size_t at_ = 0;
    std::size_t from_ = 0;
    std::size_t beyond_ = 0;
    std::string buffer_;
};

// How a row ended: where it starts, where its last character ends, where the following row starts
// (the end, or the start of the next line when the row ends its line), and whether a window was
// missing on the way, in which case the rest means nothing.
struct Row {
    std::size_t at = 0;
    std::size_t end = 0;
    std::size_t next = 0;
    bool last = false;
    bool waiting = false;
};

// Puts a cut together: the text and, for every column, the byte (from the start of the line) of the
// character standing in it, also the second column of a double-width character and every column of
// an expanded tab. Without that there would be no way to tell whether a cell falls within a mark.
struct Builder {
    Cut made;
    std::size_t line_at = 0;

    // Adds `much` spaces of an expanded tab, or of the visible part of a character cut by the left
    // edge, each pointing at that character's byte.
    void spaced(std::size_t was, int much) {
        for (int step = 0; step < much; ++step) {
            made.text.push_back(' ');
            made.bytes.push_back(was - line_at);
        }
    }

    // Adds a character, a whole cluster, taking `room` columns.
    void lettered(std::string_view sign, std::size_t was, int room) {
        made.text += sign;

        for (int step = 0; step < room; ++step) made.bytes.push_back(was - line_at);
    }
};

// Walks a row without building it, for stepping and for the memories, which need only where rows
// start and end.
struct Skipper {
    void spaced(std::size_t, int) {}

    void lettered(std::string_view, std::size_t, int) {}
};

// One screen row of a wrapped line, from byte `at`, at most `wide` columns. A tab advances to the
// next multiple of eight counted from the row's start, and a character that does not fit goes to
// the next row; a row is never empty unless its line is, so a character wider than the whole row
// stands in it alone. A line whose length is an exact multiple of the width ends with a full row,
// not with an empty one after it.
template <typename Taker>
Row wrapped_row(Spool& spool, std::size_t at, int wide, Taker& taker) {
    Reader reader{spool, at};

    Row made{at, at, at, false, false};

    int much = 0;

    while (true) {
        const std::optional<bool> end = reader.ending();

        if (!end.has_value()) {
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

        const std::size_t was = reader.at();
        const std::string_view sign = *reader.next();

        if (sign == "\t") {
            const int upto = std::min(((much / tabbing) + 1) * tabbing, wide);

            taker.spaced(was, upto - much);

            much = upto;

            continue;
        }

        const int room = width(sign);

        if (much > 0 && much + room > wide) {
            made.end = was;
            made.next = was;

            return made;
        }

        taker.lettered(sign, was, room);

        much += room;
    }
}

// The one screen row of a line without wrapping: the columns from `left` to `left + wide`, counted
// from the start of the line. `at` is the start of the character in which column `left` falls and
// `column` the column where that character starts. A tab advances to the next multiple of eight
// counted from the start of the line, as everywhere else; a tab or a double-width character cut
// by the left edge shows its visible part as spaces, and a character that does not fit at the right
// edge is left out. The row ends its line only when the line ends in view; where the next line
// starts is not looked for, because the rest of the line is not read.
template <typename Taker>
Row clipped_row(Spool& spool, std::size_t at, std::size_t column, std::size_t left, int wide, Taker& taker) {
    Reader reader{spool, at};

    Row made{at, at, at, false, false};

    std::size_t much = column;

    const std::size_t limit = left + static_cast<std::size_t>(wide);

    while (true) {
        const std::optional<bool> end = reader.ending();

        if (!end.has_value()) {
            made.waiting = true;

            return made;
        }

        if (*end || much >= limit) {
            made.end = reader.at();
            made.last = *end;

            return made;
        }

        const std::size_t was = reader.at();
        const std::string_view sign = *reader.next();

        const std::size_t room = sign == "\t" ? ((much / tabbing) + 1) * tabbing - much
                                              : static_cast<std::size_t>(width(sign));

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

// The order of marks: by line, and by start for equal lines. A mark is a range and a role, nothing
// more, exactly like Decoration.mark in CodeMirror. The range counts in bytes of the line. The set
// is kept sorted in this order, otherwise `next` would have no way to tell which mark comes next.
bool ahead(const Mark& one, const Mark& two) {
    return one.line != two.line ? one.line < two.line : one.from < two.from;
}

// Whether a cut shows this place: a byte from the cut's start up to its end, and on the last row of
// a line also everything past the end, where the cursor stands after the last character.
bool holds(const Cut& one, Place where) {
    if (one.line != where.line || where.byte < one.from) return false;

    return where.byte < one.upto || one.last || (where.byte == one.upto && one.bytes.empty());
}

// Where screen rows start in one wrapped line, for one width: every `every` rows (row number and
// byte), from row zero at the line's start, and the last row with the start of the next line once
// the line has been walked to its end.
struct Rows {
    std::size_t at = 0;
    std::size_t every = sampling;
    std::vector<std::pair<std::size_t, std::size_t>> starts;
    std::optional<std::pair<std::size_t, std::size_t>> last;
};

// At which byte columns of one line start: every `every` columns the first character starting at
// or past that column (column and byte), from column zero at the line's start.
struct Columns {
    std::size_t at = 0;
    std::size_t every = columning;
    std::vector<std::pair<std::size_t, std::size_t>> marks;
};

}

struct Page::Kept {
    // The byte document, or nothing before the first `hold`.
    std::shared_ptr<Spool> spool;

    // Intentions waiting for a spool window: "go to the end" and "stand on the row with this byte".
    // They live until they are carried out or until a new document arrives.
    bool ending = false;
    std::optional<std::size_t> sought;

    // The line at the top, where it starts once that is known, and where its top row starts (the
    // line's start without wrapping).
    std::size_t top = 0;
    std::optional<std::size_t> top_at;
    std::size_t row_at = 0;

    // The first column shown without wrapping.
    std::size_t left = 0;

    std::vector<Cut> shown;
    std::size_t walked = 0;

    // Whether there is still content past the last shown row. A row the spool does not have yet
    // counts as content.
    bool more = false;

    // A place that has to become visible, while it is not known where yet, because the page does
    // not know its size.
    std::optional<Place> wish;

    Size sized;
    bool wrapped = true;
    bool fresh = true;

    ScrollShape shape;

    std::string scrollbar_role = "talcite.scroll";
    std::string chosen = "talcite.viewer.mark.focus";

    // The scrollbar from the last frame and its unit: lines or bytes (see `gauged`).
    Grip grip;
    bool bytes = false;

    // The memories of long lines, by line number; the rows for the width `rowed`.
    std::unordered_map<std::size_t, Rows> rows;
    int rowed = -1;
    std::unordered_map<std::size_t, Columns> columns;

    // The width rows are cut to: what the last layout gave, at least one column.
    [[nodiscard]] int wide() const { return std::max(sized.wide, 1); }

    // Whether lines are cut into rows: with wrapping, once the page has a width. Before layout there
    // is nothing to cut, and a step goes by lines of the document.
    [[nodiscard]] bool cutting() const { return wrapped && sized.wide > 0; }

    // Makes sure the start of the top line is known; false while a window is on its way or when the
    // line lies past the end of the document.
    bool placed() {
        if (top_at.has_value()) return true;

        if (!spool) return false;

        top_at = spool->at(top);

        if (!top_at.has_value()) return false;

        row_at = *top_at;

        return true;
    }

    // Forgets the memories of lines far from the view once there are too many of them.
    void trimmed() {
        const auto far = [this](std::size_t line) { return line + lining / 2 < top || line > top + lining / 2; };

        if (rows.size() > lining) std::erase_if(rows, [&far](const auto& one) { return far(one.first); });

        if (columns.size() > lining) std::erase_if(columns, [&far](const auto& one) { return far(one.first); });
    }

    // The row memory of `line`, starting at `at`, for the current width; a memory from another
    // width or of a line that has since moved is started anew.
    Rows& rowed_line(std::size_t line, std::size_t at) {
        if (rowed != wide()) {
            rows.clear();
            rowed = wide();
        }

        trimmed();

        Rows& one = rows[line];

        if (one.starts.empty() || one.at != at) {
            one = Rows{};
            one.at = at;
            one.starts.push_back({0, at});
        }

        return one;
    }

    // Records that row `which` of a line starts at `at`, when it falls on the spacing; thins the
    // samples out when there are too many.
    static void sampled(Rows& into, std::size_t which, std::size_t at) {
        if (which % into.every != 0 || into.starts.back().first >= which) return;

        into.starts.push_back({which, at});

        if (into.starts.size() <= samples) return;

        std::vector<std::pair<std::size_t, std::size_t>> kept;

        for (std::size_t one = 0; one < into.starts.size(); one += 2) kept.push_back(into.starts[one]);

        into.starts = std::move(kept);
        into.every *= 2;
    }

    // Walks the rows of `line` from the latest remembered start not past byte `byte`, until `done`
    // says to stop at a row; returns that row's number and start, or nothing while a window is on
    // its way. Every row start on the spacing is remembered on the way, and so is the last row.
    template <typename Done>
    std::optional<std::pair<std::size_t, std::size_t>> rows_to(std::size_t line, std::size_t at, std::size_t byte, Done done) {
        Rows& memory = rowed_line(line, at);

        const auto past = std::upper_bound(memory.starts.begin(), memory.starts.end(), byte,
                                           [](std::size_t want, const auto& one) { return want < one.second; });

        std::pair<std::size_t, std::size_t> here = *std::prev(past);

        if (memory.last.has_value() && memory.last->second <= byte && memory.last->second >= here.second)
            here = *memory.last;

        while (true) {
            Skipper skip;

            const Row row = wrapped_row(*spool, here.second, wide(), skip);

            if (row.waiting) return {};

            if (row.last) memory.last = here;

            if (done(row)) return here;

            here = {here.first + 1, row.next};

            sampled(memory, here.first, here.second);
        }
    }

    // The start of the row holding byte `byte` of the wrapped line `line` starting at `at`; at the
    // end of the line, the last row.
    std::optional<std::size_t> row_holding(std::size_t line, std::size_t at, std::size_t byte) {
        const auto got = rows_to(line, at, byte, [byte](const Row& one) { return one.last || one.next > byte; });

        if (!got.has_value()) return {};

        return got->second;
    }

    // The start of the row before the one starting at `byte` in the wrapped line `line`.
    std::optional<std::size_t> row_before(std::size_t line, std::size_t at, std::size_t byte) {
        const auto got = rows_to(line, at, byte - 1, [byte](const Row& one) { return one.last || one.next >= byte; });

        if (!got.has_value()) return {};

        return got->second;
    }

    // The start of the last row of the wrapped line `line`.
    std::optional<std::size_t> last_row(std::size_t line, std::size_t at) {
        Rows& memory = rowed_line(line, at);

        if (memory.last.has_value()) return memory.last->second;

        const auto got = rows_to(line, at, SIZE_MAX, [](const Row& one) { return one.last; });

        if (!got.has_value()) return {};

        return got->second;
    }

    // The column memory of `line`, starting at `at`; a memory of a line that has since moved is
    // started anew.
    Columns& columned(std::size_t line, std::size_t at) {
        trimmed();

        Columns& one = columns[line];

        if (one.marks.empty() || one.at != at) {
            one = Columns{};
            one.at = at;
            one.marks.push_back({0, at});
        }

        return one;
    }

    // Walks `line` (starting at `at`) cluster by cluster from the latest remembered column mark that
    // `before` accepts, until `stop` says to stop before a cluster (given its column, width and byte)
    // or the line ends; returns the column and byte where it stopped, or nothing while a window is on
    // its way. Marks on the spacing are remembered on the way; a tab advances to the next multiple
    // of eight counted from the line's start.
    template <typename Before, typename Stop>
    std::optional<std::pair<std::size_t, std::size_t>> columns_to(std::size_t line, std::size_t at, Before before, Stop stop) {
        Columns& memory = columned(line, at);

        auto from = memory.marks.begin();

        for (auto one = memory.marks.begin(); one != memory.marks.end() && before(*one); ++one) from = one;

        std::size_t much = from->first;

        Reader reader{*spool, from->second};

        while (true) {
            const std::optional<bool> end = reader.ending();

            if (!end.has_value()) return {};

            if (*end) return std::pair{much, reader.at()};

            const std::size_t was = reader.at();
            const std::string_view sign = *reader.next();

            const std::size_t room = sign == "\t" ? ((much / tabbing) + 1) * tabbing - much
                                                  : static_cast<std::size_t>(width(sign));

            if (stop(much, room, was)) return std::pair{much, was};

            if (much / memory.every > memory.marks.back().first / memory.every) {
                memory.marks.push_back({much, was});

                if (memory.marks.size() > samples) {
                    std::vector<std::pair<std::size_t, std::size_t>> kept;

                    for (std::size_t one = 0; one < memory.marks.size(); one += 2) kept.push_back(memory.marks[one]);

                    memory.marks = std::move(kept);
                    memory.every *= 2;
                }
            }

            much += room;
        }
    }

    // The column of byte `byte`, which starts a character, in `line` (starting at `at`); past the end
    // of the line, the column after its last character.
    std::optional<std::size_t> column_at(std::size_t line, std::size_t at, std::size_t byte) {
        const auto got = columns_to(
            line, at, [byte](const auto& one) { return one.second <= byte; },
            [byte](std::size_t, std::size_t, std::size_t was) { return was >= byte; });

        if (!got.has_value()) return {};

        return got->first;
    }

    // The character of `line` (starting at `at`) in which column `column` falls: its column and
    // byte; past the end of the line, the column and byte of the end.
    std::optional<std::pair<std::size_t, std::size_t>> boundary_at(std::size_t line, std::size_t at, std::size_t column) {
        return columns_to(
            line, at, [column](const auto& one) { return one.first <= column; },
            [column](std::size_t much, std::size_t room, std::size_t) { return much + std::max<std::size_t>(room, 1) > column; });
    }

    // The column of byte `byte` in the wrapped row starting at `at`, counted from the row's start as
    // the row is drawn; past the end of the row, the column after its last character.
    std::optional<std::size_t> column_in(std::size_t at, std::size_t byte) {
        Reader reader{*spool, at};

        int much = 0;

        while (reader.at() < byte) {
            const std::optional<bool> end = reader.ending();

            if (!end.has_value()) return {};

            if (*end || much >= wide()) break;

            const std::string_view sign = *reader.next();

            if (sign == "\t") {
                much = std::min(((much / tabbing) + 1) * tabbing, wide());
            } else {
                const int room = width(sign);

                if (much > 0 && much + room > wide()) break;

                much += room;
            }
        }

        return static_cast<std::size_t>(much);
    }

    // The byte of the character in which column `column` falls in the wrapped row starting at `at`;
    // past the end of a row that ends its line, the end of the line, and past the end of a row that
    // does not, its last character, because the byte after it already belongs to the next row.
    std::optional<std::size_t> byte_in(std::size_t at, std::size_t column) {
        Reader reader{*spool, at};

        std::size_t much = 0;
        std::size_t last = at;

        const std::size_t wide_row = static_cast<std::size_t>(wide());

        while (true) {
            const std::optional<bool> end = reader.ending();

            if (!end.has_value()) return {};

            if (*end) return reader.at();

            if (much >= wide_row) return last;

            const std::size_t was = reader.at();
            const std::string_view sign = *reader.next();

            const std::size_t room = sign == "\t" ? std::min(((much / tabbing) + 1) * tabbing, wide_row) - much
                                                  : static_cast<std::size_t>(width(sign));

            if (much > 0 && sign != "\t" && much + room > wide_row) return last;

            if (much + std::max<std::size_t>(room, 1) > column) return was;

            last = was;
            much += room;
        }
    }

    // Where the row after the one starting at `at` in line `line` starts, as a line and a byte; nothing
    // at the end of the document or while a window is on its way (`waiting` then).
    std::optional<std::pair<std::size_t, std::size_t>> after(std::size_t line, std::size_t at, bool& waiting) {
        waiting = false;

        if (cutting()) {
            Skipper skip;

            const Row row = wrapped_row(*spool, at, wide(), skip);

            if (row.waiting) {
                waiting = true;

                return {};
            }

            if (!row.last) return std::pair{line, row.next};

            if (row.next >= spool->whole()) return {};

            return std::pair{line + 1, row.next};
        }

        const std::optional<Spool::Line> got = spool->line(line);

        if (!got.has_value()) {
            waiting = spool->stalled();

            return {};
        }

        if (got->next >= spool->whole()) return {};

        return std::pair{line + 1, got->next};
    }

    // One screen row down from the top; false at the end or while a window is on its way.
    bool down() {
        if (!placed()) return false;

        bool waiting = false;

        const auto next = after(top, row_at, waiting);

        if (!next.has_value()) return false;

        if (next->first != top) {
            top = next->first;
            top_at = next->second;
        }

        row_at = next->second;

        return true;
    }

    // One screen row up from the top; false at the start or while a window is on its way.
    bool up() {
        if (!placed()) return false;

        if (cutting() && row_at > *top_at) {
            const std::optional<std::size_t> went = row_before(top, *top_at, row_at);

            if (!went.has_value()) return false;

            row_at = *went;

            return true;
        }

        if (top == 0) return false;

        const std::optional<std::size_t> before = spool->at(top - 1);

        if (!before.has_value()) return false;

        std::size_t start = *before;

        if (cutting()) {
            const std::optional<std::size_t> last = last_row(top - 1, *before);

            if (!last.has_value()) return false;

            start = *last;
        }

        --top;
        top_at = *before;
        row_at = start;

        return true;
    }

    // How many screen rows there are from the top on, counting at most `most`; nothing while a window
    // is on its way, because then it is not known where the end is.
    std::optional<int> ahead_of(int most) {
        if (!placed()) return {};

        std::size_t line = top;
        std::size_t at = row_at;

        int made = 1;

        while (made < most) {
            bool waiting = false;

            const auto next = after(line, at, waiting);

            if (waiting) return {};

            if (!next.has_value()) break;

            line = next->first;
            at = next->second;

            ++made;
        }

        return made;
    }
};

// A page with the default scroll shape and the "talcite.viewer" role.
Page::Page() : kept_(std::make_unique<Kept>()) { role("talcite.viewer"); }

// A page with the given scroll shape and the "talcite.viewer" role.
Page::Page(ScrollShape shape) : kept_(std::make_unique<Kept>()) {
    role("talcite.viewer");

    kept_->shape = shape;
}

Page::~Page() = default;

// Shows a byte document read through a spool. The page holds the document, not the subclass: a
// subclass only supplies it, `Viewer` bytes or a byte source, `Editor` a spool over a quilt. A new
// document replaces the previous one entirely, together with the intentions and the memories of
// the old one.
//
// The spool wakes the page after every window: the frame is invalidated, because the page
// remembers what it showed, and the intentions waiting for a window (the end and the jump to a
// byte) complete by themselves. The callback holds the page by a raw pointer, because the page
// holds the spool; for the editor's spool it is never called, because that spool reads from memory
// and waits for nothing.
void Page::hold(std::shared_ptr<Spool> given) {
    Kept& kept = *kept_;

    kept.spool = std::move(given);
    kept.ending = false;
    kept.sought.reset();
    kept.top = 0;
    kept.top_at.reset();
    kept.row_at = 0;
    kept.left = 0;
    kept.rows.clear();
    kept.columns.clear();
    kept.fresh = true;

    (void)kept.placed();

    kept.spool->awoken([this] {
        Kept& now = *kept_;

        invalidate();

        if (now.sought.has_value()) {
            const std::size_t want = *now.sought;

            now.sought.reset();

            at(want);
        }

        if (now.ending && now.spool->counted().has_value()) {
            now.ending = false;

            end();
        }
    });

    (void)trigger(ScrollEvent{});
}

// The spool of the document, or an empty pointer before the first one.
const std::shared_ptr<Spool>& Page::spool() const { return kept_->spool; }

// Whether the page holds any document.
bool Page::held() const { return static_cast<bool>(kept_->spool); }

// The document line shown at the top, or nothing without a document.
std::optional<std::size_t> Page::first() const {
    if (!held()) return {};

    return kept_->top;
}

// The number of lines; the spool knows it only after reaching the end.
std::optional<std::size_t> Page::lines() const {
    const Kept& kept = *kept_;

    if (!kept.spool) return {};

    return kept.spool->counted();
}

// The byte at which the first visible row starts, which is what Midnight Commander shows in its
// viewer as the position in the file. Nothing without a document or while the start of the top
// line is not known yet.
std::optional<std::size_t> Page::at() const {
    const Kept& kept = *kept_;

    if (!kept.spool || !kept.top_at.has_value()) return {};

    return kept.row_at;
}

// The size of the document in bytes; nothing without a document.
std::optional<std::size_t> Page::whole() const {
    const Kept& kept = *kept_;

    if (!kept.spool) return {};

    return kept.spool->whole();
}

// Jumps to the row holding byte `given`. When the spool has not reached the byte because it waits
// for a window, the jump stays an intention and completes when the window arrives; the walk does
// not start again, it goes on from where it got.
void Page::at(std::size_t given) {
    Kept& kept = *kept_;

    if (!kept.spool) return;

    const std::size_t which = kept.spool->found(given);

    if (kept.spool->stalled()) {
        kept.sought = given;

        return;
    }

    const std::optional<std::size_t> from = kept.spool->at(which);

    if (!from.has_value()) {
        if (kept.spool->stalled()) kept.sought = given;

        return;
    }

    std::size_t start = *from;

    if (kept.wrapped && kept.sized.wide > 0) {
        const std::optional<std::size_t> row = kept.row_holding(which, *from, std::min(given, kept.spool->whole()));

        if (!row.has_value()) {
            kept.sought = given;

            return;
        }

        start = *row;
    }

    kept.top = which;
    kept.top_at = *from;
    kept.row_at = start;
    kept.fresh = true;

    (void)move_by(0);

    (void)trigger(ScrollEvent{});
}

// Puts document line `given` at the top, clamped to the last line when the count is known, and
// reports a `ScrollEvent` when the position changes.
void Page::first(std::size_t given) {
    Kept& kept = *kept_;

    if (!held()) return;

    const std::optional<std::size_t> many = lines();
    const std::size_t want = many.has_value() && *many > 0 && given >= *many ? *many - 1 : given;

    if (want == kept.top && kept.top_at.has_value() && kept.row_at == *kept.top_at) return;

    kept.top = want;
    kept.top_at.reset();
    kept.row_at = 0;
    kept.fresh = true;

    (void)kept.placed();

    (void)move_by(0);

    (void)trigger(ScrollEvent{});
}

// Scrolls so that the last line stands at the bottom of the screen. Without an index the end can
// be found only one way: by walking the document. The spool counts the lines along the way, and
// from then on `lines()` is no longer empty. With an asynchronous source the walk goes window by
// window and the jump to the end completes by itself once the count is known.
void Page::end() {
    Kept& kept = *kept_;

    if (!held()) return;

    if (!kept.spool->counted().has_value()) {
        kept.spool->counting();

        if (!kept.spool->counted().has_value()) {
            kept.ending = true;

            return;
        }
    }

    const std::size_t many = *kept.spool->counted();

    if (many == 0) return;

    const std::optional<std::size_t> from = kept.spool->at(many - 1);

    if (!from.has_value()) {
        if (kept.spool->stalled()) kept.ending = true;

        return;
    }

    std::size_t start = *from;

    if (kept.wrapped && kept.sized.wide > 0) {
        const std::optional<std::size_t> last = kept.last_row(many - 1, *from);

        if (!last.has_value()) {
            kept.ending = true;

            return;
        }

        start = *last;
    }

    kept.top = many - 1;
    kept.top_at = *from;
    kept.row_at = start;
    kept.fresh = true;

    (void)move_by(-std::max(kept.sized.high - 1, 0));

    (void)trigger(ScrollEvent{});
}

// Moves the view by `rows` screen rows without notifying anyone; returns whether the position
// changed. A step counts in screen rows, not document lines, because that is how far the picture
// moves; with wrapping one document line is several steps. It does not notify, because a jump to
// a mark scrolls and changes the current mark in one move, so it must send one event, not two.
//
// The screen does not go past the end: a step down stops when the last row stands at the bottom of
// the screen, not at the top with emptiness below it. The clamping counts forward only as far as
// the screen is high, and works for zero steps too; that is how a `first()` jump to the last page
// is brought back into order. Before layout there is no height and nothing to clamp. A row the
// spool does not have yet is not the end: a step stops at what there is, and the clamping does
// nothing, because it is not known where the end is.
bool Page::move_by(int rows) {
    Kept& kept = *kept_;

    if (!held()) return false;

    const std::size_t was = kept.top;
    const std::size_t was_at = kept.row_at;

    for (int one = 0; one < rows; ++one)
        if (!kept.down()) break;

    for (int one = 0; one > rows; --one)
        if (!kept.up()) break;

    if (const int high = kept.sized.high; high > 0)
        if (const std::optional<int> many = kept.ahead_of(high); many.has_value())
            for (int missing = high - *many; missing > 0; --missing)
                if (!kept.up()) break;

    if (kept.top == was && kept.row_at == was_at) return false;

    kept.fresh = true;

    return true;
}

// Scrolls by `rows` screen rows and reports a `ScrollEvent` when the view moved; returns whether it
// moved. The notification travels the ordinary event path, so it bubbles up to the ancestors.
bool Page::slide(int rows) {
    if (!move_by(rows)) return false;

    (void)trigger(ScrollEvent{});

    return true;
}

// Scrolls by `pages` screen heights.
void Page::page(int pages) { slide(pages * std::max(kept_->sized.high, 1)); }

// The first column shown without wrapping.
std::size_t Page::left() const { return kept_->left; }

// Scrolls sideways so that column `columns` is the first shown; it matters only without wrapping.
void Page::left(std::size_t columns) {
    Kept& kept = *kept_;

    if (kept.left == columns) return;

    kept.left = columns;
    kept.fresh = true;

    (void)trigger(ScrollEvent{});
}

// Whether long lines are wrapped.
bool Page::wrapped() const { return kept_->wrapped; }

// Turns wrapping on or off. The same byte means something else in the two modes, so the view goes
// back to the start of the line it stands on. Without that, switching would throw the screen to a
// place nobody asked for.
void Page::wrapped(bool given) {
    Kept& kept = *kept_;

    if (kept.wrapped == given) return;

    kept.wrapped = given;
    kept.left = 0;

    if (kept.top_at.has_value()) kept.row_at = *kept.top_at;

    kept.fresh = true;

    (void)trigger(ScrollEvent{});
}

// Whether the page draws a scrollbar.
bool Page::scrollbar() const { return kept_->shape.scrollbar; }

// Turns the scrollbar on or off.
void Page::scrollbar(bool given) {
    if (kept_->shape.scrollbar == given) return;

    kept_->shape.scrollbar = given;
    kept_->fresh = true;
}

// How many document lines the last frame showed.
std::size_t Page::shown() const { return kept_->walked; }

// Makes mark `which` the current one and scrolls to it as little as possible: a mark already
// visible does not move the picture, one above lands at the top, one below at the bottom. One event
// is sent, even when the move consists of two steps.
bool Page::reach(std::size_t which) {
    MarkedMixin::which(which);

    (void)reveal(Place{marks()[which].line, marks()[which].from});

    (void)trigger(ScrollEvent{});

    return true;
}

// The column (counted from the start of the line, a tab to the next multiple of eight) where the
// character at this place starts; past the end of the line, the column after it. Nothing while a
// window is on its way. This is the column of a status line; what it walks it remembers.
std::optional<std::size_t> Page::column_in_line(Place where) {
    Kept& kept = *kept_;

    if (!kept.spool) return {};

    const std::optional<std::size_t> from = kept.spool->at(where.line);

    if (!from.has_value()) return {};

    return kept.column_at(where.line, *from, *from + where.byte);
}

// The column where the character at this place starts, counted from the start of its screen row
// as the row is drawn (with wrapping a tab goes to the next multiple of eight from the row's
// start); without wrapping a row is the whole line, so this is the column in the line. Nothing
// while a window is on its way.
std::optional<std::size_t> Page::column_in_row(Place where) {
    Kept& kept = *kept_;

    if (!kept.spool) return {};

    const std::optional<std::size_t> from = kept.spool->at(where.line);

    if (!from.has_value()) return {};

    if (!kept.cutting()) return kept.column_at(where.line, *from, *from + where.byte);

    const std::optional<std::size_t> row = kept.row_holding(where.line, *from, *from + where.byte);

    if (!row.has_value()) return {};

    return kept.column_in(*row, *from + where.byte);
}

// The place `rows` screen rows away from the row of `from` (up when negative), at column `column` of
// that row: the start of the character in which the column falls, or the end of a shorter row. The
// walk stops at the edges of the document, so a page down near the end lands on the last row;
// nothing when there was nowhere to go at all, or while a window is on its way. The end of a row
// that does not end its line is the start of the next row, so a column past such a row lands on
// its last character instead. Without wrapping a row is a line.
std::optional<Place> Page::rows_away(Place from, int rows, std::size_t column) {
    Kept& kept = *kept_;

    if (!kept.spool || rows == 0) return {};

    std::size_t line = from.line;

    const std::optional<std::size_t> start = kept.spool->at(line);

    if (!start.has_value()) return {};

    std::size_t line_at = *start;
    std::size_t at = *start;

    if (kept.cutting()) {
        const std::optional<std::size_t> row = kept.row_holding(line, line_at, line_at + from.byte);

        if (!row.has_value()) return {};

        at = *row;
    }

    int moved = 0;

    for (; moved < rows; ++moved) {
        bool waiting = false;

        const auto next = kept.after(line, at, waiting);

        if (waiting) return {};

        if (!next.has_value()) break;

        if (next->first != line) line_at = next->second;

        line = next->first;
        at = next->second;
    }

    for (; moved > rows; --moved) {
        if (kept.cutting() && at > line_at) {
            const std::optional<std::size_t> before = kept.row_before(line, line_at, at);

            if (!before.has_value()) return {};

            at = *before;

            continue;
        }

        if (line == 0) break;

        const std::optional<std::size_t> above = kept.spool->at(line - 1);

        if (!above.has_value()) return {};

        --line;
        line_at = *above;
        at = *above;

        if (kept.cutting()) {
            const std::optional<std::size_t> last = kept.last_row(line, line_at);

            if (!last.has_value()) return {};

            at = *last;
        }
    }

    if (moved == 0) return {};

    if (!kept.cutting()) {
        const std::optional<std::pair<std::size_t, std::size_t>> got = kept.boundary_at(line, line_at, column);

        if (!got.has_value()) return {};

        return Place{line, got->second - line_at};
    }

    const std::optional<std::size_t> byte = kept.byte_in(at, column);

    if (!byte.has_value()) return {};

    return Place{line, *byte - line_at};
}

// Scrolls as little as possible to make this place visible: a place already visible does not move
// the picture, one above lands at the top, one below at the bottom, and without wrapping one to the
// side of the view brings the view sideways just as far as needed. Returns whether the view changed.
// It does not notify; the caller does, so that a compound move gives one event.
bool Page::reveal(Place where) {
    Kept& kept = *kept_;

    // Before the page gets its allotment there is no way to tell whether the place is visible
    // anyway: the height is zero, so every line except the first would look hidden and the picture
    // would jump for no reason. The question is postponed until the picture is assembled, when the
    // size is known.
    if (kept.sized.high <= 0 || kept.sized.wide <= 0) {
        kept.wish = where;
        kept.fresh = true;

        return true;
    }

    kept.wish.reset();

    if (!kept.spool) return false;

    bool moved = false;

    if (!kept.wrapped) {
        if (const std::optional<std::size_t> column = column_in_line(where); column.has_value()) {
            const std::size_t wide = static_cast<std::size_t>(kept.sized.wide);

            if (*column < kept.left) {
                kept.left = *column;
                moved = true;
            } else if (*column >= kept.left + wide) {
                kept.left = *column - wide + 1;
                moved = true;
            }
        }
    }

    // Whether the place is visible is read from the laid out rows, so a layout gone stale (the view
    // moved since the last frame) is laid out again first.
    if (kept.fresh) spread(kept.sized);

    if (std::any_of(kept.shown.begin(), kept.shown.end(), [where](const Cut& one) { return holds(one, where); })) {
        if (moved) kept.fresh = true;

        return moved;
    }

    const std::optional<std::size_t> from = kept.spool->at(where.line);

    if (!from.has_value()) {
        if (moved) kept.fresh = true;

        return moved;
    }

    std::size_t start = *from;

    if (kept.wrapped) {
        const std::optional<std::size_t> row = kept.row_holding(where.line, *from, *from + where.byte);

        if (row.has_value()) start = *row;
    }

    const bool above = where.line < kept.top || (where.line == kept.top && start < kept.row_at);

    kept.top = where.line;
    kept.top_at = *from;
    kept.row_at = start;
    kept.fresh = true;

    if (!above) (void)move_by(-std::max(kept.sized.high - 1, 0));

    return true;
}

// Forces the next frame to lay the document out again.
void Page::invalidate() { kept_->fresh = true; }

// The text changed from line `line` on: the memories of rows and columns from that line on are no
// longer true, and when the change reaches the top line, its start is looked up again.
void Page::changed(std::size_t line) {
    Kept& kept = *kept_;

    std::erase_if(kept.rows, [line](const auto& one) { return one.first >= line; });
    std::erase_if(kept.columns, [line](const auto& one) { return one.first >= line; });

    if (line <= kept.top) {
        kept.top_at.reset();
        kept.row_at = 0;
    }

    kept.fresh = true;
}

// The document line at the top of the view.
std::size_t Page::topmost() const { return kept_->top; }

// The cell on which this byte of this line stands, or nothing when it is off screen. The editor
// takes the cursor position from here.
std::optional<Spot> Page::spot_of(Place where) const {
    const Kept& kept = *kept_;

    for (std::size_t one = 0; one < kept.shown.size(); ++one) {
        const Cut& piece = kept.shown[one];

        if (!holds(piece, where)) continue;

        for (std::size_t col = 0; col < piece.bytes.size(); ++col)
            if (piece.bytes[col] == where.byte) return Spot{static_cast<int>(col), static_cast<int>(one)};

        // Past the last character of the row: where the cursor stands at the end of the line. A
        // place inside a character that is cut by the left edge stands on the first column.
        if (piece.bytes.empty() || where.byte > piece.bytes.back())
            return Spot{static_cast<int>(piece.bytes.size()), static_cast<int>(one)};

        return Spot{0, static_cast<int>(one)};
    }

    return {};
}

// The inverse of `spot_of`: from a cell to a document line and a byte in it. A screen row is not a
// line number, because with wrapping one line occupies several, and a column is not a byte,
// because a tab occupies eight cells and a Chinese character two. Cutting into pieces has already
// computed both maps, so all that is left here is reading them.
std::optional<Place> Page::place_of(Spot where) const {
    const Kept& kept = *kept_;

    if (where.y < 0 || static_cast<std::size_t>(where.y) >= kept.shown.size()) return {};

    const Cut& piece = kept.shown[static_cast<std::size_t>(where.y)];

    const std::size_t col = static_cast<std::size_t>(std::max(where.x, 0));

    // Past the last character of the piece: the cursor stands just past it, that is, where the
    // next piece starts or where the line ends.
    if (col >= piece.bytes.size()) return Place{piece.line, piece.upto};

    return Place{piece.line, piece.bytes[col]};
}

// The role in which the current mark is painted. The current mark is painted in a different role
// than the others. The viewer knows which one it stands on, so it substitutes the role itself; the
// application does not have to pass the marks again just so that one of them looks different.
const std::string& Page::chosen() const { return kept_->chosen; }

// Replaces the role in which the current mark is painted.
void Page::chosen(std::string given) { kept_->chosen = std::move(given); }

// The role in which the scrollbar is painted.
const std::string& Page::scrollbar_role() const { return kept_->scrollbar_role; }

// Replaces the role in which the scrollbar is painted.
void Page::scrollbar_role(std::string given) { kept_->scrollbar_role = std::move(given); }

// A page always takes the focus, so that the keys can scroll it.
bool Page::focusable() const { return true; }

// The viewer takes no children; it is the content itself.
int Page::capacity() const { return 0; }

// A page takes the whole room it is offered.
Size Page::wants(Size room) const { return room; }

// Lays out the visible rows for an area of size `given`. It recomputes only when the allotment or
// the position changed. Without that, every frame refresh would read the document again, and with
// an animated neighbor that happens ten times a second. Each row reads only the bytes it shows.
void Page::spread(Size given) {
    Kept& kept = *kept_;

    if (!kept.fresh && kept.sized == given) return;

    kept.sized = given;
    kept.fresh = false;
    kept.shown.clear();
    kept.more = false;
    kept.walked = 0;

    if (!kept.spool || given.wide <= 0 || given.high <= 0) return;

    if (kept.wish.has_value()) {
        (void)reveal(*kept.wish);

        kept.fresh = false;
    }

    // The spacing of the spool's anchors comes from the window height; the spool knows nothing
    // about the view.
    kept.spool->screen(given.high);

    if (!kept.placed()) {
        kept.more = kept.spool->stalled();

        return;
    }

    const std::size_t want = static_cast<std::size_t>(given.high);

    std::size_t line = kept.top;
    std::size_t line_at = *kept.top_at;
    std::size_t at = kept.row_at;

    while (kept.shown.size() < want) {
        Builder builder;

        builder.line_at = line_at;

        Row row;

        if (kept.wrapped) {
            row = wrapped_row(*kept.spool, at, given.wide, builder);
        } else {
            const std::optional<std::pair<std::size_t, std::size_t>> from = kept.boundary_at(line, line_at, kept.left);

            if (!from.has_value()) {
                kept.more = true;

                break;
            }

            row = clipped_row(*kept.spool, from->second, from->first, kept.left, given.wide, builder);
        }

        if (row.waiting) {
            kept.more = true;

            break;
        }

        Cut made = std::move(builder.made);

        made.line = line;
        made.at = at;
        made.end = row.end;
        made.from = at - line_at;
        made.upto = row.end - line_at;
        made.last = row.last;

        kept.shown.push_back(std::move(made));

        // Where the next row starts: with wrapping the row just walked says it, without wrapping the
        // spool, which walks the rest of the line to its end once and remembers it.
        bool waiting = false;

        std::optional<std::pair<std::size_t, std::size_t>> next;

        if (!kept.wrapped)
            next = kept.after(line, at, waiting);
        else if (!row.last)
            next = std::pair{line, row.next};
        else if (row.next < kept.spool->whole())
            next = std::pair{line + 1, row.next};

        if (!next.has_value()) {
            kept.more = waiting;

            // The rows reached the end of the document from a known line, so the line count is
            // known too, without walking the document again.
            if (!waiting && row.last) kept.spool->counted(line + 1);

            break;
        }

        if (kept.shown.size() >= want) {
            kept.more = true;

            break;
        }

        if (next->first != line) line_at = next->second;

        line = next->first;
        at = next->second;
    }

    kept.walked = kept.shown.empty() ? 0 : kept.shown.back().line - kept.top + 1;
}

// Fills in the scrollbar's position, whole and window; returns false when there is nothing to
// measure. The scrollbar counts lines when their number is known and they are short on average,
// and bytes otherwise: bytes are exact from the moment the file is opened, and in a document of a
// few very long lines they keep the thumb moving, where lines would hold it still for the whole
// length of each one.
bool Page::gauged(std::size_t& shift, std::size_t& whole, std::size_t& window) const {
    const Kept& kept = *kept_;

    if (!kept.spool) return false;

    const std::size_t much = kept.spool->whole();
    const std::optional<std::size_t> many = lines();

    if (many.has_value() && *many > 0 && *many * short_line >= much) {
        shift = kept.top;
        whole = *many;
        window = static_cast<std::size_t>(std::max(kept.sized.high, 1));

        return true;
    }

    if (much == 0 || !kept.top_at.has_value()) return false;

    // The window in bytes is the span of what currently stands on the screen: from the start of the
    // first row to the end of the last one.
    shift = kept.row_at;
    whole = much;
    window = kept.shown.empty() ? 0 : kept.shown.back().end - kept.row_at;

    return true;
}

namespace {

// Overlays the marks on one drawn piece. Marks are overlaid after the content is drawn, cell by
// cell: the letter already there stays, and the style is merged through `Style::merge`. Thanks to
// that, a role that sets only the background keeps the color of the letters, and a role that sets
// only the letters keeps the background. A mark with an address also lays its hyperlink over its
// cells (`View::link`). Only the marks of the piece's line are looked at; they are found by
// bisection, since the marks are sorted.
//
// A cell with a zero letter is the continuation of a double-width character; it is skipped,
// because the first half has already repainted both.
void painted(View& into, const Cut& piece, Spot at, const Style& look, const std::vector<Mark>& marks,
             std::optional<std::size_t> which, const std::string& chosen, const Skin& how) {
    const auto first = std::lower_bound(marks.begin(), marks.end(), Mark{piece.line, 0, 0, {}}, ahead);

    for (auto it = first; it != marks.end() && it->line == piece.line; ++it) {
        const Mark& one = *it;

        if (one.upto <= one.from || one.upto <= piece.from || one.from >= piece.upto) continue;

        const std::size_t nth = static_cast<std::size_t>(it - marks.begin());
        const bool here = which.has_value() && *which == nth;

        const Style over = Style::merge(look, how.style(here ? chosen : one.role));
        const Hyperlink target = one.address.empty() ? Hyperlink{} : hyperlink(one.address);

        for (std::size_t col = 0; col < piece.bytes.size(); ++col) {
            const std::size_t byte = piece.bytes[col];

            if (byte < one.from || byte >= one.upto) continue;

            const Spot spot{at.x + static_cast<int>(col), at.y};
            const Cell said = into.at(spot);

            if (said.letter == 0) continue;

            into.put(spot, said.letter, over);

            if (target.mark != 0) into.link({spot, {width(said.letter) == 2 ? 2 : 1, 1}}, target);
        }
    }
}

}

// Draws the visible pieces, the marks over them and, when it is on, the scrollbar.
View Page::paint(Size given, Area seen) {
    settle();

    View made{seen.size};

    made.clear({{0, 0}, seen.size});

    // The scrollbar takes one column on the right, so the content gets that much less room, just
    // as in the scrollers.
    int aside = kept_->shape.scrollbar && given.wide > 0 ? 1 : 0;

    spread({given.wide - aside, given.high});

    // The "when needed" scrollbar (`needed`): a page that fits entirely from the first line gives
    // its column back to the content and lays itself out once more. Text wrapped wider then takes
    // the same number of rows or fewer, so it still fits.
    if (aside != 0 && kept_->shape.needed && kept_->top == 0 && kept_->top_at.has_value() &&
        kept_->row_at == *kept_->top_at && !kept_->more) {
        aside = 0;

        spread({given.wide, given.high});
    }

    if (kept_->shown.empty() && aside == 0) return made;

    const Style look = skin().style(role());

    made.fill({{0, 0}, seen.size}, look);

    for (std::size_t one = 0; one < kept_->shown.size(); ++one) {
        const Cut& piece = kept_->shown[one];
        const int y = static_cast<int>(one) - seen.at.y;

        made.put({-seen.at.x, y}, piece.text, look);

        if (!marks().empty())
            painted(made, piece, {-seen.at.x, y}, look, marks().all(), which(), kept_->chosen, skin());
    }

    std::size_t shift = 0;
    std::size_t whole = 0;
    std::size_t window = 0;

    if (aside != 0 && gauged(shift, whole, window)) {
        const Scrollbar bar{.at = Scrollbar::edge(given, seen, true),
                            .much = given.high,
                            .shift = shift,
                            .whole = whole,
                            .window = window,
                            .focused = Scrollbar::focused_in(*this),
                            .skin = skin(),
                            .role = kept_->scrollbar_role};

        bar.draw(made);

        Track mine = bar.track();

        mine.at = Scrollbar::edge(given, {{0, 0}, given}, true);

        kept_->grip.laid(mine);

        const std::optional<std::size_t> many = lines();

        kept_->bytes = !(many.has_value() && *many > 0 && *many * short_line >= kept_->spool->whole());
    } else {
        kept_->grip.laid(std::nullopt);
    }

    return made;
}

// Scrolls with the arrows, PageUp, PageDown, Home and End; without wrapping, the arrows to the
// sides scroll sideways by a column, and Home goes back to the first column too. Enter follows the
// current mark when it has an address (`FollowingMixin::followed`): stepping from mark to mark
// (`MarkedMixin::step`) is how a key reaches the links of a page. It returns false for everything it
// did not use: the F-keys have to reach the legend, which is an ancestor, and with wrapping the
// side arrows go up as well (an application may give them to something else).
bool Page::handle(KeyEvent& what) {
    // Nothing with a modifier is taken: Ctrl+Home and Alt+PageDown are by design different
    // shortcuts than Home and PageDown, so the combination is passed on to the application.
    if (what.mods().ctrl || what.mods().alt) return false;

    switch (what.key().code()) {
        case Code::enter: {
            const std::optional<std::size_t> current = which();

            if (!current || marks()[*current].address.empty()) return false;

            followed(marks()[*current].address);

            return true;
        }

        case Code::up: slide(-1); return true;
        case Code::down: slide(1); return true;
        case Code::pageup: page(-1); return true;
        case Code::pagedown: page(1); return true;

        case Code::home:
            if (!kept_->wrapped) left(0);

            first(0);
            return true;

        case Code::end: end(); return true;

        case Code::left:
            if (kept_->wrapped) return false;

            if (kept_->left > 0) left(kept_->left - 1);
            return true;

        case Code::right:
            if (kept_->wrapped) return false;

            left(kept_->left + 1);
            return true;

        default: return false;
    }
}

// Scrolls `stepping` rows per wheel step; returns whether the view moved. Without wrapping the
// tilted wheel scrolls sideways; with wrapping it goes on up.
bool Page::handle(WheelEvent& what) {
    if (what.axis() == Axis::horizontal) {
        if (kept_->wrapped) return false;

        const std::size_t was = kept_->left;
        const std::ptrdiff_t to = static_cast<std::ptrdiff_t>(was) - static_cast<std::ptrdiff_t>(what.steps() * stepping);

        left(static_cast<std::size_t>(std::max<std::ptrdiff_t>(to, 0)));

        return kept_->left != was;
    }

    if (what.axis() != Axis::vertical) return false;

    const std::size_t was = kept_->top;
    const std::size_t was_at = kept_->row_at;

    slide(-what.steps() * stepping);

    return kept_->top != was || kept_->row_at != was_at;
}

// Takes the focus on a click, and a click with the left button on a mark with an address follows it
// (`FollowingMixin::followed`); a click with Ctrl is left to the terminal, which opens a hyperlink
// itself, as on a link (`Link`). The guard on the root is there because a widget is sometimes
// rendered in a test without a root.
bool Page::handle(ClickEvent& what) {
    if (root() != nullptr) focus();

    if (what.mods().ctrl || what.button() != Mouse::left) return true;

    const std::optional<Place> place = place_of(local(what.where()));

    if (!place) return true;

    const auto found = std::find_if(marks().begin(), marks().end(), [&place](const Mark& one) {
        return !one.address.empty() && one.line == place->line && place->byte >= one.from && place->byte < one.upto;
    });

    if (found != marks().end()) followed(found->address);

    return true;
}

// The mouse on the scrollbar: the track scrolls a page, and the thumb jumps to where it was put, to
// a line when the scrollbar counts lines and to a byte when it counts bytes.
bool Page::handle(MouseEvent& what) {
    const Asked asked = kept_->grip.mouse(what, local(what.where()), *this);

    switch (asked.part) {
        case Part::none: return false;
        case Part::before: page(-1); break;
        case Part::after: page(1); break;
        case Part::thumb:
            if (asked.to.has_value()) {
                if (kept_->bytes) at(*asked.to);
                else first(*asked.to);
            }
            break;
    }

    return true;
}

}
