#include "talcite/span.hpp"

#include "talcite/skin.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

#include "talcite/encoding.hpp"

#include "eliding.hpp"

namespace talcite::ui {

// A label with the given text and shape, in the "talcite.normal" role until told otherwise.
Span::Span(std::string text, SpanShape shape) : text_(std::move(text)), shape_(shape) {
    role("talcite.normal");
}

// A label holds no children.
int Span::capacity() const { return 0; }

namespace {

// Splits the text at hard line breaks. A hard line break always splits, also without wrapping:
// sent to the view like any other byte, it would come out as a replacement character in the middle
// of a sentence, and two-sentence messages are written exactly that way. Splitting gives one piece
// more than there are breaks: "a\nb" is two rows, and "a\n" is a row and an empty row after it. A
// rule without exceptions reads better than guessing whether the author wanted that empty one.
//
// A carriage return is removed from the end of each piece, because text from Windows carries
// "\r\n", and a lone "\r" left in the text would show up on screen as garbage.
std::vector<std::string> parted(const std::string& what) {
    std::vector<std::string> made;

    for (std::size_t at = 0;;) {
        const std::size_t end = what.find('\n', at);
        const std::size_t upto = end == std::string::npos ? what.size() : end;

        std::string one = what.substr(at, upto - at);

        if (!one.empty() && one.back() == '\r') one.pop_back();

        made.push_back(std::move(one));

        if (end == std::string::npos) break;

        at = end + 1;
    }

    return made;
}

// Wraps one paragraph to `wide` columns, appending the rows to `into`. It breaks at spaces, and a
// word longer than a row is broken in place, because the only alternative would be running off the
// view, which `View` would not let out anyway. Each paragraph wraps separately, so a hard line
// break never merges with the neighboring sentence.
//
// A file-local function rather than a method: it touches nothing that belongs to the label, and
// the public header has no reason to know there is one more step inside.
void wrap_into(const std::string& what, int wide, std::vector<std::string>& into) {
    std::vector<std::string> words;

    for (std::size_t at = 0; at <= what.size();) {
        const std::size_t space = what.find(' ', at);
        const std::size_t upto = space == std::string::npos ? what.size() : space;

        if (upto > at) words.push_back(what.substr(at, upto - at));

        if (space == std::string::npos) break;

        at = space + 1;
    }

    std::string line;
    int much = 0;

    for (std::string whole : words) {
        std::string_view word = whole;
        int room = measured(word).wide;

        // A word longer than the whole row is cut as many times as needed. If not even one
        // character fits (the row is one cell wide and the character is double-width), the word is
        // left whole and the view clips it; pretending that something fitted would be worse. The
        // cutting walks along the word, and what is left is measured by what each piece took, so
        // a word of a hundred thousand characters costs one pass, not one per row.
        while (room > wide) {
            if (!line.empty()) {
                into.push_back(std::move(line));

                line.clear();
                much = 0;
            }

            std::string piece = cut(word, wide);

            if (piece.empty()) break;

            word.remove_prefix(piece.size());

            // A tab's width depends on where it stands, so a word holding one is measured anew.
            room = word.find('\t') == std::string_view::npos ? room - measured(piece).wide : measured(word).wide;

            into.push_back(std::move(piece));
        }

        if (word.empty()) continue;

        if (line.empty()) {
            line = std::string{word};
            much = room;
        } else if (much + 1 + room <= wide) {
            line += ' ';
            line += word;
            much += 1 + room;
        } else {
            into.push_back(std::move(line));

            line = std::string{word};
            much = room;
        }
    }

    if (!line.empty()) into.push_back(std::move(line));
}

}

// The rows of the label for a width of `wide`, wrapped when the shape says so. An empty paragraph
// stays an empty row instead of vanishing: it is a gap someone wrote on purpose.
std::vector<std::string> Span::lines(int wide) const {
    if (text_.empty()) return {};

    const std::vector<std::string> parts = parted(text_);

    if (!shape_.wrapped) return parts;

    if (wide <= 0) return {};

    std::vector<std::string> made;

    for (const std::string& one : parts) {
        if (one.empty()) {
            made.emplace_back();

            continue;
        }

        wrap_into(one, wide, made);
    }

    return made;
}

// The widest row and the number of rows. The height comes from the rows, not from wrapping,
// because there can be more rows than one even without it.
Size Span::wants(Size room) const {
    if (text_.empty()) return {};

    const std::vector<std::string> made = lines(room.wide);

    Size want{0, static_cast<int>(made.size())};

    for (const std::string& one : made) want.wide = std::max(want.wide, measured(one).wide);

    return want;
}

// Draws the rows, elided and justified. Eliding and justifying happen only here, when drawing:
// `wants` says how much the label really wants; if it returned the length after eliding, the
// parent would never learn that space is too tight, and an elided label would stay elided forever.
View Span::paint(Size given, Area seen) {
    View made{seen.size};

    // Transparent wherever nothing is drawn, the rule described at `Node::paint`.

    made.clear({{0, 0}, seen.size});

    const Skin& how = skin();

    // The role name is set from outside. By default it is talcite.normal, but a label placed in a
    // red error box has to be red, and one role for every label in a program cannot be red and
    // navy at once. Whoever builds such a box knows what stands in it and says so directly.
    const Style look = how.style(role());
    const char32_t mark = how.glyph("talcite.elide");

    const std::vector<std::string> said = lines(given.wide);

    int y = 0;

    for (const std::string& one : said) {
        if (y >= given.high || y - seen.at.y >= seen.size.high) break;

        // A row above the visible area is skipped without drawing; for a scrolled label this is
        // the difference between twenty rows and all of them.
        if (y < seen.at.y) {
            ++y;

            continue;
        }

        const std::string shown = elided(one, given.wide, shape_.elide, mark);

        int x = 0;

        if (shape_.justify != Justify::left) {
            const int room = given.wide - measured(shown).wide;

            x = shape_.justify == Justify::right ? room : room / 2;

            if (x < 0) x = 0;
        }

        made.put({x - seen.at.x, y - seen.at.y}, shown, look);

        ++y;
    }

    return made;
}

}
