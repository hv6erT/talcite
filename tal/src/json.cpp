#include "tal/json.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "tal/text.hpp"
#include "tal/trouble.hpp"

// JSON as RFC 8259 writes it, and nothing more: no comments, no commas at the end, no NaN, one
// value in a text. The reader is meant to meet hostile text. Every text it is given is bounded
// before a byte is looked at (its size), and while it is read (how deep values nest, how many there
// are in all, how many digits a number has), so that neither the stack nor the memory grows with
// the attacker's wish; repeated names are found by sorting, which no choice of names slows down, as
// it could a hash. A refusal says where and why, never what stood there.
namespace tal::json {

namespace {

// The byte order mark, which Notepad puts at the start of a file.
constexpr std::string_view marked = "\xEF\xBB\xBF";

// The kind of a value held as the alternative `index` of the variant.
constexpr Kind kinds[] = {Kind::null, Kind::boolean, Kind::integer, Kind::number, Kind::string, Kind::array, Kind::object};

// Throws `Trouble` of kind `malformed` for a mistake at the line `line` and column `column`.
[[noreturn]] void refused(int line, int column, std::string_view why) {
    throw Trouble{::tal::Kind::malformed, "line " + std::to_string(line) + ", column " + std::to_string(column) + ": " + std::string{why}};
}

// The double written in `digits`, which the reader made sure is a JSON number: exact to the nearest
// double where the standard library can read floating point by itself, otherwise through a stream
// in the classic locale, which reads the point whatever the program's locale.
double converted(std::string_view digits) {
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    double made = 0;
    const auto [end, trouble] = std::from_chars(digits.data(), digits.data() + digits.size(), made);

    if (trouble == std::errc::result_out_of_range) return std::numeric_limits<double>::infinity();

    (void)end;

    return made;
#else
    std::istringstream stream{std::string{digits}};
    double made = 0;

    stream.imbue(std::locale::classic());
    stream >> made;

    return stream.fail() ? std::numeric_limits<double>::infinity() : made;
#endif
}

// The shortest text that reads back as `one`, a finite double, with a point or an exponent so that
// it reads back as a number and not as an integer.
std::string spelled(double one) {
    std::string made;

#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    char room[64];
    const auto [end, trouble] = std::to_chars(room, room + sizeof room, one);

    (void)trouble;

    made.assign(room, end);
#else
    std::ostringstream stream;

    stream.imbue(std::locale::classic());
    stream.precision(17);
    stream << one;

    made = stream.str();
#endif

    if (made.find_first_of(".eE") == std::string::npos) made += ".0";

    return made;
}

// Appends `said`, a string, in quotes: a quote, a backslash and the control characters escaped, the
// rest as it is. A string that is not UTF-8 throws std::invalid_argument, since JSON cannot hold
// it.
void quoted(std::string& into, const std::string& said) {
    if (!text::valid(said)) throw std::invalid_argument{"tal::json::written: a string is not UTF-8"};

    into += '"';

    for (const char one : said) {
        switch (one) {
            case '"': into += "\\\""; break;
            case '\\': into += "\\\\"; break;
            case '\b': into += "\\b"; break;
            case '\f': into += "\\f"; break;
            case '\n': into += "\\n"; break;
            case '\r': into += "\\r"; break;
            case '\t': into += "\\t"; break;
            default:
                if (static_cast<unsigned char>(one) < 0x20) {
                    constexpr std::string_view hex = "0123456789abcdef";

                    into += "\\u00";
                    into += hex[static_cast<unsigned char>(one) >> 4];
                    into += hex[static_cast<unsigned char>(one) & 0xF];
                } else {
                    into += one;
                }
        }
    }

    into += '"';
}

// Appends the line break and the spaces before an item `depth` levels deep, when indented.
void broken(std::string& into, std::optional<int> indent, int depth) {
    if (!indent) return;

    into += '\n';
    into.append(static_cast<std::size_t>(depth) * 2, ' ');
}

// Appends `one` written as JSON; `indent` is how deep it stands when indented, or none for the
// compact form.
void wrote(std::string& into, const Value& one, std::optional<int> indent) {
    const std::optional<int> deeper = indent ? std::optional<int>{*indent + 1} : std::nullopt;

    switch (one.kind()) {
        case Kind::null: into += "null"; return;
        case Kind::boolean: into += one.boolean() ? "true" : "false"; return;
        case Kind::integer: into += std::to_string(one.integer()); return;
        case Kind::number:
            if (!std::isfinite(one.number())) throw std::invalid_argument{"tal::json::written: a number is not finite"};

            into += spelled(one.number());
            return;
        case Kind::string: quoted(into, one.string()); return;
        case Kind::array:
            into += '[';

            for (std::size_t at = 0; at < one.size(); ++at) {
                if (at > 0) into += ',';

                broken(into, deeper, deeper.value_or(0));
                wrote(into, one.at(at), deeper);
            }

            if (one.size() > 0) broken(into, indent, indent.value_or(0));

            into += ']';
            return;
        case Kind::object:
            into += '{';

            for (std::size_t at = 0; at < one.size(); ++at) {
                if (at > 0) into += ',';

                broken(into, deeper, deeper.value_or(0));
                quoted(into, one.names()[at]);
                into += indent ? ": " : ":";
                wrote(into, one.at(at), deeper);
            }

            if (one.size() > 0) broken(into, indent, indent.value_or(0));

            into += '}';
            return;
    }
}

}

// A reader of one JSON text within its limits. It knows where it stands: the line, where the line
// starts, and how many characters of the line lie before the place it counted up to, so that the
// column of a value costs only the characters since the last one asked.
class Reader {
public:
    Reader(std::string_view text, Limits limits) : text_(text), limits_(limits) {}

