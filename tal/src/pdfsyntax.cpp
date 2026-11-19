#include "pdfing.hpp"

#include <array>
#include <charconv>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#include "tal/text.hpp"

#include "tagging.hpp"

// The syntax of PDF objects (ISO 32000-2, 7.2 and 7.3), ported from talcite-adapters' tags: the
// lexer and the parser of one object over bytes in memory, and the decoding of text strings. The
// file structure (cross-reference, object streams, the page tree) lives in `pdf.cpp`; this file
// knows nothing about the file, so the same parser reads an object at a file offset and one inside
// a decompressed object stream.
//
// The text is a foreign file, so the parser holds itself to limits rather than to what the file
// says: containers nest at most `deepest` levels deep, and one object holds at most `most_items`
// values in all its containers together. Without the second limit a megabyte of "[" and "0"
// would become a million objects in memory. Running out of bytes is not a syntax error: `cut`
// says the object went on past the end of the text, and the caller who read a window of the file
// can read a larger one and try again.
//
// Where it differs from the adapter's parser, on purpose: `integer` says `cut` when the text ends
// where the number would be, and so does a single ">" at the end of the text inside a dictionary,
// which may be the first half of ">>"; `eol` says `cut` when the text ends after the CR of
// "stream", since the LF may follow. The smallest 64-bit integer is an integer, where the adapter
// made it a real (it took the sign off before reading the digits), and a real too large for a
// double is infinite, which every size check refuses, where the adapter made it zero.
namespace tal::meta::tagging::pdf {

namespace {

// The longest name kept; the standard allows 127 bytes, a real file never needs more than this.
constexpr std::size_t longest_name = 4096;

// The characters of PDFDocEncoding that differ from Latin-1 (ISO 32000-2, Annex D), from 0x18 to
// 0x1F and from 0x80 to 0xA0; zero marks a code that is undefined.
constexpr std::array<char32_t, 8> low_marks{0x02D8, 0x02C7, 0x02C6, 0x02D9, 0x02DD, 0x02DB, 0x02DA, 0x02DC};

constexpr std::array<char32_t, 33> high_marks{
    0x2022, 0x2020, 0x2021, 0x2026, 0x2014, 0x2013, 0x0192, 0x2044, 0x2039, 0x203A, 0x2212,
    0x2030, 0x201E, 0x201C, 0x201D, 0x2018, 0x2019, 0x201A, 0x2122, 0xFB01, 0xFB02, 0x0141,
    0x0152, 0x0160, 0x0178, 0x017D, 0x0131, 0x0142, 0x0153, 0x0161, 0x017E, 0,      0x20AC};

// The value of a hexadecimal digit, or -1 for any other character.
[[nodiscard]] int hex_value(int one) {
    if (one >= '0' && one <= '9') return one - '0';
    if (one >= 'a' && one <= 'f') return one - 'a' + 10;
    if (one >= 'A' && one <= 'F') return one - 'A' + 10;

    return -1;
}

// Whether the character belongs to a number: a digit, a sign or the decimal point.
[[nodiscard]] bool numeric(int one) { return (one >= '0' && one <= '9') || one == '+' || one == '-' || one == '.'; }

// The text with every language mark taken out: in a UTF-16 text string a code ESC (U+001B) opens a
// language code and a second ESC closes it (ISO 32000-2, 7.9.2.2.1); the code is not text.
[[nodiscard]] std::string unmarked(std::string text) {
    std::string made;
    bool inside = false;

    for (const char one : text) {
        if (one == '\x1B') {
            inside = !inside;

            continue;
        }

        if (!inside) made.push_back(one);
    }

    return made;
}

}

// The value under the key, or null when the dictionary has none; with the key twice (a broken
// file), the first one counts.
const Object* Dict::find(std::string_view key) const {
    for (const Entry& one : entries)
        if (one.key == key) return &one.value;

    return nullptr;
}

// The dictionary of a dictionary or of a stream, or null.
const Dict* dict_of(const Object& one) {
    if (const auto* dict = std::get_if<Dict>(&one.value)) return dict;

    if (const auto* stream = std::get_if<Stream>(&one.value)) return &stream->dict;

    return nullptr;
}

// The array, or null.
const Array* array_of(const Object& one) { return std::get_if<Array>(&one.value); }

// The stream, or null.
const Stream* stream_of(const Object& one) { return std::get_if<Stream>(&one.value); }

// The bytes of a string, or null.
const std::string* string_of(const Object& one) { return std::get_if<std::string>(&one.value); }

// The name, or null.
const Name* name_of(const Object& one) { return std::get_if<Name>(&one.value); }

// The reference, or nothing.
std::optional<Ref> ref_of(const Object& one) {
    if (const auto* ref = std::get_if<Ref>(&one.value)) return *ref;

    return std::nullopt;
}

// The integer, or nothing; a real number is not an integer.
std::optional<std::int64_t> integer_of(const Object& one) {
    if (const auto* integer = std::get_if<std::int64_t>(&one.value)) return *integer;

    return std::nullopt;
}

// The number, integer or real, or nothing.
std::optional<double> number_of(const Object& one) {
    if (const auto* integer = std::get_if<std::int64_t>(&one.value)) return static_cast<double>(*integer);

    if (const auto* real = std::get_if<double>(&one.value)) return *real;

    return std::nullopt;
}

// Whether the object is there and is the name given.
bool named(const Object* one, std::string_view text) {
    if (one == nullptr) return false;

    const Name* name = name_of(*one);

    return name != nullptr && name->text == text;
}

// Whether the character is white space (ISO 32000-2, Table 1); -1, the end of the text, is not.
bool spacing(int one) { return one == 0 || one == 9 || one == 10 || one == 12 || one == 13 || one == 32; }

// Whether the character is a delimiter (ISO 32000-2, Table 2).
bool delimiting(int one) {
    return one == '(' || one == ')' || one == '<' || one == '>' || one == '[' || one == ']' || one == '{' || one == '}' ||
           one == '/' || one == '%';
}

// A text string as UTF-8 (ISO 32000-2, 7.9.2.2): UTF-16BE after its byte order mark, UTF-8 after
// its own (PDF 2.0), and otherwise PDFDocEncoding. A UTF-16 string in little-endian order with its
// mark is not allowed, but some writers make it, and the mark says plainly what it is. An undefined
// code of PDFDocEncoding becomes U+FFFD, a zero ends the text.
std::string text_of(std::string_view raw) {
    const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(raw.data()), raw.size()};

