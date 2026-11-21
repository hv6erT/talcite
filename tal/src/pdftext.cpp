#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/docs.hpp"
#include "tal/text.hpp"
#include "tal/trouble.hpp"

#include "pdffile.hpp"
#include "pdfing.hpp"
#include "tagging.hpp"

// The text version of a PDF document (E6f), page by page, read by our own reader of the file
// (pdf.cpp) and an interpreter of the content of pages that follows the text and nothing else:
// the operators of text and of the text state, the current transformation and its saving, forms
// drawn by other forms (`form_depth` deep), marked content with ActualText, and inline pictures
// skipped. Each glyph is turned into characters by the font: its ToUnicode first, then for simple
// fonts the names of its Differences, its base encoding, the encoding built into an embedded
// Type 1 font, the Unicode cmap of a symbolic TrueType font, and StandardEncoding; the names go
// through the Adobe Glyph List (glyphs.inc), uniXXXX and uXXXX, and ligatures come out as letters.
// What no font maps is U+FFFD. Its place is where the glyph lands on the page, after the text
// matrix, the text state and the transformation.
//
// The text is read in the order of the content stream, which is the reading order of nearly every
// writer (two columns of pdfLaTeX included). A glyph whose baseline is more than half a glyph's
// height away from the line's, or that goes back along it, starts a new line; a gap along the
// line over 0.2 em is a space, where a glyph ends after its advance with the character and word
// spacing (Tc, Tw), so letter-spaced words stay words; a new line that is not below the last by
// a quarter of a line to 1.5 lines (one above it, beside it or far below) starts a new paragraph.
// An accent drawn as a glyph of its own over a letter (TeX's old encodings) is joined to the
// letter, and an unknown glyph over a letter is dropped.
// The ActualText of marked content stands in the place of all the glyphs within it. Hyphens stay
// where the file has them. The page's links to addresses on the web (Link annotations with a URI
// action) are anchors over the text of the glyphs that lie in their rectangles, a glyph lying in
// one where a point a little above the middle of its baseline does. Two links to one address, one
// in the last word of a line and the other starting the next line, are one (hyperref breaks a long
// address so, and its first rectangle may stop short of the margin the address runs into). An
// anchor leaves out the spaces at its ends.
//
// Limits, because the file is foreign: `most_operators` operators on a page (the page ends there,
// cut), forms `form_depth` deep and `most_forms` drawn (the page is cut there; each form is decoded
// once for the page, however often it is drawn), marked content `most_marked` deep (deeper marks
// are counted, not read), a ToUnicode CMap of at most `most_cmap` bytes mapping at most
// `most_mapped` codes, `most_fonts` fonts read for one document, `most_links` links on a page. The
// reader's own budgets hold: what streams unpack to is counted against the file's budget, and a
// page that would go past it is Trouble full rather than a page that silently has no text. Pages
// are read one at a time; a page asked for while another is being read is Trouble busy, and a
// reading cancelled half way leaves nothing behind: the next one opens the file again.
namespace tal::docs {

namespace {

namespace pdf = meta::tagging::pdf;

using pdf::Array;
using pdf::Dict;
using pdf::Name;
using pdf::Object;
using pdf::Stream;

// A glyph's name and its characters in UTF-8.
struct Glyph {
    std::string_view name;
    std::string_view text;
};

// A spacing accent and the combining mark it stands for.
struct Accent {
    char32_t spacing;
    char32_t mark;
};

// A letter and a combining mark and the letter they compose to.
struct Composed {
    char32_t base;
    char32_t mark;
    char32_t made;
};

#include "glyphs.inc"

constexpr std::size_t most_operators = 10000000;
constexpr std::size_t most_cmap = std::size_t{1} << 20;
constexpr std::size_t most_mapped = 1000000;
constexpr std::size_t most_fonts = 4096;
constexpr std::size_t form_depth = 8;
constexpr std::size_t most_marked = 256;
constexpr std::size_t most_links = 4096;
constexpr std::size_t most_forms = 65536;

constexpr std::string_view replacement = "\xEF\xBF\xBD";

// A matrix of PDF: [a b c d e f], points as rows ([x y 1] times it).
struct Matrix {
    double a = 1;
    double b = 0;
    double c = 0;
    double d = 1;
    double e = 0;
    double f = 0;

    // This matrix, then `then`.
    [[nodiscard]] Matrix times(const Matrix& then) const {
        return Matrix{a * then.a + b * then.c,       a * then.b + b * then.d,       c * then.a + d * then.c,
                      c * then.b + d * then.d,       e * then.a + f * then.c + then.e, e * then.b + f * then.d + then.f};
    }

    // Whether every number is finite.
    [[nodiscard]] bool finite() const {
        return std::isfinite(a) && std::isfinite(b) && std::isfinite(c) && std::isfinite(d) && std::isfinite(e) && std::isfinite(f);
    }
};

// A translation by (x, y).
[[nodiscard]] Matrix moved(double x, double y) { return Matrix{1, 0, 0, 1, x, y}; }

// The characters of `text` in UTF-8 appended to `into`, each of the ligatures U+FB00 to U+FB06 as
// its letters and a control character as U+FFFD, since a terminal could take it for a command.
void put(std::string& into, char32_t one) {
    constexpr std::array<std::string_view, 7> ligatures{"ff", "fi", "fl", "ffi", "ffl", "st", "st"};

    if (one >= 0xFB00 && one <= 0xFB06) {
        into += ligatures[one - 0xFB00];

        return;
    }

    if ((one < 0x20 && one != '\t') || (one >= 0x7F && one <= 0x9F)) one = 0xFFFD;

    text::encoded(into, one);
}

// The characters a glyph's name stands for (ISO 32000-2, 9.10.2, and the Adobe Glyph List rules):
// a name of the list, uniXXXX (one or more groups of four hexadecimal digits) or uXXXX to uXXXXXX;
// a name with a suffix after '.' is the name before it, and one of parts joined by '_' is the
// parts. None for a name that means nothing.
[[nodiscard]] std::optional<std::string> glyph_text(std::string_view name) {
    name = name.substr(0, name.find('.'));

    if (name.empty()) return std::nullopt;

    std::string made;
    std::size_t at = 0;

    while (at <= name.size()) {
        const std::size_t end = std::min(name.find('_', at), name.size());
        const std::string_view part = name.substr(at, end - at);
        const auto found = std::lower_bound(glyphs.begin(), glyphs.end(), part, [](const Glyph& one, std::string_view sought) { return one.name < sought; });

        const auto hex = [](std::string_view digits) -> std::optional<char32_t> {
            char32_t value = 0;

            for (const char one : digits) {
                int digit = -1;

                if (one >= '0' && one <= '9') digit = one - '0';
                else if (one >= 'A' && one <= 'F') digit = one - 'A' + 10;

                if (digit < 0) return std::nullopt;

                value = value * 16 + static_cast<char32_t>(digit);
            }

            if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return std::nullopt;

            return value;
        };

        if (found != glyphs.end() && found->name == part) {
            for (std::size_t one = 0; one < found->text.size();) put(made, text::decoded(found->text, one).value_or(0xFFFD));
        } else if (part.size() >= 7 && part.starts_with("uni") && (part.size() - 3) % 4 == 0) {
            for (std::size_t one = 3; one < part.size(); one += 4) {
                const std::optional<char32_t> value = hex(part.substr(one, 4));

                if (!value) return std::nullopt;

                put(made, *value);
            }
        } else if (part.size() >= 5 && part.size() <= 7 && part.front() == 'u') {
            const std::optional<char32_t> value = hex(part.substr(1));

            if (!value) return std::nullopt;

            put(made, *value);
        } else {
            return std::nullopt;
        }

        at = end + 1;
    }

    return made;
}

// The character of a glyph that a bitmap font of pdfTeX names by its code ("/a65", a letter or two
// and the code in decimal): the code in the Cork (T1) encoding of TeX's EC fonts, which are the
// fonts that come out as bitmaps when their Type 1 versions are missing. None for another name.
// It is taken only for a font that has letters among its codes, since a bitmap font of symbols
// names its glyphs the same way in an encoding of its own.
[[nodiscard]] std::optional<std::string> coded_glyph(std::string_view name) {
    std::size_t at = 0;

    while (at < name.size() && at < 2 && ((name[at] >= 'a' && name[at] <= 'z') || (name[at] >= 'A' && name[at] <= 'Z'))) ++at;

    const std::string_view digits = name.substr(at);

    if (digits.empty() || digits.size() > 3 || !std::all_of(digits.begin(), digits.end(), [](char one) { return one >= '0' && one <= '9'; })) return std::nullopt;

    const int code = std::stoi(std::string{digits});

    if (code >= 256 || cork_encoding[static_cast<std::size_t>(code)] == 0) return std::nullopt;

    std::string made;

    put(made, cork_encoding[static_cast<std::size_t>(code)]);

    return made;
}

// UTF-16BE (a destination of ToUnicode) as UTF-8: pairs of surrogates joined, a lone one U+FFFD.
[[nodiscard]] std::string from_utf16(std::string_view bytes) {
    std::string made;

    for (std::size_t at = 0; at + 1 < bytes.size(); at += 2) {
        const auto unit = [&](std::size_t where) {
            return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[where])) << 8U) | static_cast<unsigned char>(bytes[where + 1]);
        };
        const std::uint32_t one = unit(at);

