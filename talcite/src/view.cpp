#include "talcite/view.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "talcite/encoding.hpp"

namespace talcite {

// Two styles in one: the upper one adds but does not take away. A transparent color lets through
// what is below, a set one covers it; the attribute flags add up.
//
// This exists so that a widget from outside the library can have its own fallback look:
// merge(mine, from the skin) gives my look when the skin has not heard of the role, and the skin's
// look where it said something. Adding the flags up instead of replacing them follows directly
// from this use: a role that is not set gives an empty style, and replacing would wipe out with it
// what the caller gave.
//
// The price is that the upper style cannot switch off a flag of the lower one. `Style` has no
// "not set" state for the flags (only the colors have one), so "no opinion" cannot be told apart
// from "must be off" without adding that state to a type that all drawing passes through.
Style Style::merge(Style base, Style over) {
    Style made = base;

    if (!over.foreground.transparent) made.foreground = over.foreground;

    if (!over.background.transparent) made.background = over.background;

    made.bold = base.bold || over.bold;
    made.faint = base.faint || over.faint;
    made.italic = base.italic || over.italic;
    made.under = base.under || over.under;
    made.reverse = base.reverse || over.reverse;
    made.crossed = base.crossed || over.crossed;

    return made;
}

// The hyperlink to `address`: its mark and the address. The mark is what the cells carry and what
// comparing frames sees, so one address keeps one mark, frame after frame, whoever draws it (a link
// in a label, a mark in a page), and two addresses never share one: a link that changes its address
// gets another mark, otherwise its cells would equal the old ones and the terminal would keep the
// old address. The mark also goes to the terminal as the link's id, by which a terminal lights up
// the whole link under the pointer, also when it spans several rows; two places with the same
// address light up together, which is what they are. Zero is no link, so the count starts at one.
//
// The marks live in a table of the process. A program that draws ever new addresses (a viewer
// over a long list of them) would grow it without end, so past `remembered` addresses it is
// forgotten whole: the count goes on, so a forgotten address gets a new mark, never one another
// address had, and the only cost is that its cells are written once more.
Hyperlink hyperlink(std::string address) {
    static constexpr std::size_t remembered = 4096;

    static std::mutex guard;
    static std::unordered_map<std::string, std::uint32_t> known;
    static std::uint32_t issued = 0;

    const std::lock_guard<std::mutex> held{guard};

    if (const auto found = known.find(address); found != known.end()) return Hyperlink{found->second, std::move(address)};

    if (known.size() >= remembered) known.clear();

    known.emplace(address, ++issued);

    return Hyperlink{issued, std::move(address)};
}

// Text is measured in cells, not in characters or bytes: a letter with a diacritic counts as
// one, an East Asian wide character as two, a cluster as the cells it takes (`width`).
ui::Size measured(std::string_view what) { return ui::Size{width(what), what.empty() ? 0 : 1}; }

// A blank view of the given size, with every cell empty.
View::View(ui::Size given) {
    // A negative dimension comes from a cramped window, not from a mistake of the caller:
    // "minus two left" must give an empty sheet, not crash the program.
    size_.wide = std::max(given.wide, 0);
    size_.high = std::max(given.high, 0);

    cells_.assign(static_cast<std::size_t>(size_.wide) * static_cast<std::size_t>(size_.high),
                  Cell{});
}

// Whether `where` lies inside the view.
bool View::holds(ui::Spot where) const {
    return where.x >= 0 && where.y >= 0 && where.x < size_.wide && where.y < size_.high;
}

// The cell at `where`, without a bounds check; callers check with `holds` first.
Cell& View::cell(ui::Spot where) {
    return cells_[static_cast<std::size_t>(where.y) * static_cast<std::size_t>(size_.wide) +
                  static_cast<std::size_t>(where.x)];
}

// A double-width character occupies two cells, and writing into one of them orphans the other.
// We turn that other one into a space before overwriting anything; otherwise half a character
// stays on the screen and starts flickering when frames are compared.
void View::broken(ui::Spot where) {
    if (!holds(where)) return;

    const Cell one = cell(where);

    if (one.letter == 0 && where.x > 0) cell({where.x - 1, where.y}).letter = U' ';

    if (width(one.letter) == 2 && holds({where.x + 1, where.y})) {
        Cell& after = cell({where.x + 1, where.y});

        if (after.letter == 0) after.letter = U' ';
    }
}

// Puts one cell's content at `at`: a character or a cluster's glyph (see `glyph`). Every character
// goes through `shown`, so a cell never carries a control character to the terminal. A character
// that takes no cell of its own (a combining mark, ZWJ, a selector) joins the glyph to the left of
// `at`, past the second half of a wide one: code that puts text character by character and moves
// by `width` thus still draws "e" with U+0301 as one letter. With nothing to its left in the view
// it is not drawn.
void View::put(ui::Spot at, char32_t what, const Style& how) {
    if (what == 0 || !holds(at)) return;

    what = shown(what);

    const int much = width(what);

    if (much == 0) {
        ui::Spot left{at.x - 1, at.y};

        if (holds(left) && cell(left).letter == 0) --left.x;

        if (!holds(left) || cell(left).letter == 0) return;

        Cell& one = cell(left);

        const char32_t made = glyph(spelled(one.letter) + utf8::encode(what));

        // A mark that would change the glyph's width (a selector that widens) is left out rather
        // than shift what lies to the right.
        if (made != 0 && width(made) == width(one.letter)) one.letter = made;

        return;
    }

    // Half of a double-width character is garbage, so the last column gets a space instead.
    if (much == 2 && !holds({at.x + 1, at.y})) {
        broken(at);

        cell(at) = Cell{U' ', how};

        return;
    }

    broken(at);

    if (much == 2) broken({at.x + 1, at.y});

    cell(at) = Cell{what, how};

    if (much == 2) cell({at.x + 1, at.y}) = Cell{0, how};
}

// Puts UTF-8 text starting at `at`, cluster by cluster, advancing by each glyph's width and
// stopping at the right edge. A cluster that shows nothing (a lone format character) takes no
// cell.
void View::put(ui::Spot at, std::string_view what, const Style& how) {
    std::size_t which = 0;

    while (which < what.size() && at.x < size_.wide) {
        const std::size_t next = cluster_after(what, which);
        const char32_t one = glyph(what.substr(which, next - which));

        which = next;

        if (one == 0) continue;

        put(at, one, how);

        at.x += width(one);
    }
}

// Fills `where` with spaces in the style `how`.
void View::fill(const ui::Area& where, const Style& how) {
    for (int y = where.at.y; y < where.at.y + where.size.high; ++y)
        for (int x = where.at.x; x < where.at.x + where.size.wide; ++x) put({x, y}, U' ', how);
}

// A shadow has no character of its own. The cell remembers the style and the fact that it brings
// no character; that is resolved only when the view is drawn onto another, when it is clear what
// lies below. Characters cannot be blended, so the transparency of a character is something other
// than the transparency of a color: a character is either there or not. That is why a shadow
// needs no alpha.
void View::shade(const ui::Area& where, const Style& how) {
    for (int y = where.at.y; y < where.at.y + where.size.high; ++y)
        for (int x = where.at.x; x < where.at.x + where.size.wide; ++x) {
            if (!holds({x, y})) continue;

            Cell& one = cell({x, y});

            one.style = how;
            one.shown = Shown::shade;
        }
}

// Foreground only. The cell carries a character and its color, and its background, when none is
// given, is taken from what lies below when the view is drawn: a cell on the edge of a picture
// stands in a window, in a viewer, on a panel, and only what is below knows its background. Plain
// text does not work like this and must not; its role gives its own background (see the frame
// trial in `framing.cpp`). A foreground cell is a separate, explicit request. A given background
// wins: a cell with a background is an ordinary cell.
void View::foreground(ui::Spot at, char32_t what, const Style& how) {
    put(at, what, how);

    if (!holds(at) || what == 0 || !how.background.transparent) return;

    cell(at).shown = Shown::foreground;
}

// Marks cells that the node does not draw although they lie in its rectangle. This happens
// whenever the shape is not a rectangle: a menu takes the title strip and the box below it, and
// the corners between them belong to the neighbors. Without this the node would paint them over
// with spaces. A double-width character cut by the edge of `where` loses its other half, so that
// half becomes a space, as it would under `put`.
void View::clear(const ui::Area& where) {
    if (where.size.wide <= 0) return;

    for (int y = where.at.y; y < where.at.y + where.size.high; ++y) {
        broken({where.at.x, y});
        broken({where.at.x + where.size.wide - 1, y});

        for (int x = where.at.x; x < where.at.x + where.size.wide; ++x) {
            if (!holds({x, y})) continue;

            cell({x, y}) = Cell{U' ', Style{}, Shown::none};
        }
    }
}

// Draws `one` onto this view with its top left corner at `at`, clipped to this view, resolving
// shadows, foreground cells and cells that were not drawn against what lies below.
void View::draw(const View& one, ui::Spot at) {
    // Pictures go up together with the cells. Where they stand is told by the cells, not by the
    // list (the list only says what to draw them with), so there is nothing in it to shift or clip.
    // Hyperlinks the same way: the cells say where a link stands, the list what its address is.
    for (const Picture& one_picture : one.pictures_)
        if (std::none_of(pictures_.begin(), pictures_.end(),
                         [&one_picture](const Picture& mine) { return mine.mark == one_picture.mark; }))
            pictures_.push_back(one_picture);

    for (const Hyperlink& one_link : one.links_)
        if (std::none_of(links_.begin(), links_.end(), [&one_link](const Hyperlink& mine) { return mine.mark == one_link.mark; }))
            links_.push_back(one_link);

    const int from_x = std::max(at.x, 0);
    const int from_y = std::max(at.y, 0);
    const int upto_x = std::min(at.x + one.size_.wide, size_.wide);
    const int upto_y = std::min(at.y + one.size_.high, size_.high);

    // Whether a cell of `one` puts a character in place of what lies below; a cell that only dims
    // or leaves what it finds does not.
    const auto putting = [](const Cell& said) {
        return said.shown == Shown::own || said.shown == Shown::foreground;
    };

    if (upto_x > from_x)
        for (int y = from_y; y < upto_y; ++y) {
            const Cell* const row =
                &one.cells_[static_cast<std::size_t>(y - at.y) * static_cast<std::size_t>(one.size_.wide)];

            for (int x = from_x; x < upto_x;) {
                const Cell& said = row[x - at.x];

                // A cell without its own character only changes the style of what it finds; this
                // is how a shadow looks: the text below stays visible, only dimmed. A cell that the
                // node did not draw at all changes nothing.
                if (said.shown == Shown::none) {
                    ++x;

                    continue;
                }

                if (said.shown == Shown::shade) {
                    Cell& under = cell({x, y});

                    under.style = said.style;

                    // A shadow over emptiness stays a shadow instead of vanishing. A cell on
                    // which nothing has been drawn yet still says "I am not here", and drawing
                    // skips such cells, so the dimming would be lost one level up and the shadow
                    // under a dialog would break off halfway.
                    if (under.shown == Shown::none) under.shown = Shown::shade;

                    // A pixel picture cannot be dimmed, so the shadow hides it: the cell stops
                    // belonging to the picture, and the picture is drawn in characters until the
                    // shadow goes away.
                    under.picture = 0;

                    ++x;

                    continue;
                }

                // A run of cells that put characters. Where it starts and where it stops, it may
                // cover half of a double-width character below, and the half it leaves becomes a
                // space. This is the real edge of a window with a transparent rim or a shadow,
                // inside its rectangle. Within the run both halves go together, so the edges are
                // all there is to check.
                const int start = x;

                if (x > 0 && cell({x, y}).letter == 0) cell({x - 1, y}).letter = U' ';

                char32_t covered = U' ';

                for (; x < upto_x && putting(row[x - at.x]); ++x) {
                    const Cell& put = row[x - at.x];
                    Cell& under = cell({x, y});

                    covered = under.letter;

                    // A foreground cell over emptiness stays what it is and waits one level up,
                    // like a shadow; over anything else it takes that background and becomes an
                    // ordinary cell.
                    if (put.shown == Shown::foreground && under.shown != Shown::none) {
                        const Color paper = under.style.background;

                        under = put;
                        under.style.background = paper;
                        under.shown = Shown::own;
                    } else {
                        under = put;
                    }
                }

                if (x < size_.wide && width(covered) == 2 && cell({x, y}).letter == 0)
                    cell({x, y}).letter = U' ';

                // A double-width character of `one` whose other half does not come with it (cut off
                // by the edge of this view, or dimmed or left out in `one`) would cover a column it
                // does not own or leave a hole, so a space stands in for it. `put` and `clear` keep
                // the two halves together within a view, so this too can happen only at the ends of
                // a run.
                if (cell({start, y}).letter == 0) cell({start, y}).letter = U' ';

                if (width(cell({x - 1, y}).letter) == 2) cell({x - 1, y}).letter = U' ';
            }
        }
}

// A pixel picture. The picture's cells carry its mark and stay what they were (characters of the
// picture, drawn as before), while the list keeps the bytes. Composing carries the mark together
// with the cell, so whatever covers the picture (a window, a menu, a shadow, a scroll edge) simply
// has no mark. Transparent cells become an empty foreground: they take their background from below
// as before, and they must carry the mark, otherwise a picture with a transparent edge would never
// be visible in full.
void View::picture(const ui::Area& where, Picture what) {
    for (int y = where.at.y; y < where.at.y + where.size.high; ++y)
        for (int x = where.at.x; x < where.at.x + where.size.wide; ++x) {
            if (!holds({x, y})) continue;

            Cell& one = cell({x, y});

            if (one.shown == Shown::none) one = Cell{U' ', Style{}, Shown::foreground};

            one.picture = what.mark;
        }

    std::erase_if(pictures_, [&what](const Picture& one) { return one.mark == what.mark; });

    pictures_.push_back(std::move(what));
}

// A hyperlink over `where`: its cells carry the link's mark, and the list keeps the address. Only
// cells that show something take it; a cell the node did not draw stays as it is, so a link never
// reaches over what lies below. Whatever is put over a cell afterwards (`put`, `fill`, `clear`, a
// window drawn over it) takes the mark away with the old content, and a shadow keeps it, since the
// text below it stays what it was, only dimmed.
void View::link(const ui::Area& where, Hyperlink what) {
    if (what.mark == 0) return;

    for (int y = where.at.y; y < where.at.y + where.size.high; ++y)
        for (int x = where.at.x; x < where.at.x + where.size.wide; ++x) {
            if (!holds({x, y})) continue;

            Cell& one = cell({x, y});

            if (one.shown == Shown::own || one.shown == Shown::foreground) one.link = what.mark;
        }

    std::erase_if(links_, [&what](const Hyperlink& one) { return one.mark == what.mark; });

    links_.push_back(std::move(what));
}

// A picture visible in full (every cell of its rectangle carries its mark, and the rectangle lies
// within `room`) gets pixels: its cells become empty, with the background they had and with the
// picture's mark, so that they never equal an ordinary cell. Thanks to this, comparing frames sees
// every transition between the picture and characters and rewrites the cells the picture covered.
// A picture covered in even one cell stays entirely in characters, and its cells lose the mark,
// for the same reason.
std::vector<Placement> View::bared(const ui::Area& room) {
    std::vector<Placement> made;

    for (const Picture& one : pictures_) {
        if (one.mark == 0 || one.size.wide <= 0 || one.size.high <= 0) continue;

        int least_x = size_.wide;
        int least_y = size_.high;
        int most_x = -1;
        int most_y = -1;
        long long many = 0;

        for (int y = 0; y < size_.high; ++y)
            for (int x = 0; x < size_.wide; ++x)
                if (cell({x, y}).picture == one.mark) {
                    least_x = std::min(least_x, x);
                    least_y = std::min(least_y, y);
                    most_x = std::max(most_x, x);
                    most_y = std::max(most_y, y);
                    ++many;
                }

        if (many == 0) continue;

        const ui::Area found{{least_x, least_y}, {most_x - least_x + 1, most_y - least_y + 1}};

        const bool whole = found.size == one.size &&
                           many == static_cast<long long>(one.size.wide) * one.size.high &&
                           found.at.x >= room.at.x && found.at.y >= room.at.y &&
                           found.at.x + found.size.wide <= room.at.x + room.size.wide &&
                           found.at.y + found.size.high <= room.at.y + room.size.high;

        for (int y = found.at.y; y < found.at.y + found.size.high; ++y)
            for (int x = found.at.x; x < found.at.x + found.size.wide; ++x) {
                Cell& here = cell({x, y});

                if (here.picture != one.mark) continue;

                if (!whole) {
                    here.picture = 0;

                    continue;
                }

                Style paper;

                paper.background = here.style.background;

                here = Cell{U' ', paper, Shown::own, one.mark};
            }

        if (whole) made.push_back(Placement{found.at, one});
    }

    return made;
}

// A copy of the cell at `where`, or an empty cell outside the view.
Cell View::at(ui::Spot where) const {
    if (!holds(where)) return Cell{};

    return cells_[static_cast<std::size_t>(where.y) * static_cast<std::size_t>(size_.wide) +
                  static_cast<std::size_t>(where.x)];
}

}
