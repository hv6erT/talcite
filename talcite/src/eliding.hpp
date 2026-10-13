#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "talcite/encoding.hpp"
#include "talcite/shape.hpp"
#include "talcite/view.hpp"

namespace talcite::ui {

// Trimming strings to a number of cells. It lives in one place because both the label and the
// legend need it; a similar template repeated in several files violates the one-definition rule,
// which the linker does not report.

// Tabs are everywhere in licenses and manual pages. We expand them when drawing, not in the data:
// if a tab expansion entered the line's content, byte positions would no longer match the file.
constexpr int tabbing = 8;

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
        const std::size_t next = cluster_after(what, at);

        if (what[at] == '\t')
            much = ((much / tabbing) + 1) * tabbing;
        else
            much += width(what.substr(at, next - at));

        at = next;
    }

    return much;
}

// The head of a string that fits in the given number of cells. We count by clusters, not bytes
// or characters, and by their widths; otherwise the first CJK character would break the wrapping,
// and a cut between a letter and its accent would leave the accent for the next row.
inline std::string cut(std::string_view what, int wide) {
    std::size_t at = 0;
    int much = 0;

    while (at < what.size()) {
        const std::size_t next = cluster_after(what, at);
        const int one = width(what.substr(at, next - at));

        if (much + one > wide) break;

        much += one;
        at = next;
    }

    return std::string{what.substr(0, at)};
}

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

}