        if (one >= 0xD800 && one <= 0xDBFF && at + 3 < bytes.size()) {
            const std::uint32_t low = unit(at + 2);

            if (low >= 0xDC00 && low <= 0xDFFF) {
                put(made, static_cast<char32_t>(0x10000 + ((one - 0xD800) << 10U) + (low - 0xDC00)));
                at += 2;

                continue;
            }
        }

        put(made, one >= 0xD800 && one <= 0xDFFF ? char32_t{0xFFFD} : static_cast<char32_t>(one));
    }

    return made;
}

// The big-endian number in the bytes of `text`, at most four of them.
[[nodiscard]] std::uint32_t code_of(std::string_view text) {
    std::uint32_t made = 0;

    for (const char one : text.substr(0, 4)) made = (made << 8U) | static_cast<unsigned char>(one);

    return made;
}

// A token of a content stream or a CMap: an operand (an object) or an operator (a word), or the
// end.
struct Token {
    std::optional<Object> operand;
    std::string_view word;
    bool end = false;
};

// The tokens of a content stream or a CMap, read by the reader's parser of objects; what is
// neither an object nor a word (a stray delimiter) is skipped.
class Tokens {
public:
    explicit Tokens(std::span<const std::byte> data) : data_(data) {}

    // The next token.
    [[nodiscard]] Token next() {
        for (;;) {
            pdf::Syntax blank{data_, at_};

            blank.blank();
            at_ = blank.at();

            if (at_ >= data_.size()) return Token{std::nullopt, {}, true};

            const int one = std::to_integer<int>(data_[at_]);

            if (one == '/' || one == '(' || one == '<' || one == '[' || one == '+' || one == '-' || one == '.' || (one >= '0' && one <= '9')) {
                pdf::Syntax syntax{data_, at_};
                std::optional<Object> made = syntax.object();

                if (made) {
                    at_ = syntax.at();

                    return Token{std::move(made), {}, false};
                }

                if (syntax.cut()) return Token{std::nullopt, {}, true};

                ++at_;

                continue;
            }

            if (pdf::delimiting(one)) {
                ++at_;

                continue;
            }

            const std::size_t from = at_;

            while (at_ < data_.size() && !pdf::spacing(std::to_integer<int>(data_[at_])) && !pdf::delimiting(std::to_integer<int>(data_[at_]))) ++at_;

            const std::string_view word{reinterpret_cast<const char*>(data_.data()) + from, at_ - from};

            if (word == "true") return Token{Object{true}, {}, false};
            if (word == "false") return Token{Object{false}, {}, false};
            if (word == "null") return Token{Object{pdf::Null{}}, {}, false};

            return Token{std::nullopt, word, false};
        }
    }

    // Past the data of an inline picture, which the cursor is at (just past "ID"): up to "EI"
    // between white space, or to the end.
    void skip_picture() {
        if (at_ < data_.size()) ++at_;

        while (at_ + 1 < data_.size()) {
            const auto at = [&](std::size_t where) { return where < data_.size() ? std::to_integer<int>(data_[where]) : -1; };

            if (at(at_) == 'E' && at(at_ + 1) == 'I' && at_ > 0 && pdf::spacing(at(at_ - 1)) && (at(at_ + 2) == -1 || pdf::spacing(at(at_ + 2)))) {
                at_ += 2;

                return;
            }

            ++at_;
        }

        at_ = data_.size();
    }

private:
    std::span<const std::byte> data_;
    std::size_t at_ = 0;
};

// A range of codes of one length that a CMap reads (codespacerange).
struct Codespace {
    std::size_t bytes = 1;
    std::uint32_t low = 0;
    std::uint32_t high = 0;
};

// A range of codes of a composite font and their width (from /W).
struct Wide {
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    double width = 0;
};

// What the text version needs of a font: how its strings split into codes, the characters of each
// code, and how wide each glyph is, in units of text space per unit of size.
struct Font {
    bool composite = false;
    bool vertical = false;
    std::vector<Codespace> codespace;
    std::map<std::uint32_t, std::string> unicode;
    std::array<std::optional<std::string>, 256> simple;
    std::vector<double> widths;
    std::uint32_t first = 0;
    double missing = 0.5;
    std::vector<Wide> wide;
    double fallback = 1.0;
    double scale = 0.001;

    // The codes of `text` with their bytes.
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::size_t>> codes(std::string_view text) const {
        std::vector<std::pair<std::uint32_t, std::size_t>> made;
        std::size_t at = 0;

        while (at < text.size()) {
            std::size_t much = composite ? 2 : 1;

            if (composite && !codespace.empty()) {
                much = 0;

                for (std::size_t bytes = 1; bytes <= 4 && much == 0 && at + bytes <= text.size(); ++bytes) {
                    const std::uint32_t code = code_of(text.substr(at, bytes));

                    for (const Codespace& one : codespace)
                        if (one.bytes == bytes && code >= one.low && code <= one.high) much = bytes;
                }

                if (much == 0) much = std::min<std::size_t>(codespace.front().bytes, text.size() - at);
            }

            much = std::min(much, text.size() - at);
            made.emplace_back(code_of(text.substr(at, much)), much);
            at += much;
        }

        return made;
    }

    // The characters of `code`, U+FFFD when nothing maps it.
    [[nodiscard]] std::string characters(std::uint32_t code) const {
        if (const auto found = unicode.find(code); found != unicode.end()) return found->second;

        if (!composite && code < 256 && simple[code]) return *simple[code];

        return std::string{replacement};
    }

    // How wide the glyph of `code` is, in units of text space per unit of size.
    [[nodiscard]] double width(std::uint32_t code) const {
        if (composite) {
            for (const Wide& one : wide)
                if (code >= one.low && code <= one.high) return one.width * scale;

            return fallback * scale;
        }

        if (code >= first && code - first < widths.size()) return widths[code - first] * scale;

        return missing * scale;
    }
};

// A glyph as it lands on the page: its characters, the link of the page it lies in (an index into
// the page's links, or -1), where it starts and ends (in the space of the page), the unit vector of
// its direction and its height.
struct Placed {
    std::string text;
    int link = -1;
    double x = 0;
    double y = 0;
    double end_x = 0;
    double end_y = 0;
    double along_x = 1;
    double along_y = 0;
    double height = 1;
};

// The combining mark that the spacing accent `one` stands for, or none.
[[nodiscard]] std::optional<char32_t> accent_of(char32_t one) {
    for (const Accent& accent : accents)
        if (accent.spacing == one) return accent.mark;

    return std::nullopt;
}

// The letter that `base` and `mark` compose to, or none; a dotless i or j takes an accent as i
// and j do, since TeX's fonts put accents over them.
[[nodiscard]] std::optional<char32_t> composed_of(char32_t base, char32_t mark) {
    if (base == 0x0131) base = 'i';
    if (base == 0x0237) base = 'j';

    const auto found = std::lower_bound(composed.begin(), composed.end(), std::pair{base, mark},
                                        [](const Composed& one, const std::pair<char32_t, char32_t>& sought) { return std::pair{one.base, one.mark} < sought; });

    if (found != composed.end() && found->base == base && found->mark == mark) return found->made;

    return std::nullopt;
}

