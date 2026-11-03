#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "talcite/encoding.hpp"
#include "talcite/mark.hpp"
#include "talcite/shape.hpp"
#include "talcite/view.hpp"

namespace talcite::ui {

// Trimming strings to a number of cells. It lives in one place because both the label and the
// key bar need it; a similar template repeated in several files violates the one-definition rule,
// which the linker does not report.

// Tabs are everywhere in licenses and manual pages. We expand them when drawing, not in the data:
// if a tab expansion entered the line's content, byte positions would no longer match the file.
constexpr int tabbing = 8;

// Whether the byte at `at` is a cluster of its own, one cell wide: an ASCII character not followed
// by a byte that could join it. A control character counts too, shown as its picture (`shown`).
[[nodiscard]] inline bool single(std::string_view what, std::size_t at) {
    return static_cast<unsigned char>(what[at]) < 0x80 && (at + 1 == what.size() || static_cast<unsigned char>(what[at + 1]) < 0x80);
}

// How many cells the start of a line takes up to the given byte. A tab advances to the next
// multiple of eight, and every other cluster takes the cells it takes (`width`): a double-width
// character two, a letter with its combining marks one. The same rule by which `Page` cuts a line
// into pieces. It lives here so that it exists in one place: if the cutting and the status bar
// counted separately, sooner or later they would show two different columns for the same cursor.
//
// No window width is involved, so a tab is not clipped at the edge. `Page` does that because it
// has to fit a piece; counting a column asks about the line, not the screen.
[[nodiscard]] inline int columns(std::string_view what, std::size_t upto) {
    int much = 0;
    std::size_t at = 0;

    while (at < what.size() && at < upto) {
        if (what[at] != '\t' && single(what, at)) {
            ++much;
            ++at;

            continue;
        }

        const std::size_t next = cluster_after(what, at);

        if (what[at] == '\t')
            much = ((much / tabbing) + 1) * tabbing;
        else
            much += width(what.substr(at, next - at));

        at = next;
    }

    return much;
}

// The head of a string that fits in the given number of cells: how many bytes it has, and how many
// cells they take. We count by clusters, not bytes or characters, and by their widths; otherwise
// the first CJK character would break the wrapping, and a cut between a letter and its accent would
// leave the accent for the next row.
inline std::pair<std::size_t, int> fitting(std::string_view what, int wide) {
    std::size_t at = 0;
    int much = 0;

    while (at < what.size()) {
        const bool lone = single(what, at);
        const std::size_t next = lone ? at + 1 : cluster_after(what, at);
        const int one = lone ? 1 : width(what.substr(at, next - at));

        if (much + one > wide) break;

        much += one;
        at = next;
    }

    return {at, much};
}

// The head itself (`fitting`).
inline std::string cut(std::string_view what, int wide) { return std::string{what.substr(0, fitting(what, wide).first)}; }

// The same from the end: the tail of the given width, whole clusters only.
inline std::string tail(std::string_view what, int wide) {
    std::size_t at = what.size();
    int room = 0;

    while (at > 0) {
        const std::size_t before = cluster_before(what, at);
        const int one = width(what.substr(before, at - before));

        if (room + one > wide) break;

        room += one;
        at = before;
    }

    return std::string{what.substr(at)};
}

// Shortens a string to the given width with an ellipsis on the side chosen by `how`. The ellipsis
// takes one cell, so the text gets one cell less. At a width of one cell only the ellipsis
// remains; that is the only honest answer, because any other would pretend that some of the
// content is visible.
//
// The caller supplies the mark, not this function: it takes it from the skin, like any other
// glyph. The argument is required precisely so that no hard-coded ellipsis goes unnoticed.
inline std::string elided(const std::string& one, int wide, Elide how, char32_t mark) {
    if (how == Elide::none) return one;

    if (measured(one).wide <= wide) return one;

    if (wide <= 0) return {};

    const std::string dots = utf8::encode(mark);

    if (wide == 1) return dots;

    if (how == Elide::right) return cut(one, wide - 1) + dots;

    if (how == Elide::left) return dots + tail(one, wide - 1);

    const int half = (wide - 1) / 2;

    return cut(one, half) + dots + tail(one, wide - 1 - half);
}


// No byte of the text: what eliding puts in (the ellipsis).
inline constexpr std::size_t nowhere = static_cast<std::size_t>(-1);

// A drawn row of text that knows, for every byte, the byte of its paragraph it came from
// (`nowhere` for the ellipsis), and which paragraph that is. Marks count bytes of the paragraph
// (`Mark`), while a drawn row is wrapped and elided, so its own byte numbers say nothing about the
// text without this. The label wraps its paragraphs into such rows; a table cell is one.
struct Traced {
    std::string text;
    std::size_t line = 0;
    std::vector<std::size_t> bytes;

