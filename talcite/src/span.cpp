#include "talcite/span.hpp"

#include "talcite/skin.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/root.hpp"

#include "eliding.hpp"

// A label: text in rows, wrapped or not, elided and justified when drawn. Its marks (`marks`) lay a
// role over a stretch of the text, counted as a page counts them: the line is the paragraph (the
// text between hard line breaks) and the bytes count from its start. A mark with an address is a
// link: its cells carry the hyperlink (`Canvas::link`, OSC 8 in a terminal that takes it), and a
// click on it follows the address the way every widget follows one (`FollowingMixin`). A label takes
// no focus, so a key does not reach its links; a click does, and a terminal with hyperlinks opens
// them itself on Ctrl+click, which is why a click with Ctrl is left to it, as on a link (`Link`).
//
// To lay marks over the rows, every row knows where each of its bytes came from in the text.
// Wrapping joins words with one space wherever the text had several, and eliding drops what does
// not fit, so the row's own byte numbers say nothing about the text without that.
namespace talcite::ui {

// A label with the given text and shape, in the "talcite.normal" role until told otherwise.
Span::Span(std::string text, SpanShape shape) : text_(std::move(text)), shape_(shape), natural_(natural()) {
    role("talcite.normal");
}

// A label holds no children.
int Span::capacity() const { return 0; }

namespace {

// A paragraph of the text: where it starts and what it holds.
struct Paragraph {
    std::size_t at = 0;
    std::string_view text;
};

// Splits the text at hard line breaks. A hard line break always splits, also without wrapping:
// sent to the view like any other byte, it would come out as a replacement character in the middle
// of a sentence, and two-sentence messages are written exactly that way. Splitting gives one piece
// more than there are breaks: "a\nb" is two rows, and "a\n" is a row and an empty row after it. A
// rule without exceptions reads better than guessing whether the author wanted that empty one.
//
// A carriage return is removed from the end of each piece, because text from Windows carries
// "\r\n", and a lone "\r" left in the text would show up on screen as garbage.
std::vector<Paragraph> parted(std::string_view what) {
    std::vector<Paragraph> made;

    for (std::size_t at = 0;;) {
        const std::size_t end = what.find('\n', at);
        const std::size_t upto = end == std::string_view::npos ? what.size() : end;

        std::string_view one = what.substr(at, upto - at);

        if (!one.empty() && one.back() == '\r') one.remove_suffix(1);

        made.push_back(Paragraph{at, one});

        if (end == std::string_view::npos) break;

        at = end + 1;
    }

    return made;
}

// Wraps one paragraph (the `line`-th) to `wide` columns, appending the rows to `into`. It breaks at
// spaces, and a word longer than a row is broken in place, because the only alternative would be
// running off the canvas, which `Canvas` would not let out anyway. Each paragraph wraps
// separately, so a hard line break never merges with the neighboring sentence. Words in a row
// stand one space apart, whatever stood between them; that space comes from the first space after
// the word before it, so that a mark over a whole phrase covers the gaps too.
void wrap_into(std::string_view what, std::size_t line, int wide, std::vector<Traced>& into) {
    struct Word {
        std::size_t at = 0;
        std::string_view text;
    };

    std::vector<Word> words;

    for (std::size_t at = 0; at <= what.size();) {
        const std::size_t space = what.find(' ', at);
        const std::size_t upto = space == std::string_view::npos ? what.size() : space;

        if (upto > at) words.push_back(Word{at, what.substr(at, upto - at)});

        if (space == std::string_view::npos) break;

        at = space + 1;
    }

    Traced row;

    row.line = line;

    int much = 0;

    // Starts a fresh row of the paragraph.
    const auto fresh = [line] {
        Traced made;

        made.line = line;

        return made;
    };

    for (const Word& whole : words) {
        std::string_view word = whole.text;
        std::size_t from = whole.at;
        int room = measured(word).wide;

        // A word longer than the whole row is cut as many times as needed. If not even one
        // character fits (the row is one cell wide and the character is double-width), the word is
        // left whole and the view clips it; pretending that something fitted would be worse. The
        // cutting walks along the word, and what is left is measured by what each piece took, so
        // a word of a hundred thousand characters costs one pass, not one per row.
        while (room > wide) {
            if (!row.text.empty()) {
                into.push_back(std::move(row));

                row = fresh();
                much = 0;
            }

            const std::string piece = cut(word, wide);

            if (piece.empty()) break;

            Traced alone = fresh();

            alone.taken(piece, from);

            word.remove_prefix(piece.size());
            from += piece.size();

            // A tab's width depends on where it stands, so a word holding one is measured anew.
            room = word.find('\t') == std::string_view::npos ? room - measured(piece).wide : measured(word).wide;

            into.push_back(std::move(alone));
        }

        if (word.empty()) continue;

        if (row.text.empty()) {
            row.taken(word, from);
            much = room;
        } else if (much + 1 + room <= wide) {
            const std::size_t gap = row.bytes.back() + 1;

            row.taken(" ", gap);
            row.taken(word, from);
            much += 1 + room;
        } else {
            into.push_back(std::move(row));

            row = fresh();
            row.taken(word, from);
            much = room;
        }
    }

    if (!row.text.empty()) into.push_back(std::move(row));
}

// The rows of the text for a width of `wide`, wrapped when told so. An empty paragraph stays an
// empty row instead of vanishing: it is a gap someone wrote on purpose.
std::vector<Traced> rows_of(std::string_view text, bool wrapped, int wide) {
    if (text.empty()) return {};

    std::vector<Traced> made;

    const std::vector<Paragraph> parts = parted(text);

    if (!wrapped) {
        for (std::size_t line = 0; line < parts.size(); ++line) made.push_back(traced(parts[line].text, line));

        return made;
    }

    if (wide <= 0) return {};

    for (std::size_t line = 0; line < parts.size(); ++line) {
        if (parts[line].text.empty()) {
            made.push_back(traced({}, line));

            continue;
        }

        wrap_into(parts[line].text, line, wide, made);
    }

    return made;
}

// A row as drawn: shortened, at its column, on its screen row.
struct Placed {
    Traced row;
    int x = 0;
    int y = 0;
};

// The rows as they are drawn in `given`: shortened (`elided`) and justified. Rows below the
// height are left out; `upto` stops earlier, so that painting reads no further than it shows.
std::vector<Placed> placed(std::string_view text, const SpanShape& shape, Size given, char32_t mark, int upto) {
    std::vector<Placed> made;

    int y = 0;

    for (const Traced& one : rows_of(text, shape.wrapped, given.wide)) {
        if (y >= given.high || y >= upto) break;

        Placed it{elided(one, given.wide, shape.elide, mark), 0, y};

        if (shape.justify != Justify::left) {
            const int room = given.wide - measured(it.row.text).wide;

            it.x = std::max(shape.justify == Justify::right ? room : room / 2, 0);
        }

        made.push_back(std::move(it));

        ++y;
    }

    return made;
}

}

// The rows of the label for a width of `wide`, wrapped when the shape says so.
std::vector<std::string> Span::lines(int wide) const {
    std::vector<std::string> made;

    for (Traced& one : rows_of(text_, shape_.wrapped, wide)) made.push_back(std::move(one.text));

    return made;
}

// The widest row and the number of rows. The height comes from the rows, not from wrapping,
// because there can be more rows than one even without it. Unwrapped, they do not depend on the
// room, so they are measured once, with the text (`natural`).
Size Span::wants(Size room) const {
    if (text_.empty()) return {};

    if (!shape_.wrapped) return natural_;

    const std::vector<std::string> made = lines(room.wide);

    Size want{0, static_cast<int>(made.size())};

    for (const std::string& one : made) want.wide = std::max(want.wide, measured(one).wide);

    return want;
}

// The widest row and the number of rows of the text unwrapped, as `wants` gives them without
// wrapping.
Size Span::natural() const {
    if (text_.empty()) return {};

    Size made{0, 0};

    for (const Traced& one : rows_of(text_, false, 0)) {
        made.wide = std::max(made.wide, measured(one.text).wide);
        ++made.high;
    }

    return made;
}

// Draws the rows, elided and justified, and the marks over them. Eliding and justifying happen only
// here, when drawing: `wants` says how much the label really wants; if it returned the length after
// eliding, the parent would never learn that space is too tight, and an elided label would stay
// elided forever.
//
// A mark is laid over the cells of the text it covers: the letter stays, and the mark's role is
// merged over the label's (`Style::merge`), so a role that sets only an underline keeps the colors.
// A mark with an address also lays its hyperlink there. A mark over an ellipsis covers nothing:
// what the ellipsis stands for is not shown.
void Span::paint(Canvas& onto) const {
    const Size given = onto.size();
    const Area seen = onto.seen();

    const Skin& how = skin();

    // The role name is set from outside. By default it is talcite.normal, but a label placed in a
    // red error box has to be red, and one role for every label in a program cannot be red and
    // navy at once. Whoever builds such a box knows what stands in it and says so directly.
    const Style look = how.style(role());
    const char32_t mark = how.glyph("talcite.elide");

    // A row above the visible area is drawn into nothing and costs only its layout; for a scrolled
    // label, the rows below it are not even laid out.
    for (const Placed& one : placed(text_, shape_, given, mark, seen.at.y + seen.size.high)) {
        if (one.y < seen.at.y) continue;

        onto.put({one.x, one.y}, one.row.text, look);

        for (const Mark& marked : marks()) {
            if (marked.line != one.row.line) continue;

            const Style over = Style::merge(look, how.style(marked.role));
            const Hyperlink target = marked.address.empty() ? Hyperlink{} : hyperlink(marked.address);

            walked(one.row, one.x, [&](int x, int much, char32_t letter, std::size_t byte) {
                if (!covers(marked, one.row.line, byte)) return;

                const Spot spot{x, one.y};

                onto.put(spot, letter, over);

                if (target.mark != 0) onto.link({spot, {much, 1}}, target);
            });
        }
    }
}

// A click with the left button on a mark with an address follows it (`FollowingMixin::followed`); a
// click with Ctrl is the terminal's, and a click anywhere else is not the label's, so it goes on to
// whoever holds the label.
bool Span::handle(ClickEvent& what) {
    if (what.mods().ctrl || what.button() != Mouse::left) return false;

    const Spot at = local(what.where());

    std::optional<std::string> found;

    for (const Placed& one : placed(text_, shape_, area().size, skin().glyph("talcite.elide"), at.y + 1)) {
        if (one.y != at.y) continue;

        walked(one.row, one.x, [&](int x, int much, char32_t, std::size_t byte) {
            if (found || at.x < x || at.x >= x + much) return;

            for (const Mark& marked : marks())
                if (!marked.address.empty() && covers(marked, one.row.line, byte)) {
                    found = marked.address;

                    return;
                }
        });
    }

    if (!found) return false;

    followed(std::move(*found));

    return true;
}

}