// The lines and paragraphs of a page, made from its glyphs as they come, with the stretches of
// their text that lie in links.
class Layout {
public:
    // A stretch of text (bytes from `from` to `upto`) in the link `link`.
    struct Span {
        std::size_t from = 0;
        std::size_t upto = 0;
        int link = -1;
    };

    // A paragraph: its text and its stretches in links.
    struct Paragraph {
        std::string text;
        std::vector<Span> spans;
    };

    // The glyph `one` added.
    void add(Placed one) {
        if (one.text.empty()) return;

        std::size_t after = 0;
        const std::optional<char32_t> first = text::decoded(one.text, after);
        const bool single = first && after == one.text.size();

        if (single) {
            if (const std::optional<char32_t> mark = accent_of(*first)) {
                if (line_ && over(*last_, one) && joined(*mark)) return;

                accent_ = std::pair{*mark, one};

                return;
            }
        }

        if (accent_) {
            const auto [mark, placed] = *accent_;

            accent_.reset();

            if (single && over(placed, one)) {
                if (const std::optional<char32_t> letter = composed_of(*first, mark)) {
                    one.text.clear();
                    text::encoded(one.text, *letter);
                }
            } else {
                Placed alone = placed;

                alone.text.clear();
                text::encoded(alone.text, accent_spacing(mark));
                placed_(std::move(alone));
            }
        }

        if (unknown_) {
            Placed held = std::move(*unknown_);

            unknown_.reset();

            if (!over(held, one)) placed_(std::move(held));
        }

        if (one.text == replacement) {
            if (!(line_ && over(*last_, one))) unknown_ = std::move(one);

            return;
        }

        placed_(std::move(one));
    }

    // The last glyph added, the one held back included, made to end at (`x`, `y`): the glyphs that
    // the text of marked content (its ActualText) stands for are covered by its first one.
    void widened(double x, double y) {
        Placed* held = accent_ ? &accent_->second : unknown_ ? &*unknown_ : last_ ? &*last_ : nullptr;

        if (held == nullptr) return;

        held->end_x = x;
        held->end_y = y;

        if (!accent_ && !unknown_ && line_) {
            line_->end_x = x;
            line_->end_y = y;
        }
    }

    // The page's paragraphs, the text gathered so far ended.
    [[nodiscard]] std::vector<Paragraph> ended() {
        if (unknown_) {
            placed_(std::move(*unknown_));
            unknown_.reset();
        }

        if (accent_) {
            Placed alone = accent_->second;

            alone.text.clear();
            text::encoded(alone.text, accent_spacing(accent_->first));
            accent_.reset();
            placed_(std::move(alone));
        }

        closed();

        return std::move(blocks_);
    }

private:
    // A line being gathered: its text and its stretches of links, where it starts, the end of its
    // last glyph, its direction and the height of its glyphs.
    struct Line {
        std::string text;
        std::vector<Span> spans;
        double x = 0;
        double y = 0;
        double end_x = 0;
        double end_y = 0;
        double along_x = 1;
        double along_y = 0;
        double height = 1;
    };

    // The spacing form of the combining `mark`.
    [[nodiscard]] static char32_t accent_spacing(char32_t mark) {
        for (const Accent& one : accents)
            if (one.mark == mark) return one.spacing;

        return mark;
    }

    // Whether `top` lies over `under`: their middles along the direction of `under` closer than
    // six tenths of the wider one, and their baselines less than one and a half heights apart.
    [[nodiscard]] static bool over(const Placed& top, const Placed& under) {
        const double middle_top_x = (top.x + top.end_x) / 2;
        const double middle_top_y = (top.y + top.end_y) / 2;
        const double middle_x = (under.x + under.end_x) / 2;
        const double middle_y = (under.y + under.end_y) / 2;
        const double along = (middle_top_x - middle_x) * under.along_x + (middle_top_y - middle_y) * under.along_y;
        const double across = (top.x - under.x) * -under.along_y + (top.y - under.y) * under.along_x;
        const double wide = std::max(std::hypot(top.end_x - top.x, top.end_y - top.y), std::hypot(under.end_x - under.x, under.end_y - under.y));
        const double tall = std::max(under.height, 1e-6);

        return std::abs(along) < 0.6 * std::max(wide, 0.3 * tall) && std::abs(across) < 1.5 * tall;
    }

    // The accent `mark` joined to the last letter of the line, when they compose.
    [[nodiscard]] bool joined(char32_t mark) {
        std::size_t at = line_->text.size();

        while (at > 0 && (static_cast<unsigned char>(line_->text[at - 1]) & 0xC0U) == 0x80U) --at;

        if (at == 0) return false;

        --at;

        std::size_t from = at;
        const std::optional<char32_t> letter = text::decoded(line_->text, from);

        if (!letter) return false;

        const std::optional<char32_t> made = composed_of(*letter, mark);

        if (!made) return false;

        const std::size_t was = line_->text.size();

        line_->text.resize(at);
        text::encoded(line_->text, *made);

        if (!line_->spans.empty() && line_->spans.back().upto == was) line_->spans.back().upto = line_->text.size();

        return true;
    }

    // The glyph `one` put on the line it belongs to.
    void placed_(Placed one) {
        const double tall = std::max({one.height, line_ ? line_->height : 0.0, 1e-6});

        if (line_) {
            const double dot = line_->along_x * one.along_x + line_->along_y * one.along_y;
            const double dx = one.x - line_->end_x;
            const double dy = one.y - line_->end_y;
            const double along = dx * line_->along_x + dy * line_->along_y;
            const double across = dx * -line_->along_y + dy * line_->along_x;

            if (dot > 0.95 && std::abs(across) <= 0.5 * tall && along >= -tall) {
                if (along > 0.2 * tall && !line_->text.empty() && line_->text.back() != ' ' && one.text.front() != ' ') line_->text += ' ';

                appended(*line_, one);
            } else {
                newline(one);
            }
        } else {
            started(one);
        }

        last_ = std::move(one);
    }

    // `one` appended to `line`, a space after a space dropped; in a link, the stretch of the link
    // before it goes on over it (and over a space between them), or a new one starts.
    static void appended(Line& line, const Placed& one) {
        if (one.text == " " && (line.text.empty() || line.text.back() == ' ')) {
            line.end_x = one.end_x;
            line.end_y = one.end_y;

            return;
        }

        const std::size_t from = line.text.size();

        if (one.link >= 0) {
            Span* last = line.spans.empty() ? nullptr : &line.spans.back();
            const bool going = last != nullptr && last->link == one.link && (last->upto == from || (last->upto + 1 == from && line.text.back() == ' '));

            if (going) last->upto = from + one.text.size();
            else line.spans.push_back(Span{from, from + one.text.size(), one.link});
        }

        line.text += one.text;
        line.end_x = one.end_x;
        line.end_y = one.end_y;
        line.height = std::max(line.height, one.height);
    }

    // A new line for `one`: in the same paragraph when it is below the last line by less than one
    // and a half times the step between lines, else in a new one.
    void newline(const Placed& one) {
        const double down = (one.x - line_->x) * line_->along_y + (one.y - line_->y) * -line_->along_x;
        const double pitch = step_ > 0 ? step_ : 1.2 * std::max(line_->height, 1e-6);
        const bool same = down > 0.25 * line_->height && down <= 1.5 * pitch;

        lined();

        if (same) {
            step_ = step_ > 0 ? std::min(step_, down) : down;
            paragraph_ += '\n';
        } else {
            closed();
        }

        started(one);
    }

    // A line started with `one`.
    void started(const Placed& one) {
        Line made;

        made.x = one.x;
        made.y = one.y;
        made.along_x = one.along_x;
        made.along_y = one.along_y;
        made.height = one.height;
        made.end_x = one.x;
        made.end_y = one.y;
        line_ = std::move(made);
        appended(*line_, one);
    }

