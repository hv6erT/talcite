#include "reading.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>

#include "tal/text.hpp"
#include "tal/trouble.hpp"

namespace tal::reading {

namespace {

constexpr int deepest = 64;

// A reader of one JSON text, strict: no comments, no trailing commas, no repeated names, nothing
// after the value. It knows where it stands, so every refusal names a line and a column.
class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}

    Value whole();

private:
    Value value(int depth);

    Value object(int depth);

    Value array(int depth);

    std::string string();

    Value number();

    Value word();

    void spaced();

    void expected(char one);

    std::uint32_t hexed();

    Value here() const;

    [[noreturn]] void failed(std::string_view why) const;

    std::string_view text_;
    std::size_t at_ = 0;
    int line_ = 1;
    std::size_t start_ = 0;
};

// The value of the whole text, which must hold one value and nothing after it but space.
Value Reader::whole() {
    if (!text::valid(text_)) failed("the text is not UTF-8");

    spaced();

    Value made = value(0);

    spaced();

    if (at_ < text_.size()) failed("something follows the value");

    return made;
}

// A value of any kind, at most `deepest` arrays and objects down.
Value Reader::value(int depth) {
    if (at_ >= text_.size()) failed("a value is missing");

    if (depth >= deepest) failed("the values nest too deep");

    const char one = text_[at_];

    if (one == '{') return object(depth + 1);

    if (one == '[') return array(depth + 1);

    if (one == '"') {
        Value made = here();

        made.kind = Kind::string;
        made.text = string();

        return made;
    }

    if (one == '-' || (one >= '0' && one <= '9')) return number();

    return word();
}

// An object: names in quotes, each once.
Value Reader::object(int depth) {
    Value made = here();

    made.kind = Kind::object;

    ++at_;
    spaced();

    if (at_ < text_.size() && text_[at_] == '}') {
        ++at_;

        return made;
    }

    while (true) {
        if (at_ >= text_.size() || text_[at_] != '"') failed("a name in quotes is expected");

        const Value named = here();
        std::string name = string();

        if (std::find(made.names.begin(), made.names.end(), name) != made.names.end())
            refused(named, "the name \"" + name + "\" is repeated");

        spaced();
        expected(':');
        spaced();

        made.items.push_back(value(depth));
        made.names.push_back(std::move(name));

        spaced();

        if (at_ < text_.size() && text_[at_] == ',') {
            ++at_;
            spaced();

            continue;
        }

        expected('}');

        return made;
    }
}

// An array of values.
Value Reader::array(int depth) {
    Value made = here();

    made.kind = Kind::array;

    ++at_;
    spaced();

    if (at_ < text_.size() && text_[at_] == ']') {
        ++at_;

        return made;
    }

    while (true) {
        made.items.push_back(value(depth));

        spaced();

        if (at_ < text_.size() && text_[at_] == ',') {
            ++at_;
            spaced();

            continue;
        }

        expected(']');

        return made;
    }
}

// A string in quotes with its escapes undone. A \u escape of half a surrogate pair needs the
// other half right after it.
std::string Reader::string() {
    std::string made;

    ++at_;

    while (true) {
        if (at_ >= text_.size()) failed("the string has no end");

        const auto one = static_cast<unsigned char>(text_[at_]);

        if (one == '"') {
            ++at_;

            return made;
        }

        if (one < 0x20) failed("a control character stands in a string");

        if (one != '\\') {
            made.push_back(static_cast<char>(one));
            ++at_;

            continue;
        }

        if (at_ + 1 >= text_.size()) failed("the string has no end");

        const char escaped = text_[at_ + 1];

        at_ += 2;

        switch (escaped) {
            case '"': made.push_back('"'); break;
            case '\\': made.push_back('\\'); break;
            case '/': made.push_back('/'); break;
            case 'b': made.push_back('\b'); break;
            case 'f': made.push_back('\f'); break;
            case 'n': made.push_back('\n'); break;
            case 'r': made.push_back('\r'); break;
            case 't': made.push_back('\t'); break;
            case 'u': {
                std::uint32_t point = hexed();

                if (point >= 0xDC00 && point <= 0xDFFF) failed("a low surrogate stands alone");

                if (point >= 0xD800 && point <= 0xDBFF) {
                    if (text_.substr(at_, 2) != "\\u") failed("a high surrogate stands alone");

                    at_ += 2;

                    const std::uint32_t low = hexed();

                    if (low < 0xDC00 || low > 0xDFFF) failed("a high surrogate stands alone");

                    point = 0x10000 + ((point - 0xD800) << 10) + (low - 0xDC00);
                }

                if (point == 0) failed("a string holds a zero character");

                text::encoded(made, static_cast<char32_t>(point));

                break;
            }
            default: failed("an unknown escape stands in a string");
        }
    }
}