    if (raw.starts_with("\xFE\xFF")) return unmarked(utf16(bytes.subspan(2), true));

    if (raw.starts_with("\xFF\xFE")) return unmarked(utf16(bytes.subspan(2), false));

    if (raw.starts_with("\xEF\xBB\xBF")) return utf8(bytes);

    std::string made;

    made.reserve(raw.size());

    for (const char one : raw) {
        const auto code = static_cast<unsigned char>(one);

        if (code == 0) break;

        char32_t mark = code;

        if (code >= 0x18 && code <= 0x1F) mark = low_marks[code - 0x18];
        else if (code >= 0x80 && code <= 0xA0) mark = high_marks[code - 0x80];
        else if (code == 0x7F || code == 0xAD) mark = 0;

        text::encoded(made, mark == 0 ? char32_t{0xFFFD} : mark);
    }

    return made;
}

// The character `ahead` places after the cursor, or -1 past the end of the text.
int Syntax::peek(std::size_t ahead) const {
    if (at_ + ahead >= text_.size()) return -1;

    return std::to_integer<int>(text_[at_ + ahead]);
}

// Skips white space and comments; a comment runs to the end of its line.
void Syntax::blank() {
    for (;;) {
        const int one = peek();

        if (spacing(one)) {
            ++at_;

            continue;
        }

        if (one != '%') return;

        while (peek() != -1 && peek() != '\n' && peek() != '\r') ++at_;
    }
}

// The run of regular characters at the cursor: a keyword, or the text of a number.
std::string_view Syntax::word() {
    const std::size_t from = at_;

    while (peek() != -1 && !spacing(peek()) && !delimiting(peek())) ++at_;

    if (peek() == -1) cut_ = true;

    return {reinterpret_cast<const char*>(text_.data()) + from, at_ - from};
}

// Takes the keyword when it stands next, after white space, as a whole word; otherwise the cursor
// stays where it was. When the text ends inside what could still become the keyword, `cut` says
// so: the answer depends on bytes that were not read.
bool Syntax::keyword(std::string_view wanted) {
    const std::size_t was = at_;
    const bool cut_before = cut_;

    blank();

    const std::string_view got = word();

    if (got == wanted) return true;

    const bool ended = peek() == -1;

    at_ = was;
    cut_ = cut_before || (ended && wanted.starts_with(got));

    return false;
}

// Takes the end of the line right after a keyword, as the one after "stream" has to be: CR LF or
// LF; a lone CR is not allowed, but some writers make it. Says whether there was one. When the
// text ends after a CR, or where the end of line should be, `cut` says so: an LF may follow.
bool Syntax::eol() {
    if (peek() == '\r') {
        ++at_;

        if (peek() == '\n') ++at_;
        else if (peek() == -1) cut_ = true;

        return true;
    }

    if (peek() == '\n') {
        ++at_;

        return true;
    }

    if (peek() == -1) cut_ = true;

    return false;
}