    // The line being gathered added to the paragraph, its spaces at the ends trimmed, its stretches
    // in links with it.
    void lined() {
        if (!line_) return;

        std::string_view text = line_->text;

        while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
        while (!text.empty() && text.back() == ' ') text.remove_suffix(1);

        const std::size_t lead = static_cast<std::size_t>(text.data() - line_->text.data());
        const std::size_t base = paragraph_.size();

        for (const Span& one : line_->spans) {
            const std::size_t from = std::max(one.from, lead) - lead;
            const std::size_t upto = std::min(one.upto, lead + text.size()) - lead;

            if (from < upto) spans_.push_back(Span{base + from, base + upto, one.link});
        }

        paragraph_ += text;
        line_.reset();
    }

    // The paragraph being gathered ended.
    void closed() {
        lined();

        while (!paragraph_.empty() && paragraph_.back() == '\n') paragraph_.pop_back();

        for (Span& one : spans_) one.upto = std::min(one.upto, paragraph_.size());

        if (!paragraph_.empty()) blocks_.push_back(Paragraph{std::move(paragraph_), std::move(spans_)});

        paragraph_.clear();
        spans_.clear();
        step_ = 0;
    }

    std::optional<Line> line_;
    std::optional<Placed> last_;
    std::optional<std::pair<char32_t, Placed>> accent_;
    std::optional<Placed> unknown_;
    std::string paragraph_;
    std::vector<Span> spans_;
    std::vector<Paragraph> blocks_;
    double step_ = 0;
};


// The codespace and the characters that the CMap `data` gives (a ToUnicode CMap, or an embedded
// one read only for its codespace), at most `most_mapped` codes: begincodespacerange,
// beginbfchar and beginbfrange, whose destination is UTF-16BE, its last unit counted up through
// the range, or an array of one destination a code.
void mapped(std::span<const std::byte> data, Font& into, bool characters) {
    Tokens tokens{data};
    std::vector<Object> operands;
    std::size_t count = 0;

    const auto text = [](const Object& one) -> const std::string* { return pdf::string_of(one); };

    for (;;) {
        Token token = tokens.next();

        if (token.end) return;

        if (token.operand) {
            if (operands.size() < 3 * 100 + 3) operands.push_back(std::move(*token.operand));

            continue;
        }

        const std::string_view word = token.word;

        if (word == "endcodespacerange") {
            for (std::size_t at = 0; at + 1 < operands.size(); at += 2) {
                const std::string* low = text(operands[at]);
                const std::string* high = text(operands[at + 1]);

                if (low != nullptr && high != nullptr && low->size() == high->size() && !low->empty() && low->size() <= 4 && into.codespace.size() < 256)
                    into.codespace.push_back(Codespace{low->size(), code_of(*low), code_of(*high)});
            }
        } else if (word == "endbfchar" && characters) {
            for (std::size_t at = 0; at + 1 < operands.size() && count < most_mapped; at += 2) {
                const std::string* source = text(operands[at]);
                const std::string* target = text(operands[at + 1]);

                if (source == nullptr || source->empty() || source->size() > 4) continue;

                if (target != nullptr) into.unicode[code_of(*source)] = from_utf16(*target);
                else if (const Name* name = pdf::name_of(operands[at + 1])) into.unicode[code_of(*source)] = glyph_text(name->text).value_or(std::string{replacement});
                else continue;

                ++count;
            }
        } else if (word == "endbfrange" && characters) {
            for (std::size_t at = 0; at + 2 < operands.size() && count < most_mapped; at += 3) {
                const std::string* low = text(operands[at]);
                const std::string* high = text(operands[at + 1]);

                if (low == nullptr || high == nullptr || low->empty() || low->size() > 4 || low->size() != high->size()) continue;

                const std::uint32_t first = code_of(*low);
                const std::uint32_t last = code_of(*high);

                if (last < first || last - first >= most_mapped - count) continue;

                if (const std::string* target = text(operands[at + 2]); target != nullptr && target->size() >= 2) {
                    for (std::uint32_t code = first; code <= last; ++code) {
                        std::string units = *target;
                        const std::uint32_t unit = (static_cast<std::uint32_t>(static_cast<unsigned char>(units[units.size() - 2])) << 8U) +
                                                   static_cast<unsigned char>(units.back()) + (code - first);

                        units[units.size() - 2] = static_cast<char>((unit >> 8U) & 0xFFU);
                        units.back() = static_cast<char>(unit & 0xFFU);
                        into.unicode[code] = from_utf16(units);
                        ++count;
                    }
                } else if (const Array* each = pdf::array_of(operands[at + 2])) {
                    for (std::uint32_t code = first; code <= last && code - first < each->size(); ++code) {
                        if (const std::string* one = text((*each)[code - first])) into.unicode[code] = from_utf16(*one);

                        ++count;
                    }
                }
            }
        }

        operands.clear();
    }
}

// The names that the program of an embedded Type 1 font (`program`, its clear part) gives its
// codes: StandardEncoding when it says so, else its "dup code /name put" lines.
[[nodiscard]] std::array<std::optional<std::string>, 256> builtin(std::span<const std::byte> program) {
    std::array<std::optional<std::string>, 256> made;
    const std::string_view text{reinterpret_cast<const char*>(program.data()), program.size()};
    const std::string_view clear = text.substr(0, std::min(text.find("eexec"), std::size_t{1} << 16));

    if (clear.find("/Encoding StandardEncoding def") != std::string_view::npos) {
        for (std::size_t code = 0; code < 256; ++code) {
            if (standard_encoding[code] == 0) continue;

            std::string one;

            put(one, standard_encoding[code]);
            made[code] = std::move(one);
        }

        return made;
    }

    Tokens tokens{std::span<const std::byte>{program.data(), clear.size()}};
    std::vector<Object> operands;

    for (;;) {
        Token token = tokens.next();

        if (token.end) break;

        if (token.operand) {
            if (operands.size() < 4) operands.push_back(std::move(*token.operand));

            continue;
        }

        if (token.word == "put" && operands.size() >= 2) {
            const std::optional<std::int64_t> code = pdf::integer_of(operands[operands.size() - 2]);
            const Name* name = pdf::name_of(operands.back());

            if (code && *code >= 0 && *code < 256 && name != nullptr) made[static_cast<std::size_t>(*code)] = glyph_text(name->text);
        }

        operands.clear();
    }

    return made;
}

// The big-endian number of `size` bytes at `at` in `data`, or none past its end.
[[nodiscard]] std::optional<std::uint32_t> number_at(std::span<const std::byte> data, std::size_t at, std::size_t size) {
    if (at > data.size() || size > data.size() - at) return std::nullopt;

    std::uint32_t made = 0;

    for (std::size_t one = 0; one < size; ++one) made = (made << 8U) | std::to_integer<std::uint32_t>(data[at + one]);

    return made;
}

// The glyphs of the codes in one subtable of a TrueType 'cmap' (formats 0, 4, 6 and 12), at most
// 0x20000 of them; nothing for a format not read here or a table that runs out.
[[nodiscard]] std::map<std::uint32_t, std::uint32_t> subtable(std::span<const std::byte> data, std::size_t at) {
    std::map<std::uint32_t, std::uint32_t> made;
    const std::optional<std::uint32_t> format = number_at(data, at, 2);
    constexpr std::size_t most = 0x20000;

    if (format == 0u) {
        for (std::uint32_t code = 0; code < 256; ++code)
            if (const auto glyph = number_at(data, at + 6 + code, 1); glyph && *glyph != 0) made[code] = *glyph;
    } else if (format == 4u) {
        const std::optional<std::uint32_t> doubled = number_at(data, at + 6, 2);

        if (!doubled) return made;

        const std::size_t segments = *doubled / 2;
        const std::size_t ends = at + 14;
        const std::size_t starts = ends + 2 * segments + 2;
        const std::size_t deltas = starts + 2 * segments;
        const std::size_t ranges = deltas + 2 * segments;

        for (std::size_t one = 0; one < segments && made.size() < most; ++one) {
            const auto end = number_at(data, ends + 2 * one, 2);
            const auto start = number_at(data, starts + 2 * one, 2);
            const auto delta = number_at(data, deltas + 2 * one, 2);
            const auto offset = number_at(data, ranges + 2 * one, 2);

            if (!end || !start || !delta || !offset || *start > *end) continue;

            for (std::uint32_t code = *start; code <= *end && made.size() < most; ++code) {
                std::uint32_t glyph = 0;

                if (*offset == 0) {
                    glyph = (code + *delta) & 0xFFFFU;
                } else {
                    const auto found = number_at(data, ranges + 2 * one + *offset + 2 * (code - *start), 2);

                    if (!found) break;

                    glyph = *found == 0 ? 0 : (*found + *delta) & 0xFFFFU;
                }

                if (glyph != 0) made[code] = glyph;

                if (code == 0xFFFF) break;
            }
        }
    } else if (format == 6u) {
        const auto first = number_at(data, at + 6, 2);
        const auto count = number_at(data, at + 8, 2);

        for (std::uint32_t one = 0; first && count && one < *count; ++one)
            if (const auto glyph = number_at(data, at + 10 + 2 * one, 2); glyph && *glyph != 0) made[*first + one] = *glyph;
    } else if (format == 12u) {
        const auto groups = number_at(data, at + 12, 4);

        for (std::uint32_t group = 0; groups && group < *groups && made.size() < most; ++group) {
            const auto start = number_at(data, at + 16 + 12 * group, 4);
            const auto end = number_at(data, at + 20 + 12 * group, 4);
            const auto glyph = number_at(data, at + 24 + 12 * group, 4);

            if (!start || !end || !glyph || *start > *end || *end > 0x10FFFF) break;

            for (std::uint32_t code = *start; code <= *end && made.size() < most; ++code) made[code] = *glyph + (code - *start);
        }
    }

    return made;
}

