#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "tal/json.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

// JSON read strictly within limits and written back: what RFC 8259 accepts and what it does not,
// every limit just under and just over, numbers, strings, objects in order, and random documents
// and random damage.

namespace {

using tal::json::Kind;
using tal::json::Limits;
using tal::json::parsed;
using tal::json::Value;
using tal::json::written;

// Whether `what` throws an exception of type E.
template <typename E, typename F>
bool throws(F what) {
    try {
        what();
    } catch (const E&) {
        return true;
    }

    return false;
}

// Why `text` is refused within `limits`, or "read" when it is not.
std::string refusal(std::string_view text, Limits limits = {}) {
    try {
        (void)parsed(text, limits);
    } catch (const tal::Trouble& why) {
        return why.what();
    }

    return "read";
}

// `text` read and written back compactly.
std::string again(std::string_view text) { return written(parsed(text)); }

// A random value at most `depth` levels deep: every kind, strings with escapes and characters of
// every length, numbers of every size.
Value invented(std::mt19937& random, int depth) {
    const auto pick = [&random](unsigned much) { return static_cast<unsigned>(random() % much); };
    const unsigned kind = depth > 0 ? pick(7) : pick(5);

    switch (kind) {
        case 0: return Value{};
        case 1: return Value{pick(2) == 0};
        case 2: return Value{static_cast<std::int64_t>((static_cast<std::uint64_t>(random()) << 32) | random())};
        case 3: {
            const double made = std::ldexp(static_cast<double>(random()) / 4294967296.0, static_cast<int>(pick(400)) - 200);

            return Value{pick(2) == 0 ? made : -made};
        }
        case 4: {
            static const std::vector<std::string> pieces{"a", "\"", "\\", "/", "\n", "\t", "\x01", "\x1f", "\xc4\x85", "\xe6\x97\xa5", "\xf0\x9f\x98\x80", " "};
            std::string made;

            for (unsigned at = pick(8); at > 0; --at) made += pieces[pick(static_cast<unsigned>(pieces.size()))];

            return Value{made};
        }
        case 5: {
            Value made = Value::array();

            for (unsigned at = pick(5); at > 0; --at) made.push(invented(random, depth - 1));

            return made;
        }
        default: {
            Value made = Value::object();

            for (unsigned at = pick(5); at > 0; --at) made.put("k" + std::to_string(pick(6)), invented(random, depth - 1));

            return made;
        }
    }
}

}

// Every kind of value reads and writes back, compact and indented two spaces a level; an object
// keeps the order of its members.
TRIAL("JSON: values of every kind, read and written back") {
    const std::string text = R"({"b":[1,2.5,true,false,null,"x"],"a":{},"c":[],"d":{"e":-7}})";
    const Value one = parsed(text);

    SAME(written(one), text);
    SAME(written(one, true), std::string{"{\n  \"b\": [\n    1,\n    2.5,\n    true,\n    false,\n    null,\n    \"x\"\n  ],\n  \"a\": {},\n  \"c\": [],\n  \"d\": {\n    \"e\": -7\n  }\n}"});
    OUGHT(parsed(written(one, true)) == one);
    OUGHT(one.kind() == Kind::object);
    OUGHT(one.names() == (std::vector<std::string>{"b", "a", "c", "d"}));
    OUGHT(one.find("b")->at(0).kind() == Kind::integer);
    OUGHT(one.find("b")->at(1).kind() == Kind::number);
    OUGHT(one.find("b")->at(2).kind() == Kind::boolean);
    OUGHT(one.find("b")->at(4).kind() == Kind::null);
    OUGHT(one.find("b")->at(5).kind() == Kind::string);
    OUGHT(one.find("a")->kind() == Kind::object);
    OUGHT(one.find("c")->kind() == Kind::array);
    SAME(one.at(3).find("e")->integer(), std::int64_t{-7});
    OUGHT(one.find("z") == nullptr);
    SAME(again(" \t\r\n 7 \n"), std::string{"7"});
    SAME(again("\"a\""), std::string{"\"a\""});
    SAME(again("null"), std::string{"null"});
}