// The four hexadecimal digits of a \u escape.
std::uint32_t Reader::hexed() {
    if (at_ + 4 > text_.size()) failed("a \\u escape is cut short");

    std::uint32_t made = 0;
    const auto [end, trouble] = std::from_chars(text_.data() + at_, text_.data() + at_ + 4, made, 16);

    if (trouble != std::errc{} || end != text_.data() + at_ + 4) failed("a \\u escape needs four hexadecimal digits");

    at_ += 4;

    return made;
}

// Ten to the power `many`: exact up to 10^22, which a double holds exactly, and the nearest
// double beyond.
double tenfold(long many) {
    if (many > 22) return std::pow(10.0, static_cast<double>(many));

    double made = 1;

    for (long at = 0; at < many; ++at) made *= 10;

    return made;
}

// A number as JSON writes it: an optional minus, no leading zeros, an optional fraction and
// exponent. The first 18 significant digits are kept, which is exact for every whole number and
// short decimal a sheet holds; it is read without std::from_chars, which not every standard
// library has for floating point.
Value Reader::number() {
    Value made = here();
    const bool negative = text_[at_] == '-';
    std::uint64_t mantissa = 0;
    int significant = 0;
    long scale = 0;
    const auto digits = [&](bool fraction) {
        const std::size_t begun = at_;

        while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
            const auto digit = static_cast<std::uint64_t>(text_[at_] - '0');

            if (significant < 18) {
                if (mantissa != 0 || digit != 0) ++significant;

                mantissa = mantissa * 10 + digit;

                if (fraction) --scale;
            } else if (!fraction) {
                ++scale;
            }

            ++at_;
        }

        return at_ - begun;
    };

    if (negative) ++at_;

    if (at_ < text_.size() && text_[at_] == '0') {
        ++at_;
    } else if (digits(false) == 0) {
        failed("a number needs a digit");
    }

    if (at_ < text_.size() && text_[at_] == '.') {
        ++at_;

        if (digits(true) == 0) failed("a fraction needs a digit");
    }

    if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
        ++at_;

        const bool down = at_ < text_.size() && text_[at_] == '-';

        if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) ++at_;

        long exponent = 0;
        const std::size_t begun = at_;

        for (; at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9'; ++at_)
            exponent = std::min(100000L, exponent * 10 + (text_[at_] - '0'));

        if (at_ == begun) failed("an exponent needs a digit");

        scale += down ? -exponent : exponent;
    }

    const auto whole = static_cast<double>(mantissa);
    const double value = mantissa == 0 ? 0 : scale >= 0 ? whole * tenfold(scale) : whole / tenfold(-scale);

    if (!std::isfinite(value)) refused(made, "the number is out of range");

    made.kind = Kind::number;
    made.number = negative ? -value : value;

    return made;
}

// One of the words true, false and null.
Value Reader::word() {
    Value made = here();

    for (const std::string_view one : {"true", "false", "null"}) {
        if (text_.substr(at_, one.size()) != one) continue;

        at_ += one.size();

        made.kind = one == "null" ? Kind::null : Kind::boolean;
        made.boolean = one == "true";

        return made;
    }

    failed("a value is expected");
}

// Skips space, counting lines.
void Reader::spaced() {
    while (at_ < text_.size()) {
        const char one = text_[at_];

        if (one == '\n') {
            ++line_;
            start_ = at_ + 1;
        } else if (one != ' ' && one != '\t' && one != '\r') {
            return;
        }

        ++at_;
    }
}

// Takes the character `one`, which must stand here.
void Reader::expected(char one) {
    if (at_ >= text_.size() || text_[at_] != one) failed(std::string{"'"} + one + "' is expected");

    ++at_;
}

// An empty value marked with the place the reader stands at.
Value Reader::here() const {
    Value made;

    made.line = line_;
    made.column = static_cast<int>(at_ - start_) + 1;

    return made;
}

// Refuses the text at the place the reader stands at.
void Reader::failed(std::string_view why) const { refused(here(), why); }

}

// The member `name` of an object, or nothing when the value is not an object or has no such
// member.
const Value* Value::member(std::string_view name) const {
    if (kind != Kind::object) return nullptr;

    for (std::size_t at = 0; at < names.size(); ++at)
        if (names[at] == name) return &items[at];

    return nullptr;
}

// The value of a JSON text; a text that is not JSON throws `Trouble` of kind `malformed` with the
// line and column of the mistake.
Value parsed(std::string_view json) { return Reader{json}.whole(); }

// Throws `Trouble` of kind `malformed` for a mistake at the place of `where`.
void refused(const Value& where, std::string_view why) {
    throw Trouble{::tal::Kind::malformed,
                  "line " + std::to_string(where.line) + ", column " + std::to_string(where.column) + ": " + std::string{why}};
}

}