// The characters of the codes of a symbolic TrueType font without an encoding, from its program
// (`program`, FontFile2): each code's glyph by the symbol subtable (3,0), at the code or at
// 0xF000 plus it, or by the Macintosh one (1,0), and that glyph's character by the Unicode
// subtable (3,1 or 3,10) read backwards.
[[nodiscard]] std::array<std::optional<std::string>, 256> symbolic(std::span<const std::byte> program) {
    std::array<std::optional<std::string>, 256> made;
    const std::optional<std::uint32_t> tables = number_at(program, 4, 2);
    std::optional<std::size_t> cmap;

    for (std::uint32_t one = 0; tables && one < *tables; ++one) {
        const auto tag = number_at(program, 12 + 16 * one, 4);
        const auto offset = number_at(program, 12 + 16 * one + 8, 4);

        if (tag == 0x636D6170u && offset) cmap = *offset;
    }

    if (!cmap) return made;

    std::map<std::uint32_t, std::uint32_t> symbol;
    std::map<std::uint32_t, std::uint32_t> mac;
    std::map<std::uint32_t, char32_t> unicode;
    const std::optional<std::uint32_t> count = number_at(program, *cmap + 2, 2);

    for (std::uint32_t one = 0; count && one < std::min<std::uint32_t>(*count, 64); ++one) {
        const auto platform = number_at(program, *cmap + 4 + 8 * one, 2);
        const auto encoding = number_at(program, *cmap + 6 + 8 * one, 2);
        const auto offset = number_at(program, *cmap + 8 + 8 * one, 4);

        if (!platform || !encoding || !offset) continue;

        const std::size_t at = *cmap + *offset;

        if (*platform == 3 && *encoding == 0) symbol = subtable(program, at);
        else if (*platform == 1 && *encoding == 0) mac = subtable(program, at);
        else if (*platform == 3 && (*encoding == 1 || *encoding == 10))
            for (const auto& [code, glyph] : subtable(program, at)) unicode.try_emplace(glyph, static_cast<char32_t>(code));
    }

    for (std::uint32_t code = 0; code < 256; ++code) {
        std::optional<std::uint32_t> glyph;

        if (const auto found = symbol.find(0xF000 + code); found != symbol.end()) glyph = found->second;
        else if (const auto at = symbol.find(code); at != symbol.end()) glyph = at->second;
        else if (const auto in = mac.find(code); in != mac.end()) glyph = in->second;

        if (!glyph) continue;

        if (const auto character = unicode.find(*glyph); character != unicode.end()) {
            std::string one;

            put(one, character->second);
            made[code] = std::move(one);
        }
    }

    return made;
}

// The fonts, the file and the budget of one opened document, which the text of its pages reads.
struct Session {
    Session(meta::Reader& from, std::uint64_t size) : gathered(description), file(from, size, gathered) { gathered.spent->size = size; }

    meta::Description description;
    meta::tagging::Gathered gathered;
    pdf::File file;
    std::vector<pdf::Leaf> leaves;
    std::map<const Dict*, std::unique_ptr<Font>> fonts;
};

// The decoded data of the stream that `object` is or names, or none. When it is the content of a
// page (`content`) and does not decode, the text would go missing without a word, so that is
// Trouble: full when the file's budget of unpacking is spent, broken otherwise.
Task<std::optional<Bytes>> stream_data(Session& session, const Object* object, bool content = false) {
    const Object* resolved = co_await session.file.resolved(object);
    const Stream* stream = pdf::stream_of(*resolved);

    if (stream == nullptr) co_return std::nullopt;

    std::optional<Bytes> data = co_await session.file.decoded(*stream);

    if (!data && content) {
        if (session.gathered.unpackable() == 0) throw Trouble{Kind::full, "The document unpacks to more than its text version may read."};

        throw Trouble{Kind::broken, "The content of the page could not be read."};
    }

    co_return data;
}

// A number among the operands, or zero.
[[nodiscard]] double number(const std::vector<Object>& operands, std::size_t at) {
    if (at >= operands.size()) return 0;

    const std::optional<double> one = pdf::number_of(operands[at]);

    return one && std::isfinite(*one) ? *one : 0;
}