// What RFC 8259 accepts reads, top-level scalars and the empty name too.
TRIAL("JSON: what the grammar accepts") {
    const std::vector<std::pair<std::string_view, std::string_view>> cases{
        {"[]", "[]"},
        {"{}", "{}"},
        {"[\"\"]", "[\"\"]"},
        {"{\"\":0}", "{\"\":0}"},
        {"[-0]", "[0]"},
        {"[0]", "[0]"},
        {"[0e+1]", "[0.0]"},
        {"[1E22]", "[1e+22]"},
        {"[1.5e-3]", "[0.0015]"},
        {"[-1.25E+2]", "[-125.0]"},
        {"[123.456e78]", "[1.23456e+80]"},
        {"[\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"]", "[\"\\\"\\\\/\\b\\f\\n\\r\\t\"]"},
        {"[\"\\u0060\\u012a\\u12AB\"]", "[\"`\xc4\xaa\xe1\x8a\xab\"]"},
        {"[\"\\ud83d\\ude00\"]", "[\"\xf0\x9f\x98\x80\"]"},
        {"[\"\\u001f\"]", "[\"\\u001f\"]"},
        {"[\"\xe6\x97\xa5\"]", "[\"\xe6\x97\xa5\"]"},
        {"[[[[[]]]]]", "[[[[[]]]]]"},
        {"{\"a\":{\"b\":{\"c\":[1,{\"d\":null}]}}}", "{\"a\":{\"b\":{\"c\":[1,{\"d\":null}]}}}"},
        {"\xEF\xBB\xBF[1]", "[1]"},
        {"[1 , 2 ,\t3\r\n]", "[1,2,3]"},
        {"true", "true"},
        {"-9223372036854775808", "-9223372036854775808"},
    };

    for (const auto& [text, expected] : cases) SAME(again(text), std::string{expected});
}

// What RFC 8259 refuses throws, with no other exception: commas, quotes, numbers, words, escapes,
// broken UTF-8, comments, a second value, nothing at all.
TRIAL("JSON: what the grammar refuses") {
    const std::vector<std::string_view> cases{
        "",
        " ",
        "[",
        "]",
        "[1,]",
        "[,1]",
        "[1,,2]",
        "{\"a\":1,}",
        "{a:1}",
        "{'a':1}",
        "{\"a\" 1}",
        "{\"a\":}",
        "{\"a\"}",
        "{1:1}",
        "[01]",
        "[-01]",
        "[1.]",
        "[.1]",
        "[-]",
        "[+1]",
        "[1e]",
        "[1e+]",
        "[- 1]",
        "[0x1]",
        "[Infinity]",
        "[NaN]",
        "[TRUE]",
        "[tru]",
        "[nul]",
        "[true false]",
        "[\"\\x\"]",
        "[\"\\u12\"]",
        "[\"\\u12G4\"]",
        "[\"\\ud800\"]",
        "[\"\\udc00\"]",
        "[\"\\ud800\\u0041\"]",
        "[\"\\u0000\"]",
        "[\"\t\"]",
        "[\"a",
        "[\"a\\",
        "[1]x",
        "[1] [2]",
        "/*c*/[]",
        "[]//",
        "[\xc0\xaf]",
        "[\"\xed\xa0\x80\"]",
        "[\"\xf5\x80\x80\x80\"]",
        "[\"\xe6\x97\"]",
        "\xEF\xBB\xBF\xEF\xBB\xBF[]",
        "[1e400]",
        "[-1e400]",
        "[1e-400]",
    };

    for (const std::string_view text : cases) OUGHT(refusal(text) != "read");

    SAME(refusal("{\n  \"a\": [1,]\n}"), std::string{"line 2, column 11: a value is expected"});
    SAME(refusal("[\"\xc4\x85\xc4\x85\", x]"), std::string{"line 1, column 8: a value is expected"});
    SAME(refusal("\xEF\xBB\xBF[x]"), std::string{"line 1, column 2: a value is expected"});
    SAME(refusal("[\r\n1,\r\n?]"), std::string{"line 3, column 1: a value is expected"});
    SAME(refusal("{\"secret\": 1, \"secret\": 2}"), std::string{"line 1, column 15: a name is repeated"});
    SAME(refusal("[\"\xff\"]"), std::string{"line 1, column 1: the text is not UTF-8"});
    SAME(refusal("{1:1}"), std::string{"line 1, column 2: a name in quotes is expected"});
    SAME(refusal("[\"a\\"), std::string{"line 1, column 4: the string has no end"});
    SAME(refusal("[\"\\u12"), std::string{"line 1, column 7: a \\u escape is cut short"});
    SAME(refusal("[\"\x1f\"]"), std::string{"line 1, column 3: a control character stands in a string"});
    SAME(refusal("[\"\x7f\"]"), std::string{"read"});
    SAME(refusal("[\"\\ud800\\ue000\"]"), std::string{"line 1, column 15: a high surrogate stands alone"});
    SAME(refusal("[\"\\ud800\\udbff\"]"), std::string{"line 1, column 15: a high surrogate stands alone"});

    std::string names = "{";

    for (int one = 0; one < 60; ++one) names += (one > 0 ? ",\"" : "\"") + std::string{one % 20 == 7 ? "x" : "n" + std::to_string(one)} + "\":0";

    names += "}";

    SAME(refusal(names), "line 1, column " + std::to_string(names.find("\"x\"", names.find("\"x\"") + 1) + 1) + ": a name is repeated");
    SAME(refusal("{\"b\":0,\"a\":0,\"b\":0,\"a\":0}"), std::string{"line 1, column 14: a name is repeated"});
}