    // Appends a stretch of the paragraph that starts at its byte `from`.
    void taken(std::string_view what, std::size_t from) {
        text += what;

        for (std::size_t at = 0; at < what.size(); ++at) bytes.push_back(from + at);
    }

    // Appends what the paragraph does not have, such as the ellipsis.
    void added(std::string_view what) {
        text += what;
        bytes.insert(bytes.end(), what.size(), nowhere);
    }
};

// A whole paragraph as one row, every byte its own.
[[nodiscard]] inline Traced traced(std::string_view text, std::size_t line) {
    Traced made;

    made.line = line;
    made.taken(text, 0);

    return made;
}

// The row shortened to `wide` with an ellipsis on the side `how` says, with what each byte came
// from; the same cut as `elided` of a string, which makes the text alone.
[[nodiscard]] inline Traced elided(const Traced& one, int wide, Elide how, char32_t mark) {
    if (how == Elide::none || measured(one.text).wide <= wide) return one;

    Traced made;

    made.line = one.line;

    if (wide <= 0) return made;

    const std::string dots = utf8::encode(mark);

    // The head and the tail of the row kept whole, with where their bytes came from.
    const auto head = [&one, &made](int room) {
        const std::string kept = cut(one.text, room);

        made.text += kept;
        made.bytes.insert(made.bytes.end(), one.bytes.begin(), one.bytes.begin() + static_cast<std::ptrdiff_t>(kept.size()));
    };

    const auto tailed = [&one, &made](int room) {
        const std::string kept = tail(one.text, room);

        made.text += kept;
        made.bytes.insert(made.bytes.end(), one.bytes.end() - static_cast<std::ptrdiff_t>(kept.size()), one.bytes.end());
    };

    if (wide == 1) {
        made.added(dots);

        return made;
    }

    if (how == Elide::right) {
        head(wide - 1);
        made.added(dots);
    } else if (how == Elide::left) {
        made.added(dots);
        tailed(wide - 1);
    } else {
        const int half = (wide - 1) / 2;

        head(half);
        made.added(dots);
        tailed(wide - 1 - half);
    }

    return made;
}

// Calls `each` for every cluster of a drawn row that came from the text, with its column (the row
// starting at `x`), its width, its glyph and the paragraph byte it came from, advancing as
// `View::put` does.
template <typename Each>
void walked(const Traced& one, int x, Each each) {
    for (std::size_t at = 0; at < one.text.size();) {
        const std::size_t next = cluster_after(one.text, at);
        const char32_t letter = glyph(std::string_view{one.text}.substr(at, next - at));

        if (letter != 0) {
            const int much = width(letter);

            if (one.bytes[at] != nowhere) each(x, much, letter, one.bytes[at]);

            x += much;
        }

        at = next;
    }
}

// Whether the mark covers the paragraph byte of the line.
[[nodiscard]] inline bool covers(const Mark& one, std::size_t line, std::size_t byte) {
    return one.line == line && byte >= one.from && byte < one.upto;
}

}