// The font of the dictionary `dict`, read as the text version needs it.
Task<std::unique_ptr<Font>> font_of(Session& session, const Dict& dict) {
    auto made = std::make_unique<Font>();
    pdf::File& file = session.file;
    const Name* subtype = pdf::name_of(*co_await file.resolved(dict.find("Subtype")));
    const bool composite = subtype != nullptr && subtype->text == "Type0";

    const std::optional<Bytes> cmap = co_await stream_data(session, dict.find("ToUnicode"));

    if (cmap && cmap->size() <= most_cmap) mapped(std::span<const std::byte>{*cmap}, *made, true);

    if (composite) {
        made->composite = true;

        const Object* encoding = co_await file.resolved(dict.find("Encoding"));

        if (const Name* name = pdf::name_of(*encoding)) {
            made->vertical = name->text.ends_with("-V");
        } else if (pdf::stream_of(*encoding) != nullptr && made->codespace.empty()) {
            const std::optional<Bytes> embedded = co_await stream_data(session, encoding);

            if (embedded && embedded->size() <= most_cmap) mapped(std::span<const std::byte>{*embedded}, *made, false);
        }

        if (made->codespace.empty()) made->codespace.push_back(Codespace{2, 0, 0xFFFF});

        const Array* descendants = pdf::array_of(*co_await file.resolved(dict.find("DescendantFonts")));
        const Object* first_descendant = descendants == nullptr || descendants->empty() ? nullptr : &descendants->front();
        const Dict* descendant = pdf::dict_of(*co_await file.resolved(first_descendant));

        if (descendant != nullptr) {
            const std::optional<double> fallback = pdf::number_of(*co_await file.resolved(descendant->find("DW")));

            if (fallback && std::isfinite(*fallback)) made->fallback = *fallback;

            const Array* widths = pdf::array_of(*co_await file.resolved(descendant->find("W")));

            for (std::size_t at = 0; widths != nullptr && at < widths->size() && made->wide.size() < 65536;) {
                const std::optional<std::int64_t> first = pdf::integer_of(*co_await file.resolved(&(*widths)[at]));

                if (!first || *first < 0 || at + 1 >= widths->size()) break;

                const Object* next = co_await file.resolved(&(*widths)[at + 1]);

                if (const Array* each = pdf::array_of(*next)) {
                    for (std::size_t one = 0; one < each->size() && made->wide.size() < 65536; ++one) {
                        const std::optional<double> width = pdf::number_of(*co_await file.resolved(&(*each)[one]));
                        const auto code = static_cast<std::uint32_t>(*first + static_cast<std::int64_t>(one));

                        if (width) made->wide.push_back(Wide{code, code, *width});
                    }

                    at += 2;
                } else {
                    const std::optional<std::int64_t> last = pdf::integer_of(*next);
                    const Object* third = at + 2 < widths->size() ? &(*widths)[at + 2] : nullptr;
                    const std::optional<double> width = pdf::number_of(*co_await file.resolved(third));

                    if (!last || !width || *last < *first) break;

                    made->wide.push_back(Wide{static_cast<std::uint32_t>(*first), static_cast<std::uint32_t>(std::min<std::int64_t>(*last, 0xFFFFFFFF)), *width});
                    at += 3;
                }
            }
        }

        co_return made;
    }

    const Dict* descriptor = pdf::dict_of(*co_await file.resolved(dict.find("FontDescriptor")));

    const bool type3 = subtype != nullptr && subtype->text == "Type3";

    if (type3) {
        made->missing = 0;

        const Array* matrix = pdf::array_of(*co_await file.resolved(dict.find("FontMatrix")));
        const Object* first_number = matrix != nullptr && !matrix->empty() ? &matrix->front() : nullptr;
        const std::optional<double> scale = pdf::number_of(*co_await file.resolved(first_number));

        if (scale && std::isfinite(*scale) && *scale != 0) made->scale = *scale;
    }

    const std::optional<double> missing = pdf::number_of(*co_await file.resolved(descriptor != nullptr ? descriptor->find("MissingWidth") : nullptr));

    if (missing && std::isfinite(*missing)) made->missing = *missing;

    const std::optional<std::int64_t> first = pdf::integer_of(*co_await file.resolved(dict.find("FirstChar")));

    if (first && *first >= 0 && *first < 256) made->first = static_cast<std::uint32_t>(*first);

    const Array* widths = pdf::array_of(*co_await file.resolved(dict.find("Widths")));

    if (widths != nullptr) {
        for (std::size_t at = 0; at < std::min<std::size_t>(widths->size(), 256); ++at) {
            const std::optional<double> width = pdf::number_of(*co_await file.resolved(&(*widths)[at]));

            made->widths.push_back(width && std::isfinite(*width) ? *width : made->missing);
        }
    }

    const Object* encoding = co_await file.resolved(dict.find("Encoding"));
    const Dict* differing = pdf::dict_of(*encoding);
    const Object* based = co_await file.resolved(differing != nullptr ? differing->find("BaseEncoding") : nullptr);
    const Name* base = differing != nullptr ? pdf::name_of(*based) : pdf::name_of(*encoding);
    const std::array<char32_t, 256>* table = nullptr;
    std::array<std::optional<std::string>, 256> program;

    if (base != nullptr && base->text == "WinAnsiEncoding") table = &win_ansi_encoding;
    else if (base != nullptr && base->text == "MacRomanEncoding") table = &mac_roman_encoding;
    else if (base != nullptr && base->text == "StandardEncoding") table = &standard_encoding;

    if (table == nullptr && descriptor != nullptr) {
        const std::optional<std::int64_t> flags = pdf::integer_of(*co_await file.resolved(descriptor->find("Flags")));
        const bool symbol = flags && (*flags & 4) != 0;

        const std::optional<Bytes> type1 = co_await stream_data(session, descriptor->find("FontFile"));

        if (type1) {
            program = builtin(std::span<const std::byte>{*type1});
        } else if (symbol && subtype != nullptr && subtype->text == "TrueType") {
            const std::optional<Bytes> truetype = co_await stream_data(session, descriptor->find("FontFile2"));

            if (truetype) program = symbolic(std::span<const std::byte>{*truetype});
        }
    }

    const bool built = std::any_of(program.begin(), program.end(), [](const std::optional<std::string>& one) { return one.has_value(); });

    for (std::size_t code = 0; code < 256; ++code) {
        if (table != nullptr || !built) {
            const char32_t one = (table != nullptr ? *table : standard_encoding)[code];

            if (one != 0) {
                std::string text;

                put(text, one);
                made->simple[code] = std::move(text);
            }
        } else {
            made->simple[code] = program[code];
        }
    }

    const Array* differences = pdf::array_of(*co_await file.resolved(differing != nullptr ? differing->find("Differences") : nullptr));

    if (differences != nullptr) {
        std::int64_t code = -1;
        std::vector<std::pair<std::size_t, std::string_view>> coded;
        bool letters = false;

        for (const Object& one : *differences) {
            if (const std::optional<std::int64_t> start = pdf::integer_of(one)) {
                code = *start;
            } else if (const Name* name = pdf::name_of(one)) {
                if (code >= 0 && code < 256) {
                    const auto at = static_cast<std::size_t>(code);

                    made->simple[at] = glyph_text(name->text);

                    if (!made->simple[at] && type3) coded.emplace_back(at, name->text);

                    letters = letters || (at >= 'A' && at <= 'Z') || (at >= 'a' && at <= 'z');
                }

                ++code;
            }
        }

        if (letters)
            for (const auto& [at, name] : coded) made->simple[at] = coded_glyph(name);
    }

    co_return made;
}

// A link of a page to an address on the web: its rectangle in the space of the page and the
// address.
struct Linked {
    double left = 0;
    double bottom = 0;
    double right = 0;
    double top = 0;
    std::string address;
};

// The text state of PDF and the transformation, as q and Q save and restore them.
struct State {
    Matrix ctm;
    const Font* font = nullptr;
    double size = 0;
    double spacing = 0;
    double words = 0;
    double scale = 1;
    double leading = 0;
    double rise = 0;
};

// The reading of the content of one page into a layout.
class Reading {
public:
    // A reading into `layout`, the glyphs that lie in one of `links` (rectangles of the page) told
    // so.
    Reading(Session& session, Layout& layout, const std::vector<Linked>& links) : session_(session), layout_(layout), links_(links) {}

    [[nodiscard]] bool cut() const { return cut_; }

    // The content `data` run with the resources `resources`, from the transformation `ctm`, as the
    // content of a form `depth` forms deep.
    Task<void> run(std::span<const std::byte> data, const Dict* resources, Matrix ctm, std::size_t depth) {
        Tokens tokens{data};
        std::vector<Object> operands;
        std::vector<State> saved;
        State state;
        Matrix line;
        Matrix text;
        std::vector<Span> marked;
        std::size_t beyond = 0;

        state.ctm = ctm;

        for (;;) {
            Token token = tokens.next();

            if (token.end || cut_) break;

            if (token.operand) {
                if (operands.size() < 64) operands.push_back(std::move(*token.operand));

                continue;
            }

            if (++operators_ > most_operators) {
                cut_ = true;

                break;
            }

            const std::string_view word = token.word;

            if (word == "q") {
                if (saved.size() < 256) saved.push_back(state);
            } else if (word == "Q") {
                if (!saved.empty()) {
                    state = saved.back();
                    saved.pop_back();
                }
            } else if (word == "cm" && operands.size() >= 6) {
                const Matrix by{number(operands, 0), number(operands, 1), number(operands, 2), number(operands, 3), number(operands, 4), number(operands, 5)};

                state.ctm = by.times(state.ctm);
            } else if (word == "BT") {
                text = Matrix{};
                line = Matrix{};
            } else if (word == "Tf" && operands.size() >= 2) {
                const Name* name = pdf::name_of(operands[0]);

                state.size = number(operands, 1);
                state.font = co_await font_named(resources, name != nullptr ? name->text : std::string_view{});
            } else if (word == "Tc" && !operands.empty()) {
                state.spacing = number(operands, 0);
            } else if (word == "Tw" && !operands.empty()) {
                state.words = number(operands, 0);
            } else if (word == "Tz" && !operands.empty()) {
                state.scale = number(operands, 0) / 100;
            } else if (word == "TL" && !operands.empty()) {
                state.leading = number(operands, 0);
            } else if (word == "Ts" && !operands.empty()) {
                state.rise = number(operands, 0);
            } else if ((word == "Td" || word == "TD") && operands.size() >= 2) {
                if (word == "TD") state.leading = -number(operands, 1);

                line = moved(number(operands, 0), number(operands, 1)).times(line);
                text = line;
            } else if (word == "Tm" && operands.size() >= 6) {
                line = Matrix{number(operands, 0), number(operands, 1), number(operands, 2), number(operands, 3), number(operands, 4), number(operands, 5)};
                text = line;
            } else if (word == "T*" || word == "'" || word == "\"") {
                if (word == "\"" && operands.size() >= 3) {
                    state.words = number(operands, 0);
                    state.spacing = number(operands, 1);
                }

                line = moved(0, -state.leading).times(line);
                text = line;

                if (word != "T*" && !operands.empty())
                    if (const std::string* shown = pdf::string_of(operands.back())) showed(*shown, state, text, marked);
            } else if (word == "Tj" && !operands.empty()) {
                if (const std::string* shown = pdf::string_of(operands.back())) showed(*shown, state, text, marked);
            } else if (word == "TJ" && !operands.empty()) {
                if (const Array* parts = pdf::array_of(operands.back())) {
                    for (const Object& part : *parts) {
                        if (const std::string* shown = pdf::string_of(part)) {
                            showed(*shown, state, text, marked);
                        } else if (const std::optional<double> shift = pdf::number_of(part); shift && std::isfinite(*shift)) {
                            const double by = -*shift / 1000 * state.size;

                            text = (state.font != nullptr && state.font->vertical ? moved(0, by) : moved(by * state.scale, 0)).times(text);
                        }
                    }
                }
            } else if (word == "Do" && !operands.empty() && depth < form_depth) {
                if (const Name* name = pdf::name_of(operands[0])) co_await formed(resources, name->text, state.ctm, depth);
            } else if ((word == "BMC" || word == "BDC") && marked.size() >= most_marked) {
                ++beyond;
            } else if (word == "BMC") {
                marked.emplace_back();
            } else if (word == "BDC") {
                Span span;

                span.text = co_await actual(resources, operands.size() >= 2 ? &operands[1] : nullptr);
                marked.push_back(std::move(span));
            } else if (word == "EMC" && beyond > 0) {
                --beyond;
            } else if (word == "EMC") {
                if (!marked.empty()) marked.pop_back();
            } else if (word == "BI") {
                pictured(tokens);
            }

            operands.clear();
        }
    }

private:
    // Marked content being read: its ActualText, if it has one, and whether that text was placed
    // already, for the first glyph within it.
    struct Span {
        std::optional<std::string> text;
        bool placed = false;
    };