    // The value of the whole text, which must hold one value and nothing after it but space.
    Value whole() {
        if (text_.size() > limits_.bytes) failed("the text is longer than the limit");

        if (text_.starts_with(marked)) start_ = at_ = counted_at_ = marked.size();

        if (!text::valid(text_)) failed("the text is not UTF-8");

        spaced();

        Value made = value(0);

        spaced();

        if (at_ < text_.size()) failed("something follows the value");

        return made;
    }

private:
    // A value of any kind, `depth` arrays and objects down, counted against the limit of values.
    Value value(int depth) {
        if (at_ >= text_.size()) failed("a value is missing");

        if (++values_ > limits_.values) failed("the text holds more values than the limit");

        const char one = text_[at_];

        if (one == '{' || one == '[') {
            if (depth >= limits_.depth) failed("the values nest deeper than the limit");

            return one == '{' ? object(depth + 1) : array(depth + 1);
        }

        if (one == '"') {
            Value made = here();

            made.held_ = string();

            return made;
        }

        if (one == '-' || (one >= '0' && one <= '9')) return number();

        return word();
    }

    // An object: names in quotes, each once. The names are sorted when it ends, ties by their
    // order, so that a name repeated anywhere is refused at the first repetition in the text.
    Value object(int depth) {
        Value made = here();
        Value::Object held;

        ++at_;
        spaced();

        if (at_ < text_.size() && text_[at_] == '}') {
            ++at_;
            made.held_ = std::move(held);

            return made;
        }

        std::vector<std::pair<int, int>> places;

        while (true) {
            if (at_ >= text_.size() || text_[at_] != '"') failed("a name in quotes is expected");

            const Value named = here();

            places.emplace_back(named.line_, named.column_);
            held.names.push_back(string());
            spaced();
            expected(':');
            spaced();
            held.items.push_back(value(depth));
            spaced();

            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                spaced();

                continue;
            }

            expected('}');

            break;
        }

        std::vector<std::size_t> order(held.names.size());

        for (std::size_t at = 0; at < order.size(); ++at) order[at] = at;

        std::sort(order.begin(), order.end(), [&held](std::size_t one, std::size_t two) {
            return held.names[one] != held.names[two] ? held.names[one] < held.names[two] : one < two;
        });

        std::size_t first = order.size();

        for (std::size_t at = 1; at < order.size(); ++at)
            if (held.names[order[at]] == held.names[order[at - 1]]) first = std::min(first, order[at]);

        if (first < order.size()) refused(places[first].first, places[first].second, "a name is repeated");

        made.held_ = std::move(held);

        return made;
    }

    // An array of values.
    Value array(int depth) {
        Value made = here();
        Value::Array held;

        ++at_;
        spaced();

        if (at_ < text_.size() && text_[at_] == ']') {
            ++at_;
            made.held_ = std::move(held);

            return made;
        }

        while (true) {
            held.items.push_back(value(depth));
            spaced();

            if (at_ < text_.size() && text_[at_] == ',') {
                ++at_;
                spaced();

                continue;
            }

            expected(']');

            break;
        }

        made.held_ = std::move(held);

        return made;
    }

