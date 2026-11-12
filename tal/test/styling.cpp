#include <optional>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

#include "tal/geometry.hpp"
#include "tal/style.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

// The geometry of cells and the style sheets of tal::ui: selectors and their weight, the cascade,
// the levels of color, the glyphs and the built-in sheet underneath every other one.

namespace {

using tal::Area;
using tal::Border;
using tal::Color;
using tal::Colors;
using tal::Look;
using tal::Selection;
using tal::Sides;
using tal::Spot;
using tal::State;
using tal::States;
using tal::Style;
using tal::StyleSheet;

// The classes of a node and its ancestors, kept alive for the selection that points at them.
struct Asked {
    std::vector<std::vector<std::string>> classes;
    States states;
    std::string part;

    [[nodiscard]] Selection selection() const { return Selection{classes, states, part}; }
};

// The style a sheet gives a node with `classes` (the node first, then its ancestors), `states` and
// `part`, at `level`.
Style styled(const StyleSheet& sheet, std::vector<std::vector<std::string>> classes, States states = {},
             std::string part = {}, Colors level = Colors::rgb) {
    const Asked asked{std::move(classes), states, std::move(part)};

    return sheet.style(asked.selection(), level);
}

// A sheet from rules written one to a line as "selector": {style}.
StyleSheet sheeted(std::vector<std::pair<std::string, std::string>> rules, std::string glyphs = "{}") {
    std::string text = "{\"rules\": [";

    for (std::size_t at = 0; at < rules.size(); ++at)
        text += (at == 0 ? "" : ",") + std::string{"{\"select\": \""} + rules[at].first + "\", \"style\": " + rules[at].second + "}";

    return StyleSheet::parsed(text + "], \"glyphs\": " + glyphs + "}");
}

// The message of the `Trouble` that reading `text` throws, or "" when it reads.
std::string refusal(std::string_view text) {
    try {
        (void)StyleSheet::parsed(text);
    } catch (const tal::Trouble& why) {
        OUGHT(why.kind() == tal::Kind::malformed);

        return why.what();
    }

    return {};
}

constexpr Color red{128, 0, 0};
constexpr Color blue{0, 0, 128};

TRIAL("Area: contains its own cells only, an empty area none") {
    const Area one{{2, 3}, {4, 2}};

    OUGHT(one.contains({2, 3}));
    OUGHT(one.contains({5, 4}));
    OUGHT(!one.contains({6, 4}));
    OUGHT(!one.contains({5, 5}));
    OUGHT(!one.contains({1, 3}));
    OUGHT(!Area{{0, 0}, {0, 5}}.contains({0, 0}));
}

TRIAL("Area: inner takes the sides away and never goes negative") {
    const Area one{{10, 20}, {8, 6}};

    SAME(one.inner(Sides{1, 2, 3, 4}), (Area{{14, 21}, {2, 2}}));
    SAME(one.inner(Sides{10, 0, 0, 0}), (Area{{10, 26}, {8, 0}}));
    SAME(one.inner(Sides{0, 5, 0, 5}), (Area{{15, 20}, {0, 6}}));
}

TRIAL("Area: common is the overlap, or empty where there is none") {
    const Area one{{0, 0}, {10, 10}};

    SAME(one.common(Area{{5, 7}, {10, 10}}), (Area{{5, 7}, {5, 3}}));
    SAME(one.common(Area{{2, 2}, {3, 3}}), (Area{{2, 2}, {3, 3}}));
    SAME(one.common(Area{{20, 1}, {3, 3}}).size, (tal::Size{0, 3}));
    SAME(one.common(Area{{20, 20}, {3, 3}}).size, (tal::Size{0, 0}));
}

TRIAL("States: a set of states, compared as a set") {
    States one{State::focus, State::mark};

    OUGHT(one.has(State::focus));
    OUGHT(one.has(State::mark));
    OUGHT(!one.has(State::disabled));

    one.set(State::focus, false);
    one.set(State::drag, true);

    SAME(one, (States{State::drag, State::mark}));
    OUGHT(!(one == States{}));
}

TRIAL("Sheet: more weight wins whatever the order, and at equal weight the later rule") {
    const StyleSheet sheet = sheeted({{"knob:focus", R"({"ink": "red"})"},
                                      {"knob", R"({"ink": "blue", "bold": true})"},
                                      {"knob", R"({"ink": "green"})"}});

    SAME(styled(sheet, {{"knob"}}).look.ink, (std::optional<Color>{Color{0, 128, 0}}));
    SAME(styled(sheet, {{"knob"}}, {State::focus}).look.ink, std::optional<Color>{red});
    OUGHT(styled(sheet, {{"knob"}}, {State::focus}).look.bold);
}

TRIAL("Sheet: states and the part outweigh classes, classes outweigh the ancestor") {
    const StyleSheet sheet = sheeted({{"frame knob:focus", R"({"ink": "#010101"})"},
                                      {"knob:focus", R"({"ink": "#020202"})"},
                                      {"knob.big.round", R"({"ink": "#030303"})"},
                                      {"frame knob", R"({"ink": "#040404"})"},
                                      {"knob.big", R"({"ink": "#050505"})"}});

    SAME(styled(sheet, {{"knob", "big"}, {"frame"}}).look.ink, (std::optional<Color>{Color{5, 5, 5}}));
    SAME(styled(sheet, {{"knob"}, {"frame"}}).look.ink, (std::optional<Color>{Color{4, 4, 4}}));
    SAME(styled(sheet, {{"knob", "big", "round"}}, {State::focus}).look.ink, (std::optional<Color>{Color{2, 2, 2}}));
    SAME(styled(sheet, {{"knob", "big", "round"}, {"frame"}}, {State::focus}).look.ink, (std::optional<Color>{Color{1, 1, 1}}));
}

TRIAL("Sheet: a rule matches with all its classes and states, its part and an ancestor anywhere") {
    const StyleSheet sheet = sheeted({{"knob.big:focus:mark", R"({"bold": true})"},
                                      {"knob::key", R"({"under": true})"},
                                      {"window knob", R"({"italic": true})"}});

    OUGHT(styled(sheet, {{"big", "knob", "other"}}, {State::focus, State::mark, State::drag}).look.bold);
    OUGHT(!styled(sheet, {{"knob"}}, {State::focus, State::mark}).look.bold);
    OUGHT(!styled(sheet, {{"knob", "big"}}, {State::focus}).look.bold);
    OUGHT(styled(sheet, {{"knob"}}, {}, "key").look.under);
    OUGHT(!styled(sheet, {{"knob"}}).look.under);
    OUGHT(!styled(sheet, {{"knob"}}, {}, "title").look.under);
    OUGHT(styled(sheet, {{"knob"}, {"row"}, {"pane", "window"}}).look.italic);
    OUGHT(!styled(sheet, {{"knob", "window"}}).look.italic);
    SAME(styled(sheet, {}), Style{});
}

TRIAL("Sheet: fields a rule does not give stay, and \"default\" clears a color") {
    const StyleSheet sheet = sheeted({{"knob", R"({"ink": "red", "paper": "blue", "bold": true, "faint": true})"},
                                      {"knob:focus", R"({"paper": "default", "faint": false, "reverse": true})"},
                                      {"knob:mark", R"({"ink": "default"})"}});
    const Look look = styled(sheet, {{"knob"}}, {State::focus}).look;

    SAME(look.ink, std::optional<Color>{red});
    OUGHT(!styled(sheet, {{"knob"}}, {State::mark}).look.ink.has_value());
    SAME(styled(sheet, {{"knob"}}, {State::mark}).look.paper, std::optional<Color>{blue});
    OUGHT(!look.paper.has_value());
    OUGHT(look.bold);
    OUGHT(!look.faint);
    OUGHT(look.reverse);
}

TRIAL("Sheet: the variant of the highest level not above the terminal's, none below it gives nothing") {
    const StyleSheet sheet = sheeted({{"knob", R"({"bold": true})"},
                                      {"knob", R"({"mono": {"under": true}, "256": {"ink": "color196"}})"},
                                      {"knob.rich", R"({"256": {"ink": "#ffffff"}})"}});

    OUGHT(styled(sheet, {{"knob"}}, {}, {}, Colors::mono).look.under);
    OUGHT(styled(sheet, {{"knob"}}, {}, {}, Colors::ansi).look.under);
    SAME(styled(sheet, {{"knob"}}, {}, {}, Colors::palette).look.ink, (std::optional<Color>{Color{255, 0, 0}}));
    SAME(styled(sheet, {{"knob"}}, {}, {}, Colors::rgb).look.ink, (std::optional<Color>{Color{255, 0, 0}}));
    OUGHT(styled(sheet, {{"knob"}}, {}, {}, Colors::rgb).look.bold);
    OUGHT(!styled(sheet, {{"knob", "rich"}}, {}, {}, Colors::ansi).look.ink.has_value());
    SAME(styled(sheet, {{"knob", "rich"}}, {}, {}, Colors::rgb).look.ink, (std::optional<Color>{Color{255, 255, 255}}));
}

TRIAL("Sheet: every spelling of a color that the old skins knew") {
    const auto ink = [](std::string_view said) {
        return styled(sheeted({{"knob", "{\"ink\": \"" + std::string{said} + "\"}"}}), {{"knob"}}).look.ink;
    };

    SAME(ink("brown"), (std::optional<Color>{Color{128, 128, 0}}));
    SAME(ink("brightcyan"), (std::optional<Color>{Color{0, 255, 255}}));
    SAME(ink("color0"), (std::optional<Color>{Color{0, 0, 0}}));
    SAME(ink("color16"), (std::optional<Color>{Color{0, 0, 0}}));
    SAME(ink("color231"), (std::optional<Color>{Color{255, 255, 255}}));
    SAME(ink("color67"), (std::optional<Color>{Color{95, 135, 175}}));
    SAME(ink("gray0"), (std::optional<Color>{Color{8, 8, 8}}));
    SAME(ink("gray23"), (std::optional<Color>{Color{238, 238, 238}}));
    SAME(ink("rgb520"), (std::optional<Color>{Color{255, 135, 0}}));
    SAME(ink("#1a2B3c"), (std::optional<Color>{Color{0x1a, 0x2b, 0x3c}}));
    SAME(ink("#f0a"), (std::optional<Color>{Color{255, 0, 170}}));
    OUGHT(!ink("default").has_value());
}

TRIAL("Sheet: the box of a style, with sides as one number or four") {
    const Style one = styled(sheeted({{"pane", R"({"border": "twin", "padding": 1, "margin": [0, 2, 3, 4]})"}}), {{"pane"}});

    SAME(one.border, Border::twin);
    SAME(one.padding, (Sides{1, 1, 1, 1}));
    SAME(one.margin, (Sides{0, 2, 3, 4}));
    OUGHT(!one.shadow);
}

// A shadow is said with a boolean, and a later rule may take it away.
TRIAL("Sheet: a style casts a shadow when its rule says so") {
    const StyleSheet sheet = sheeted({{"pane", R"({"shadow": true})"}, {"pane.flat", R"({"shadow": false})"}});

    OUGHT(styled(sheet, {{"pane"}}).shadow);
    OUGHT(!styled(sheet, {{"pane", "flat"}}).shadow);
    OUGHT(refusal(R"({"rules": [{"select": "pane", "style": {"shadow": 1}}]})").ends_with("\"shadow\" is true or false"));
}

TRIAL("Sheet: numbers in every form JSON allows, read exactly where they are whole") {
    const auto padded = [](std::string_view said) {
        return styled(sheeted({{"pane", "{\"padding\": " + std::string{said} + "}"}}), {{"pane"}}).padding.top;
    };

    SAME(padded("0"), 0);
    SAME(padded("1000"), 1000);
    SAME(padded("2.0"), 2);
    SAME(padded("0.5e1"), 5);
    SAME(padded("30E-1"), 3);
    SAME(padded("1000e-3"), 1);
    SAME(padded("0e99999"), 0);
    SAME(padded("-0"), 0);
    SAME(padded("0.000000000000000000000000000003e30"), 3);
    SAME(padded("100000000000000000000e-18"), 100);
    SAME(refusal(R"({"rules": [{"select": "pane", "style": {"padding": 1e999999}}]})"),
         std::string{"line 1, column 52: the number is out of range"});
    OUGHT(refusal(R"({"rules": [{"select": "pane", "style": {"padding": 1001}}]})").ends_with("from 0 to 1000"));
    OUGHT(refusal(R"({"rules": [{"select": "pane", "style": {"padding": -1}}]})").ends_with("from 0 to 1000"));
    OUGHT(refusal(R"({"rules": [{"select": "pane", "style": {"padding": 2.5}}]})").ends_with("from 0 to 1000"));
    OUGHT(refusal(R"({"rules": [{"select": "pane", "style": {"padding": 25E-1}}]})").ends_with("from 0 to 1000"));
    OUGHT(refusal(R"({"x": 1.})").ends_with("a fraction needs a digit"));
    OUGHT(refusal(R"({"x": 1e})").ends_with("an exponent needs a digit"));
    OUGHT(refusal(R"({"x": -})").ends_with("a number needs a digit"));
}

TRIAL("Sheet: a sheet lies over the built-in one, which gives what it does not say") {
    const StyleSheet built;
    const StyleSheet sheet = sheeted({{"menu:focus", R"({"bold": true})"}});

    OUGHT(styled(built, {{"button"}}, {State::focus}).look.reverse);
    OUGHT(styled(sheet, {{"button"}}, {State::focus}).look.reverse);
    OUGHT(styled(built, {{"link"}}, {State::focus}).look.under);
    OUGHT(styled(built, {{"input"}}, {State::disabled}).look.faint);
    OUGHT(styled(sheet, {{"menu"}}, {State::focus}).look.bold);
    OUGHT(styled(sheet, {{"menu"}}, {State::focus}).look.reverse);
    SAME(styled(built, {{"window"}}).border, Border::thin);
    SAME(styled(built, {{"window", "dialog"}}).border, Border::twin);
    OUGHT(styled(built, {{"window", "dialog"}}).shadow);
    SAME(styled(built, {{"window", "screen"}}).border, Border::none);
}

TRIAL("Sheet: glyphs by level, from the sheet, else the built-in one, else a question mark") {
    const StyleSheet sheet = sheeted({}, R"({"dot": "*", "arrow": {"ascii": ">", "unicode": "\u25b8"}, "plain": {"ascii": "-"},
                                             "fancy": {"unicode": "\ud83d\ude00"}})");

    SAME(sheet.glyph("dot", true), std::string_view{"*"});
    SAME(sheet.glyph("arrow", false), std::string_view{">"});
    SAME(sheet.glyph("arrow", true), std::string_view{"\u25b8"});
    SAME(sheet.glyph("plain", true), std::string_view{"-"});
    SAME(sheet.glyph("fancy", true), std::string_view{"\U0001F600"});
    SAME(sheet.glyph("fancy", false), std::string_view{"?"});
    SAME(sheet.glyph("elide", true), std::string_view{"\u2026"});
    SAME(sheet.glyph("nothing.like.it", true), std::string_view{"?"});
}

TRIAL("Sheet: the lines of a frame, thin for none, from the sheet over the built-in ones") {
    const StyleSheet built;
    const StyleSheet sheet = sheeted({}, R"({"line.twin.across": "="})");

    SAME(built.lines(Border::thin, true).upper_left, std::string_view{"\u250c"});
    SAME(built.lines(Border::none, true).across, std::string_view{"\u2500"});
    SAME(built.lines(Border::thick, true).right_joint, std::string_view{"\u252b"});
    SAME(sheet.lines(Border::twin, true).across, std::string_view{"="});
    SAME(sheet.lines(Border::twin, true).upright, std::string_view{"\u2551"});
}

TRIAL("Sheet: a copy gives what the original gives, also after the original is gone") {
    std::optional<StyleSheet> first{sheeted({{"knob", R"({"ink": "red"})"}}, R"({"dot": "*"})")};
    const StyleSheet copy = *first;

    first.reset();

    SAME(styled(copy, {{"knob"}}).look.ink, std::optional<Color>{red});
    SAME(copy.glyph("dot", false), std::string_view{"*"});
}

TRIAL("Sheet: a text that is not JSON is refused at its line and column") {
    SAME(refusal("{\n  \"rules\": [,]\n}"), std::string{"line 2, column 13: a value is expected"});
    SAME(refusal("{\"rules\": []} x"), std::string{"line 1, column 15: something follows the value"});
    SAME(refusal("{\"rules\": [], \"rules\": []}"), std::string{"line 1, column 15: a name is repeated"});
    SAME(refusal("{\"glyphs\": {\"a\": \"\\ud800\"}}"), std::string{"line 1, column 25: a high surrogate stands alone"});
    SAME(refusal("[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[]]]]"), std::string{"line 1, column 65: the values nest deeper than the limit"});
    SAME(refusal("{\"x\": \"a\x01\"}"), std::string{"line 1, column 9: a control character stands in a string"});
    SAME(refusal("{\"x\": 01}"), std::string{"line 1, column 8: '}' is expected"});
    SAME(refusal("{\"x\": \xff}"), std::string{"line 1, column 1: the text is not UTF-8"});
}

TRIAL("Sheet: a sheet with a mistake is refused, not read in half") {
    const auto rule = [](std::string_view select, std::string_view style) {
        return refusal("{\"rules\": [{\"select\": \"" + std::string{select} + "\", \"style\": " + std::string{style} + "}]}");
    };

    OUGHT(rule("knob", R"({"ink": "nocolor"})").ends_with("unknown color \"nocolor\""));
    OUGHT(rule("knob", R"({"colour": "red"})").ends_with("unknown field \"colour\""));
    OUGHT(rule("knob:press", "{}").ends_with("unknown state \"press\""));
    OUGHT(rule("Knob", "{}").ends_with("the selector needs class names"));
    OUGHT(rule("a b c", "{}").ends_with("a selector has at most one ancestor"));
    OUGHT(rule("knob::a::b", "{}").ends_with("a selector has at most one part"));
    OUGHT(rule("knob.", "{}").ends_with("the selector needs class names"));
    OUGHT(rule("knob", R"({"mono": {}, "bold": true})").ends_with("a style by levels holds only mono, 16, 256 and rgb"));
    OUGHT(rule("knob", R"({"bold": 1})").ends_with("\"bold\" is true or false"));
    OUGHT(rule("knob", R"({"padding": [1, 2]})").ends_with("sides are a number or [top, right, bottom, left]"));
    OUGHT(rule("knob", R"({"padding": 1.5})").ends_with("a size is a whole number from 0 to 1000"));
    OUGHT(rule("knob", R"({"border": "dotted"})").ends_with("a border is none, thin, twin or thick"));
    OUGHT(refusal(R"({"rules": [{"select": "knob"}]})").ends_with("a rule holds \"select\" and \"style\""));
    OUGHT(refusal(R"({"glyphs": {"dot": "ab"}})").ends_with("a glyph is one character"));
    OUGHT(refusal(R"({"glyphs": {"dot": {"wide": "a"}}})").ends_with("a glyph holds only ascii and unicode"));
    OUGHT(refusal(R"({"styles": {}})").ends_with("unknown member \"styles\""));
    OUGHT(refusal("[]").ends_with("a style sheet is an object"));
}

}