    // The glyphs of `shown` placed in the text state `state` from the text matrix `text`, which
    // moves past them. Within marked content with ActualText (the outermost that has one, whose
    // text stands for all within it, marked content inside included), the first glyph stands for
    // that text and the others widen it to the end of the last of them, so that what follows the
    // marked content is spaced from all it covers; when the ActualText is empty, the glyphs within
    // it give no text at all.
    void showed(std::string_view shown, const State& state, Matrix& text, std::vector<Span>& marked) {
        if (state.font == nullptr) return;

        const Font& font = *state.font;
        const Matrix size{state.size * state.scale, 0, 0, state.size, 0, state.rise};
        Span* actual = nullptr;

        for (Span& one : marked) {
            if (one.text) {
                actual = &one;

                break;
            }
        }

        for (const auto& [code, bytes] : font.codes(shown)) {
            const Matrix start = size.times(text).times(state.ctm);
            const double width = font.width(code);
            const double space = bytes == 1 && code == 32 ? state.words : 0;
            const Matrix next = (font.vertical ? moved(0, -state.size + state.spacing + space) : moved((width * state.size + state.spacing + space) * state.scale, 0)).times(text);
            const Matrix end = size.times(next).times(state.ctm);

            text = next;

            if (!start.finite() || !end.finite()) continue;

            Placed placed;

            placed.x = start.e;
            placed.y = start.f;
            placed.end_x = end.e;
            placed.end_y = end.f;

            const double moved_x = end.e - start.e;
            const double moved_y = end.f - start.f;
            const double length = std::hypot(moved_x, moved_y);
            const double base = std::hypot(start.a, start.b);

            if (length > 1e-9) {
                placed.along_x = moved_x / length;
                placed.along_y = moved_y / length;
            } else if (base > 1e-9) {
                placed.along_x = start.a / base;
                placed.along_y = start.b / base;
            }

            placed.height = std::max(font.vertical ? base : std::hypot(start.c, start.d), 1e-6);
            placed.link = linked((placed.x + placed.end_x) / 2 - 0.3 * placed.height * placed.along_y,
                                 (placed.y + placed.end_y) / 2 + 0.3 * placed.height * placed.along_x);

            if (actual != nullptr && actual->placed) {
                if (!actual->text->empty()) layout_.widened(placed.end_x, placed.end_y);

                continue;
            }

            if (actual != nullptr) {
                placed.text = *actual->text;
                actual->placed = true;
            } else {
                placed.text = font.characters(code);
            }

            layout_.add(std::move(placed));
        }
    }

    // The first of the page's links whose rectangle holds the point (`x`, `y`), or -1.
    [[nodiscard]] int linked(double x, double y) const {
        for (std::size_t at = 0; at < links_.size(); ++at) {
            const Linked& one = links_[at];

            if (x >= one.left && x <= one.right && y >= one.bottom && y <= one.top) return static_cast<int>(at);
        }

        return -1;
    }

    // The font that the resources `resources` name `name`, read once for the document.
    Task<const Font*> font_named(const Dict* resources, std::string_view name) {
        if (resources == nullptr) co_return nullptr;

        const Dict* fonts = pdf::dict_of(*co_await session_.file.resolved(resources->find("Font")));
        const Dict* dict = pdf::dict_of(*co_await session_.file.resolved(fonts != nullptr && !name.empty() ? fonts->find(name) : nullptr));

        if (dict == nullptr) co_return nullptr;

        if (const auto found = session_.fonts.find(dict); found != session_.fonts.end()) co_return found->second.get();

        if (session_.fonts.size() >= most_fonts) co_return nullptr;

        std::unique_ptr<Font> made = co_await font_of(session_, *dict);
        const Font* kept = made.get();

        session_.fonts.emplace(dict, std::move(made));

        co_return kept;
    }

    // The form that the resources `resources` name `name` run, from `ctm` times its matrix, with
    // its own resources or else these; its data decoded once for the page. Past `most_forms` forms
    // drawn the page is cut.
    Task<void> formed(const Dict* resources, std::string_view name, Matrix ctm, std::size_t depth) {
        if (resources == nullptr) co_return;

        const Dict* objects = pdf::dict_of(*co_await session_.file.resolved(resources->find("XObject")));
        const Object* object = objects == nullptr ? nullptr : objects->find(name);
        const Object* resolved = co_await session_.file.resolved(object);
        const Stream* stream = pdf::stream_of(*resolved);

        if (stream == nullptr || !pdf::named(stream->dict.find("Subtype"), "Form")) co_return;

        if (++drawn_ > most_forms) {
            cut_ = true;

            co_return;
        }

        Matrix by;

        const Array* matrix = pdf::array_of(*co_await session_.file.resolved(stream->dict.find("Matrix")));

        if (matrix != nullptr && matrix->size() == 6) {
            std::vector<Object> numbers{matrix->begin(), matrix->end()};

            by = Matrix{number(numbers, 0), number(numbers, 1), number(numbers, 2), number(numbers, 3), number(numbers, 4), number(numbers, 5)};
        }

        const Dict* own = pdf::dict_of(*co_await session_.file.resolved(stream->dict.find("Resources")));
        std::shared_ptr<const Bytes> data;

        if (const auto found = forms_.find(stream); found != forms_.end()) {
            data = found->second;
        } else {
            std::optional<Bytes> read = co_await stream_data(session_, resolved);

            if (read) data = std::make_shared<const Bytes>(std::move(*read));

            forms_.emplace(stream, data);
        }

        if (!data) co_return;

        co_await run(std::span<const std::byte>{*data}, own != nullptr ? own : resources, by.times(ctm), depth + 1);
    }