    // A string in quotes with its escapes undone. The text is UTF-8 already; a \u escape of half a
    // surrogate pair needs the other half right after it, and the zero character is refused.
    std::string string() {
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
                    at_ += 2;

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

                    continue;
                }
                default: failed("an unknown escape stands in a string");
            }

            at_ += 2;
        }
    }

    // The four hexadecimal digits of a \u escape.
    std::uint32_t hexed() {
        std::uint32_t made = 0;

        for (int step = 0; step < 4; ++step, ++at_) {
            if (at_ >= text_.size()) failed("a \\u escape is cut short");

            const char one = text_[at_];
            const int digit = one >= '0' && one <= '9'   ? one - '0'
                              : one >= 'a' && one <= 'f' ? one - 'a' + 10
                              : one >= 'A' && one <= 'F' ? one - 'A' + 10
                                                         : -1;

            if (digit < 0) failed("a \\u escape needs four hexadecimal digits");

            made = made * 16 + static_cast<std::uint32_t>(digit);
        }

        return made;
    }

    // A number as JSON writes it: an optional minus, no leading zeros, an optional fraction and
    // exponent, at most the limit of digits in all. A number that reads whole as 64 bits, without
    // fraction or exponent, is held exactly; any other is the nearest double, and one beyond the
    // doubles is refused.
    Value number() {
        Value made = here();
        const std::size_t from = at_;
        int digits = 0;

        const auto run = [this, &digits] {
            const std::size_t begun = at_;

            while (at_ < text_.size() && text_[at_] >= '0' && text_[at_] <= '9') {
                if (++digits > limits_.digits) failed("a number has more digits than the limit");

                ++at_;
            }

            return at_ > begun;
        };

        if (text_[at_] == '-') ++at_;

        if (at_ < text_.size() && text_[at_] == '0') {
            ++at_;
            ++digits;
        } else if (!run()) {
            failed("a number needs a digit");
        }

        if (at_ < text_.size() && text_[at_] == '.') {
            ++at_;

            if (!run()) failed("a fraction needs a digit");
        }

        if (at_ < text_.size() && (text_[at_] == 'e' || text_[at_] == 'E')) {
            ++at_;

            if (at_ < text_.size() && (text_[at_] == '+' || text_[at_] == '-')) ++at_;

            if (!run()) failed("an exponent needs a digit");
        }

        const std::string_view spelled = text_.substr(from, at_ - from);

        std::int64_t exact = 0;
        const auto [end, trouble] = std::from_chars(spelled.data(), spelled.data() + spelled.size(), exact);

        if (trouble == std::errc{} && end == spelled.data() + spelled.size()) {
            made.held_ = exact;

            return made;
        }

        const double near = converted(spelled);

        if (!std::isfinite(near)) refused(made.line_, made.column_, "the number is out of range");

        made.held_ = near;

        return made;
    }

    // One of the words true, false and null.
    Value word() {
        Value made = here();

        for (const std::string_view one : {"true", "false", "null"}) {
            if (text_.substr(at_, one.size()) != one) continue;

            at_ += one.size();

            if (one != "null") made.held_ = one == "true";

            return made;
        }

        failed("a value is expected");
    }

    // Skips space, counting lines.
    void spaced() {
        while (at_ < text_.size()) {
            const char one = text_[at_];

            if (one == '\n') {
                ++line_;
                start_ = at_ + 1;
                counted_at_ = start_;
                counted_ = 0;
            } else if (one != ' ' && one != '\t' && one != '\r') {
                return;
            }

            ++at_;
        }
    }

    // Takes the character `one`, which must stand here.
    void expected(char one) {
        if (at_ >= text_.size() || text_[at_] != one) failed(std::string{"'"} + one + "' is expected");

        ++at_;
    }

    // An empty value marked with the line and the column, in characters, the reader stands at.
    Value here() {
        for (; counted_at_ < at_ && counted_at_ < text_.size(); ++counted_at_)
            if ((static_cast<unsigned char>(text_[counted_at_]) & 0xC0) != 0x80) ++counted_;

        Value made;

        made.line_ = line_;
        made.column_ = counted_ + 1;

        return made;
    }

    // Refuses the text at the place the reader stands at.
    [[noreturn]] void failed(std::string_view why) {
        const Value where = here();

        refused(where.line_, where.column_, why);
    }

    std::string_view text_;
    Limits limits_;
    std::size_t at_ = 0;
    int line_ = 1;
    std::size_t start_ = 0;
    std::size_t counted_at_ = 0;
    int counted_ = 0;
    std::size_t values_ = 0;
};