// Takes an integer when one stands next, after white space; otherwise the cursor stays.
std::optional<std::int64_t> Syntax::integer() {
    const std::size_t was = at_;

    blank();

    if (!numeric(peek())) {
        if (peek() == -1) cut_ = true;

        at_ = was;

        return std::nullopt;
    }

    const std::optional<Object> made = numeral();
    const std::optional<std::int64_t> whole = made ? integer_of(*made) : std::nullopt;

    if (whole) return whole;

    at_ = was;

    return std::nullopt;
}

// A number at the cursor. Writers make numbers the standard does not allow ("--3", "1.2.3", a
// lone "-"), and readers take them as best they can: repeated signs count as one, everything after
// a second decimal point is dropped, and a sign without digits is zero. An integer too large for
// 64 bits becomes a real number, and a real too large for a double is infinite.
std::optional<Object> Syntax::numeral() {
    const std::size_t from = at_;

    while (numeric(peek())) ++at_;

    if (peek() == -1) cut_ = true;

    std::string_view raw{reinterpret_cast<const char*>(text_.data()) + from, at_ - from};

    bool negative = false;

    while (!raw.empty() && (raw.front() == '-' || raw.front() == '+')) {
        negative = negative || raw.front() == '-';
        raw.remove_prefix(1);
    }

    std::string clean{raw.substr(0, raw.find_first_of("+-"))};

    const std::size_t point = clean.find('.');

    if (point != std::string::npos) clean = clean.substr(0, clean.find('.', point + 1));

    if (clean.empty() || clean == ".") return Object{std::int64_t{0}};

    // The sign goes with the digits, so that the smallest 64-bit integer is one.
    if (negative) clean.insert(clean.begin(), '-');

    if (point == std::string::npos) {
        std::int64_t whole = 0;
        const auto [end, failed] = std::from_chars(clean.data(), clean.data() + clean.size(), whole);

        if (failed == std::errc{} && end == clean.data() + clean.size()) return Object{whole};
    }

    double real = 0;
    const auto [end, failed] = std::from_chars(clean.data(), clean.data() + clean.size(), real);

    // Out of range is too large (digits other than zeros before the point), and then infinite,
    // which every size check refuses, or too small, and then zero.
    if (failed == std::errc::result_out_of_range) {
        const std::string_view whole_part = std::string_view{clean}.substr(0, clean.find('.'));
        const bool large = whole_part.find_first_of("123456789") != std::string_view::npos;

        real = large ? std::numeric_limits<double>::infinity() : 0.0;

        return Object{negative ? -real : real};
    }

    if (failed != std::errc{}) return Object{std::int64_t{0}};

    return Object{real};
}

// A name at the cursor, which stands on the slash; #xx is the byte xx (ISO 32000-2, 7.3.5), and a
// # without two hexadecimal digits after it stays itself, as readers take it.
std::optional<Name> Syntax::name() {
    ++at_;

    Name made;

    while (peek() != -1 && !spacing(peek()) && !delimiting(peek())) {
        int one = peek();

        if (one == '#' && hex_value(peek(1)) >= 0 && hex_value(peek(2)) >= 0) {
            one = hex_value(peek(1)) * 16 + hex_value(peek(2));
            at_ += 2;
        }

        ++at_;

        if (made.text.size() >= longest_name) return std::nullopt;

        made.text.push_back(static_cast<char>(one));
    }

    if (peek() == -1) cut_ = true;

    return made;
}

// A literal string at the cursor, which stands on the opening parenthesis (ISO 32000-2, 7.3.4.2):
// balanced parentheses, the escapes, three octal digits at most, a backslash before the end of a
// line joins the lines, and an end of line in the string is LF whatever it was. Past `most_text`
// the bytes are read but not kept.
std::optional<std::string> Syntax::literal() {
    ++at_;

    std::string made;
    std::size_t open = 1;

    const auto put = [&made](char one) {
        if (made.size() < most_text) made.push_back(one);
    };

    for (;;) {
        const int one = peek();

        if (one == -1) {
            cut_ = true;

            return std::nullopt;
        }

        ++at_;

        if (one == '(') {
            ++open;
            put('(');
        } else if (one == ')') {
            if (--open == 0) return made;

            put(')');
        } else if (one == '\r') {
            if (peek() == '\n') ++at_;

            put('\n');
        } else if (one == '\\') {
            const int next = peek();

            if (next == -1) continue;

            ++at_;

            switch (next) {
                case 'n': put('\n'); break;
                case 'r': put('\r'); break;
                case 't': put('\t'); break;
                case 'b': put('\b'); break;
                case 'f': put('\f'); break;
                case '\r':
                    if (peek() == '\n') ++at_;

                    break;
                case '\n': break;
                default:
                    if (next >= '0' && next <= '7') {
                        int code = next - '0';

                        for (int more = 0; more < 2 && peek() >= '0' && peek() <= '7'; ++more) {
                            code = code * 8 + (peek() - '0');
                            ++at_;
                        }

                        put(static_cast<char>(code & 0xFF));
                    } else {
                        put(static_cast<char>(next));
                    }
            }
        } else {
            put(static_cast<char>(one));
        }
    }
}