    // The ActualText of the properties `properties` of marked content (a dictionary, or a name in
    // the resources), or none.
    Task<std::optional<std::string>> actual(const Dict* resources, const Object* properties) {
        if (properties == nullptr) co_return std::nullopt;

        const Dict* dict = pdf::dict_of(*properties);

        const Name* name = pdf::name_of(*properties);

        if (dict == nullptr && resources != nullptr && name != nullptr) {
            const Dict* named = pdf::dict_of(*co_await session_.file.resolved(resources->find("Properties")));
            const Object* entry = named != nullptr ? named->find(name->text) : nullptr;

            dict = pdf::dict_of(*co_await session_.file.resolved(entry));
        }

        if (dict == nullptr) co_return std::nullopt;

        const std::string* text = pdf::string_of(*co_await session_.file.resolved(dict->find("ActualText")));

        if (text == nullptr) co_return std::nullopt;

        const std::string decoded = pdf::text_of(*text);
        std::string made;

        for (std::size_t at = 0; at < decoded.size();) put(made, text::decoded(decoded, at).value_or(0xFFFD));

        co_return made;
    }

    // Past an inline picture: its dictionary up to ID, then its data up to EI.
    static void pictured(Tokens& tokens) {
        for (;;) {
            const Token token = tokens.next();

            if (token.end) return;

            if (!token.operand && token.word == "ID") break;
        }

        tokens.skip_picture();
    }

    Session& session_;
    Layout& layout_;
    const std::vector<Linked>& links_;
    std::map<const Stream*, std::shared_ptr<const Bytes>> forms_;
    std::size_t drawn_ = 0;
    std::size_t operators_ = 0;
    bool cut_ = false;
};

// The text version of a PDF document read by our reader, a page at a time.
class Read final : public Prose {
public:
    Read(std::shared_ptr<meta::Reader> from, std::uint64_t size) : from_(std::move(from)), size_(size) {}

    // The file opened and its pages listed. Throws `Trouble`: denied for an encrypted file,
    // malformed for one without a catalog.
    Task<void> opened() {
        auto session = std::make_unique<Session>(*from_, size_);
        const Dict* catalog = co_await session->file.opened();

        if (session->file.sealed()) throw Trouble{Kind::denied, "The document is encrypted, so it has no text version."};

        if (catalog == nullptr) throw Trouble{Kind::malformed, "The document has no catalog."};

        session->leaves = co_await session->file.leaves(*catalog);

        if (!session_) count_ = session->leaves.size();

        session_ = std::move(session);
    }

    // How many pages the document has.
    [[nodiscard]] std::size_t pages() const override { return count_; }

    // The text of the page `which` (from 0): a block of the page's number, then its paragraphs.
    // Throws `Trouble`: other for a page that is not there, busy while another page is read, broken
    // for content that does not decode, full for content past the file's budget or over
    // `most_packet` together, what opening the file again throws.
    Task<Plain> page(std::size_t which) override {
        if (which >= count_) throw Trouble{Kind::other, "No such page."};

        if (busy_) throw Trouble{Kind::busy, "Another page of the text is being read."};

        Guard guard{*this};

        busy_ = true;

        if (!session_) co_await opened();

        if (which >= session_->leaves.size()) throw Trouble{Kind::other, "No such page."};

        const pdf::Leaf leaf = session_->leaves[which];
        const std::vector<Linked> links = co_await linked(*leaf.page);
        Layout layout;
        Reading reading{*session_, layout, links};
        const Object* contents = co_await session_->file.resolved(leaf.page->find("Contents"));
        Bytes all;

        if (const Array* parts = pdf::array_of(*contents)) {
            for (std::size_t at = 0; at < parts->size(); ++at) {
                const std::optional<Bytes> part = co_await stream_data(*session_, &(*parts)[at], true);

                if (!part) continue;

                if (all.size() + part->size() > meta::tagging::most_packet) throw Trouble{Kind::full, "The content of the page is too large."};

                all.insert(all.end(), part->begin(), part->end());
                all.push_back(std::byte{'\n'});
            }
        } else {
            std::optional<Bytes> one = co_await stream_data(*session_, contents, true);

            if (one) all = std::move(*one);
        }

        const Dict* resources = pdf::dict_of(*co_await session_->file.resolved(leaf.from.resources));

        co_await reading.run(std::span<const std::byte>{all}, resources, Matrix{}, 0);

        Plain made;
        Block number;

        number.kind = Block::Kind::page;
        number.text = std::to_string(which + 1);
        made.blocks.push_back(std::move(number));

        for (Layout::Paragraph& paragraph : layout.ended()) {
            Block one;

            one.text = std::move(paragraph.text);

            for (const Layout::Span& span : paragraph.spans) {
                const std::string& address = links[static_cast<std::size_t>(span.link)].address;
                Anchor* last = one.anchors.empty() ? nullptr : &one.anchors.back();
                const std::size_t end = last == nullptr ? std::string::npos : one.text.find('\n', last->upto);
                const bool going = end != std::string::npos && last->address == address && span.from == end + 1 &&
                                   one.text[last->upto - 1] != ' ' && one.text.find(' ', last->upto) >= end;

                if (going) last->upto = span.upto;
                else one.anchors.push_back(Anchor{0, span.from, span.upto, address});
            }

            for (Anchor& anchor : one.anchors) {
                while (anchor.upto > anchor.from && one.text[anchor.upto - 1] == ' ') --anchor.upto;
                while (anchor.from < anchor.upto && one.text[anchor.from] == ' ') ++anchor.from;
            }

            std::erase_if(one.anchors, [](const Anchor& anchor) { return anchor.from >= anchor.upto; });

            made.blocks.push_back(std::move(one));
        }

        made.cut = reading.cut();
        guard.done = true;

        co_return made;
    }

private:
    // The links of `page` to addresses on the web, at most `most_links`: its annotations of the
    // subtype Link whose action is URI, with a rectangle of four numbers. An address must be of
    // printable ASCII, as ISO 32000 has URIs; others are left out rather than shown or followed.
    Task<std::vector<Linked>> linked(const Dict& page) {
        std::vector<Linked> made;
        const Array* annotations = pdf::array_of(*co_await session_->file.resolved(page.find("Annots")));

        for (std::size_t at = 0; annotations != nullptr && at < annotations->size() && made.size() < most_links; ++at) {
            const Dict* annotation = pdf::dict_of(*co_await session_->file.resolved(&(*annotations)[at]));

            if (annotation == nullptr || !pdf::named(annotation->find("Subtype"), "Link")) continue;

            const Dict* action = pdf::dict_of(*co_await session_->file.resolved(annotation->find("A")));
            const Array* rect = pdf::array_of(*co_await session_->file.resolved(annotation->find("Rect")));

            if (action == nullptr || rect == nullptr || rect->size() != 4 || !pdf::named(action->find("S"), "URI")) continue;

            const std::string* address = pdf::string_of(*co_await session_->file.resolved(action->find("URI")));

            if (address == nullptr || address->empty() || !std::all_of(address->begin(), address->end(), [](char one) { return one > ' ' && one < 0x7F; })) continue;

            std::vector<Object> corners{rect->begin(), rect->end()};
            Linked one;

            one.left = std::min(number(corners, 0), number(corners, 2));
            one.right = std::max(number(corners, 0), number(corners, 2));
            one.bottom = std::min(number(corners, 1), number(corners, 3));
            one.top = std::max(number(corners, 1), number(corners, 3));
            one.address = *address;
            made.push_back(std::move(one));
        }

        co_return made;
    }

    // Marks the document free again when a reading ends, and drops the opened file when it ended
    // half way (cancelled or failed), so that the next reading opens it again.
    struct Guard {
        Read& of;
        bool done = false;

        ~Guard() {
            of.busy_ = false;

            if (!done) of.session_.reset();
        }
    };

    std::shared_ptr<meta::Reader> from_;
    std::uint64_t size_;
    std::unique_ptr<Session> session_;
    std::size_t count_ = 0;
    bool busy_ = false;
};

}

// The text version of the PDF document `from`, its file opened and its pages listed. Throws
// `Trouble`: unsupported for a file that is no PDF, denied for an encrypted one, malformed for one
// without a catalog, what reading throws.
Task<std::shared_ptr<Prose>> prose(std::shared_ptr<meta::Reader> from) {
    const std::uint64_t size = co_await from->size();
    const bool pdf = co_await meta::tagging::pdf_at(*from, size);

    if (!pdf) throw Trouble{Kind::unsupported, "This is not a PDF document."};

    auto made = std::make_shared<Read>(std::move(from), size);

    co_await made->opened();

    co_return made;
}

}