// Each limit holds a text right at it and refuses one a step over it, before reading further.
TRIAL("JSON: the limits, just under and just over") {
    Limits bytes;

    bytes.bytes = 9;

    SAME(refusal("[1,2,3,4]", bytes), std::string{"read"});
    SAME(refusal("[1,2,3,45]", bytes), std::string{"line 1, column 1: the text is longer than the limit"});

    Limits deep;

    deep.depth = 3;

    SAME(refusal("[[{\"a\":1}]]", deep), std::string{"read"});
    SAME(refusal("[[{\"a\":[]}]]", deep), std::string{"line 1, column 8: the values nest deeper than the limit"});
    SAME(refusal(std::string(64, '[') + std::string(64, ']')), std::string{"read"});
    SAME(refusal(std::string(65, '[') + std::string(65, ']')), std::string{"line 1, column 65: the values nest deeper than the limit"});

    Limits many;

    many.values = 5;

    SAME(refusal("[1,2,3,4]", many), std::string{"read"});
    SAME(refusal("{\"a\":[1,2,3]}", many), std::string{"read"});
    SAME(refusal("[1,2,3,4,5]", many), std::string{"line 1, column 10: the text holds more values than the limit"});

    Limits digits;

    digits.digits = 5;

    SAME(refusal("[12345, -1.2345, 1.23e45, 0.0001]", digits), std::string{"read"});
    SAME(refusal("[123456]", digits), std::string{"line 1, column 7: a number has more digits than the limit"});
    SAME(refusal("[1.23e456]", digits), std::string{"line 1, column 9: a number has more digits than the limit"});
    SAME(refusal("[0.00001]", digits), std::string{"line 1, column 8: a number has more digits than the limit"});
    SAME(refusal("[0." + std::string(399, '1') + "]"), std::string{"read"});
    SAME(refusal("[0." + std::string(400, '1') + "]"), std::string{"line 1, column 403: a number has more digits than the limit"});

    const Limits plain;

    SAME(plain.bytes, std::size_t{16} << 20);
    SAME(plain.depth, 64);
    SAME(plain.values, std::size_t{1} << 20);
    SAME(plain.digits, 400);
}

// A whole number that fits 64 bits stays exact, a larger one or one with a fraction or exponent is
// the nearest double, written so that it reads back the same and as a number; one beyond the
// doubles is refused.
TRIAL("JSON: numbers") {
    SAME(parsed("9223372036854775807").integer(), std::numeric_limits<std::int64_t>::max());
    SAME(parsed("-9223372036854775808").integer(), std::numeric_limits<std::int64_t>::min());
    OUGHT(parsed("9223372036854775808").kind() == Kind::number);
    SAME(parsed("9223372036854775808").number(), 9223372036854775808.0);
    OUGHT(parsed("1.0").kind() == Kind::number);
    OUGHT(parsed("1e2").kind() == Kind::number);
    SAME(parsed("7").number(), 7.0);

    for (const double one : {0.1, 1e-300, 5e-324, 1.7976931348623157e308, -2.5, 3.0, 123456789.125, 1e21})
        SAME(parsed(written(Value{one})).number(), one);

    SAME(written(Value{3.0}), std::string{"3.0"});
    SAME(written(Value{-2.5}), std::string{"-2.5"});
    SAME(written(Value{7}), std::string{"7"});
    SAME(written(Value{std::uint8_t{200}}), std::string{"200"});
    OUGHT(throws<std::invalid_argument>([] { (void)written(Value{std::numeric_limits<double>::infinity()}); }));
    OUGHT(throws<std::invalid_argument>([] { (void)written(Value{std::numeric_limits<double>::quiet_NaN()}); }));
}