// A hexadecimal string at the cursor, which stands on "<" (ISO 32000-2, 7.3.4.3): white space is
// ignored, an odd last digit has a zero after it, and any other character means it was no string.
std::optional<std::string> Syntax::hexadecimal() {
    ++at_;

    std::string made;
    int high = -1;

    for (;;) {
        const int one = peek();

        if (one == -1) {
            cut_ = true;

            return std::nullopt;
        }

        ++at_;

        if (one == '>') break;

        if (spacing(one)) continue;

        const int value = hex_value(one);

        if (value < 0) return std::nullopt;

        if (high < 0) {
            high = value;
        } else {
            if (made.size() < most_text) made.push_back(static_cast<char>(high * 16 + value));

            high = -1;
        }
    }

    if (high >= 0 && made.size() < most_text) made.push_back(static_cast<char>(high * 16));

    return made;
}

// The next object, after white space, or nothing when none stands there: a keyword that is no
// value ("endobj"), a stray delimiter, damage, or the end of the text (then `cut` is set).
std::optional<Object> Syntax::object() {
    items_ = 0;

    return nested(0);
}

// An object at the given depth of containers. An integer followed by another and by R is a
// reference; otherwise the cursor goes back to just after the first integer, and `cut` stays set
// when the text ended during the look ahead, because the reference may lie past it.
std::optional<Object> Syntax::nested(std::size_t depth) {
    if (depth > deepest || ++items_ > most_items) return std::nullopt;

    blank();

    const int one = peek();

    if (one == -1) {
        cut_ = true;

        return std::nullopt;
    }

    if (one == '/') {
        std::optional<Name> made = name();

        if (!made) return std::nullopt;

        return Object{std::move(*made)};
    }

    if (one == '(') {
        std::optional<std::string> made = literal();

        if (!made) return std::nullopt;

        return Object{std::move(*made)};
    }

    if (one == '<' && peek(1) != '<') {
        std::optional<std::string> made = hexadecimal();

        if (!made) return std::nullopt;

        return Object{std::move(*made)};
    }

    if (one == '<') {
        at_ += 2;

        Dict made;

        for (;;) {
            blank();

            if (peek() == '>' && peek(1) == '>') {
                at_ += 2;

                return Object{std::move(made)};
            }

            if (peek() == -1 || (peek() == '>' && peek(1) == -1)) {
                cut_ = true;

                return std::nullopt;
            }

            if (peek() != '/') return std::nullopt;

            std::optional<Name> key = name();

            if (!key) return std::nullopt;

            blank();

            if (peek() == '>' && peek(1) == '>') {
                made.entries.push_back(Entry{std::move(key->text), Object{Null{}}});

                continue;
            }

            if (peek() == '>' && peek(1) == -1) {
                cut_ = true;

                return std::nullopt;
            }

            std::optional<Object> value = nested(depth + 1);

            if (!value) return std::nullopt;

            made.entries.push_back(Entry{std::move(key->text), std::move(*value)});
        }
    }

    if (one == '[') {
        ++at_;

        Array made;

        for (;;) {
            blank();

            if (peek() == ']') {
                ++at_;

                return Object{std::move(made)};
            }

            std::optional<Object> value = nested(depth + 1);

            if (!value) return std::nullopt;

            made.push_back(std::move(*value));
        }
    }

    if (numeric(one)) {
        std::optional<Object> made = numeral();

        const std::optional<std::int64_t> number = made ? integer_of(*made) : std::nullopt;

        if (number && *number >= 0 && *number <= 0xFFFFFFFF) {
            const std::size_t after = at_;
            const std::optional<std::int64_t> generation = integer();

            if (generation && *generation >= 0 && *generation <= 0xFFFFFFFF && keyword("R"))
                return Object{Ref{static_cast<std::uint32_t>(*number), static_cast<std::uint32_t>(*generation)}};

            at_ = after;
        }

        return made;
    }

    if (delimiting(one)) return std::nullopt;

    const std::size_t was = at_;
    const std::string_view text = word();

    if (text == "true") return Object{true};
    if (text == "false") return Object{false};
    if (text == "null") return Object{Null{}};

    at_ = was;

    return std::nullopt;
}

}