// An empty array.
Value Value::array() {
    Value made;

    made.held_ = Array{};

    return made;
}

// An empty object.
Value Value::object() {
    Value made;

    made.held_ = Object{};

    return made;
}

// The kind of the value.
Kind Value::kind() const { return kinds[held_.index()]; }

// The value of a boolean; another kind throws std::logic_error.
bool Value::boolean() const {
    if (const bool* one = std::get_if<bool>(&held_)) return *one;

    throw std::logic_error{"tal::json::Value: not a boolean"};
}

// The value of an integer; another kind throws std::logic_error.
std::int64_t Value::integer() const {
    if (const std::int64_t* one = std::get_if<std::int64_t>(&held_)) return *one;

    throw std::logic_error{"tal::json::Value: not an integer"};
}

// The value of a number, an integer too; another kind throws std::logic_error.
double Value::number() const {
    if (const double* one = std::get_if<double>(&held_)) return *one;

    if (const std::int64_t* one = std::get_if<std::int64_t>(&held_)) return static_cast<double>(*one);

    throw std::logic_error{"tal::json::Value: not a number"};
}

// The text of a string; another kind throws std::logic_error.
const std::string& Value::string() const {
    if (const std::string* one = std::get_if<std::string>(&held_)) return *one;

    throw std::logic_error{"tal::json::Value: not a string"};
}

// How many items an array or members an object has; another kind throws std::logic_error.
std::size_t Value::size() const {
    if (const Array* one = std::get_if<Array>(&held_)) return one->items.size();

    if (const Object* one = std::get_if<Object>(&held_)) return one->items.size();

    throw std::logic_error{"tal::json::Value: not an array or an object"};
}

// The item `which` of an array, or the value of the member `which` of an object, whose name is the
// same in `names`; another kind throws std::logic_error, one past the last std::out_of_range.
const Value& Value::at(std::size_t which) const {
    if (const Array* one = std::get_if<Array>(&held_)) return one->items.at(which);

    if (const Object* one = std::get_if<Object>(&held_)) return one->items.at(which);

    throw std::logic_error{"tal::json::Value: not an array or an object"};
}

// Puts `one` at the end of an array; another kind throws std::logic_error.
void Value::push(Value one) {
    Array* held = std::get_if<Array>(&held_);

    if (held == nullptr) throw std::logic_error{"tal::json::Value: not an array"};

    held->items.push_back(std::move(one));
}

// The member `name` of an object, or none, also for a value that is not an object.
const Value* Value::find(std::string_view name) const {
    const Object* one = std::get_if<Object>(&held_);

    if (one == nullptr) return nullptr;

    const auto found = std::find(one->names.begin(), one->names.end(), name);

    return found == one->names.end() ? nullptr : &one->items[static_cast<std::size_t>(found - one->names.begin())];
}

// Sets the member `name` of an object to `one`: in its place when it is there, at the end
// otherwise, so that an object never holds a name twice. Another kind throws std::logic_error.
void Value::put(std::string name, Value one) {
    Object* held = std::get_if<Object>(&held_);

    if (held == nullptr) throw std::logic_error{"tal::json::Value: not an object"};

    const auto found = std::find(held->names.begin(), held->names.end(), name);

    if (found != held->names.end()) {
        held->items[static_cast<std::size_t>(found - held->names.begin())] = std::move(one);

        return;
    }

    held->names.push_back(std::move(name));
    held->items.push_back(std::move(one));
}

// The names of an object's members, in their order; another kind throws std::logic_error.
const std::vector<std::string>& Value::names() const {
    if (const Object* one = std::get_if<Object>(&held_)) return one->names;

    throw std::logic_error{"tal::json::Value: not an object"};
}

// The value of the JSON text `text` read within `limits`; a text that is not JSON or breaks a limit
// throws `Trouble` of kind `malformed` with the line and column, in characters, of the place.
Value parsed(std::string_view text, Limits limits) { return Reader{text, limits}.whole(); }

// `one` written as JSON: compact, or `indented` two spaces a level with a member's name and value
// apart. A string that is not UTF-8 or a number that is not finite throws std::invalid_argument.
std::string written(const Value& one, bool indented) {
    std::string made;

    wrote(made, one, indented ? std::optional<int>{0} : std::nullopt);

    return made;
}

}