// Strings come back with their escapes undone and go out with the quote, the backslash and the
// control characters escaped, the rest as it is; a string that is not UTF-8 is not written.
TRIAL("JSON: strings") {
    SAME(parsed(R"("a\"b\\c\/d\be\ff\ng\rh\ti")").string(), std::string{"a\"b\\c/d\be\ff\ng\rh\ti"});
    SAME(written(Value{"a\"b\\c/d\be\ff\ng\rh\ti\x01\x1f\x7f\xc4\x85"}), std::string{"\"a\\\"b\\\\c/d\\be\\ff\\ng\\rh\\ti\\u0001\\u001f\x7f\xc4\x85\""});
    OUGHT(throws<std::invalid_argument>([] { (void)written(Value{"\xff"}); }));

    Value nested = Value::array();

    nested.push(Value{"\xc0\xaf"});

    OUGHT(throws<std::invalid_argument>([&nested] { (void)written(nested); }));
    SAME(written(Value{std::string_view{"x"}}), std::string{"\"x\""});
    SAME(written(Value{std::string{"y"}}), std::string{"\"y\""});
}

// The accessors: each kind answers its own question and refuses another's; an item past the last
// is out of range; `put` replaces a member in its place and adds one at the end; `at` reaches
// members in the order of `names`. A value made by the program stands nowhere in a text.
TRIAL("JSON: values made and asked") {
    Value object = Value::object();

    object.put("b", Value{1});
    object.put("a", Value{true});
    object.put("b", Value{"two"});

    OUGHT(object.names() == (std::vector<std::string>{"b", "a"}));
    SAME(object.at(0).string(), std::string{"two"});
    SAME(object.size(), std::size_t{2});
    SAME(written(object), std::string{"{\"b\":\"two\",\"a\":true}"});
    SAME(object.line(), 0);

    Value array = Value::array();

    array.push(Value{});
    array.push(object);

    SAME(array.size(), std::size_t{2});
    OUGHT(array.at(1) == object);
    OUGHT(!(array.at(0) == object));
    OUGHT(throws<std::out_of_range>([&array] { (void)array.at(2); }));
    OUGHT(throws<std::out_of_range>([&object] { (void)object.at(2); }));
    OUGHT(throws<std::logic_error>([] { (void)Value{}.boolean(); }));
    OUGHT(throws<std::logic_error>([] { (void)Value{1.5}.integer(); }));
    OUGHT(throws<std::logic_error>([] { (void)Value{true}.number(); }));
    OUGHT(throws<std::logic_error>([] { (void)Value{1}.string(); }));
    OUGHT(throws<std::logic_error>([] { (void)Value{"x"}.size(); }));
    OUGHT(throws<std::logic_error>([] { (void)Value{"x"}.at(0); }));
    OUGHT(throws<std::logic_error>([] { (void)Value::array().names(); }));
    OUGHT(throws<std::logic_error>([] { Value::object().push(Value{}); }));
    OUGHT(throws<std::logic_error>([] { Value::array().put("a", Value{}); }));
    OUGHT(Value{1}.find("a") == nullptr);

    const Value placed = parsed("{\n  \"\xc4\x85\": [\n    true]}");

    SAME(placed.line(), 1);
    SAME(placed.column(), 1);
    SAME(placed.at(0).line(), 2);
    SAME(placed.at(0).column(), 8);
    SAME(placed.at(0).at(0).line(), 3);
    SAME(placed.at(0).at(0).column(), 5);
}

// Random documents of every kind written compactly and indented read back equal; random damage to
// a valid text either reads or is refused with `Trouble`, never anything else.
TRIAL("JSON: random documents and random damage") {
    std::mt19937 random{11};

    for (int round = 0; round < 2000; ++round) {
        const Value one = invented(random, 4);

        OUGHT(parsed(written(one)) == one);
        OUGHT(parsed(written(one, true)) == one);
    }

    const std::string sample = written(invented(random, 6), true);
    int refused = 0;

    for (int round = 0; round < 20000; ++round) {
        std::string damaged = sample;

        for (unsigned many = random() % 4 + 1; many > 0; --many) {
            const std::size_t at = random() % (damaged.size() + 1);

            switch (random() % 3) {
                case 0: damaged.insert(at, 1, static_cast<char>(random() % 256)); break;
                case 1:
                    if (at < damaged.size()) damaged.erase(at, 1);
                    break;
                default:
                    if (at < damaged.size()) damaged[at] = static_cast<char>(random() % 256);
            }
        }

        try {
            const Value read = parsed(damaged);

            OUGHT(parsed(written(read)) == read);
        } catch (const tal::Trouble&) {
            ++refused;
        }
    }

    OUGHT(refused > 0);
}
