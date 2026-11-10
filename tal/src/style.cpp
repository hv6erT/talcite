#include "tal/style.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <tuple>
#include <utility>

#include "reading.hpp"
#include "tal/text.hpp"

namespace tal {

namespace {

#include "built.inc"

using reading::Value;

constexpr std::array<std::string_view, 4> levels{"mono", "16", "256", "rgb"};

constexpr std::array<std::string_view, 6> flag_names{"bold", "faint", "italic", "under", "reverse", "crossed"};

constexpr std::array<std::string_view, 7> state_names{"focus", "disabled", "select", "current", "mark", "drag", "hover"};

constexpr std::array<std::string_view, 4> border_names{"none", "thin", "twin", "thick"};

constexpr std::array<std::string_view, 16> basics{
    "black", "red", "green", "brown", "blue", "magenta", "cyan", "lightgray",
    "gray", "brightred", "brightgreen", "yellow", "brightblue", "brightmagenta", "brightcyan", "white"};

constexpr std::array<Color, 16> basic_values{{{0, 0, 0},
                                              {128, 0, 0},
                                              {0, 128, 0},
                                              {128, 128, 0},
                                              {0, 0, 128},
                                              {128, 0, 128},
                                              {0, 128, 128},
                                              {192, 192, 192},
                                              {128, 128, 128},
                                              {255, 0, 0},
                                              {0, 255, 0},
                                              {255, 255, 0},
                                              {0, 0, 255},
                                              {255, 0, 255},
                                              {0, 255, 255},
                                              {255, 255, 255}}};

constexpr std::array<std::uint8_t, 6> steps{0, 95, 135, 175, 215, 255};

// What one rule says at one level: only the fields it gives. A color given as "default" is an
// empty color, which differs from a color not given at all.
struct Partial {
    std::optional<std::optional<Color>> ink;
    std::optional<std::optional<Color>> paper;
    std::array<std::optional<bool>, 6> flags;
    std::optional<Border> border;
    std::optional<Sides> padding;
    std::optional<Sides> margin;
};

// A rule of a sheet: its selector taken apart, its weight and what it says at each level.
struct Rule {
    std::string ancestor;
    std::vector<std::string> classes;
    States states;
    std::string part;
    std::array<std::optional<Partial>, 4> levels;
    std::array<int, 3> weight{};
};

// A glyph at the two levels; either may be missing.
struct Sign {
    std::optional<std::string> ascii;
    std::optional<std::string> unicode;
};

// What one layer of a sheet holds: its rules in order, the rules by their first class, its glyphs.
struct Kept {
    std::vector<Rule> rules;
    std::map<std::string, std::vector<std::size_t>, std::less<>> by_class;
    std::map<std::string, Sign, std::less<>> glyphs;
};

// The color of palette number `which` (0 to 255): the basic colors with xterm's values, the 6x6x6
// cube and the grey ramp, as in the old talcite, so that a skin reads the same colors.
Color indexed(int which) {
    if (which < 16) return basic_values[static_cast<std::size_t>(which)];

    if (which >= 232) {
        const auto grey = static_cast<std::uint8_t>(8 + 10 * (which - 232));

        return Color{grey, grey, grey};
    }

    const int cube = which - 16;

    return Color{steps[static_cast<std::size_t>(cube / 36)], steps[static_cast<std::size_t>((cube / 6) % 6)],
                 steps[static_cast<std::size_t>(cube % 6)]};
}

// A decimal number of one to three digits, or nothing.
std::optional<int> numbered(std::string_view said) {
    if (said.empty() || said.size() > 3) return std::nullopt;

    int made = 0;

    for (const char one : said) {
        if (one < '0' || one > '9') return std::nullopt;

        made = made * 10 + (one - '0');
    }

    return made;
}

// The value of a hexadecimal digit, or nothing.
std::optional<int> hexed(char one) {
    if (one >= '0' && one <= '9') return one - '0';
    if (one >= 'a' && one <= 'f') return one - 'a' + 10;
    if (one >= 'A' && one <= 'F') return one - 'A' + 10;

    return std::nullopt;
}

// A color in the spellings of Midnight Commander's skins: a basic name, colorN, grayN, rgbXYZ,
// #rrggbb, #rgb, and "default" for no color. Nothing for a spelling it does not know.
std::optional<std::optional<Color>> colored(std::string_view said) {
    if (said == "default") return std::optional<Color>{};

    for (std::size_t at = 0; at < basics.size(); ++at)
        if (said == basics[at]) return std::optional<Color>{basic_values[at]};

    if (said.starts_with("color")) {
        const std::optional<int> which = numbered(said.substr(5));

        if (which && *which <= 255) return std::optional<Color>{indexed(*which)};

        return std::nullopt;
    }

    if (said.starts_with("gray")) {
        const std::optional<int> which = numbered(said.substr(4));

        if (which && *which <= 23) return std::optional<Color>{indexed(232 + *which)};

        return std::nullopt;
    }

    if (said.starts_with("rgb") && said.size() == 6) {
        int which = 16;

        for (std::size_t at = 0; at < 3; ++at) {
            const char one = said[3 + at];

            if (one < '0' || one > '5') return std::nullopt;

            which += (one - '0') * (at == 0 ? 36 : at == 1 ? 6 : 1);
        }

        return std::optional<Color>{indexed(which)};
    }

    if (said.starts_with("#") && (said.size() == 7 || said.size() == 4)) {
        std::array<int, 6> digits{};

        for (std::size_t at = 1; at < said.size(); ++at) {
            const std::optional<int> one = hexed(said[at]);

            if (!one) return std::nullopt;

            digits[at - 1] = *one;
        }

        if (said.size() == 4)
            return std::optional<Color>{Color{static_cast<std::uint8_t>(digits[0] * 17), static_cast<std::uint8_t>(digits[1] * 17),
                                              static_cast<std::uint8_t>(digits[2] * 17)}};

        return std::optional<Color>{Color{static_cast<std::uint8_t>(digits[0] * 16 + digits[1]),
                                          static_cast<std::uint8_t>(digits[2] * 16 + digits[3]),
                                          static_cast<std::uint8_t>(digits[4] * 16 + digits[5])}};
    }

    return std::nullopt;
}

// The index of `said` in `among`, or nothing.
template <std::size_t many>
std::optional<std::size_t> found(const std::array<std::string_view, many>& among, std::string_view said) {
    const auto at = std::find(among.begin(), among.end(), said);

    if (at == among.end()) return std::nullopt;

    return static_cast<std::size_t>(at - among.begin());
}

// Whether `said` is a name in a selector: lower-case letters, digits, '-' and '_', at least one.
bool named(std::string_view said) {
    return !said.empty() && std::all_of(said.begin(), said.end(), [](char one) {
        return (one >= 'a' && one <= 'z') || (one >= '0' && one <= '9') || one == '-' || one == '_';
    });
}

// The parts of `said` between the separator `by`, empty ones too.
std::vector<std::string_view> split(std::string_view said, std::string_view by) {
    std::vector<std::string_view> made;

    while (true) {
        const std::size_t at = said.find(by);

        made.push_back(said.substr(0, at));

        if (at == std::string_view::npos) return made;

        said.remove_prefix(at + by.size());
    }
}

// Takes the selector `[ancestor ]class(.class)*(:state)*(::part)?` apart into `into`, with its
// weight: states and part, then classes of the node, then the ancestor.
void selected(const Value& where, Rule& into) {
    if (where.kind != reading::Kind::string) reading::refused(where, "\"select\" is not a string");

    std::string_view said = where.text;
    const std::vector<std::string_view> spaced = split(said, " ");

    if (spaced.size() > 2) reading::refused(where, "a selector has at most one ancestor");

    if (spaced.size() == 2) {
        if (!named(spaced[0])) reading::refused(where, "the ancestor is not a class name");

        into.ancestor = std::string{spaced[0]};
        said = spaced[1];
    }

    const std::vector<std::string_view> parted = split(said, "::");

    if (parted.size() > 2) reading::refused(where, "a selector has at most one part");

    if (parted.size() == 2) {
        if (!named(parted[1])) reading::refused(where, "the part is not a name");

        into.part = std::string{parted[1]};
    }

    const std::vector<std::string_view> stated = split(parted[0], ":");
    int many_states = 0;

    for (std::size_t at = 1; at < stated.size(); ++at) {
        const std::optional<std::size_t> which = found(state_names, stated[at]);

        if (!which) reading::refused(where, "unknown state \"" + std::string{stated[at]} + "\"");

        if (!into.states.has(static_cast<State>(*which))) ++many_states;

        into.states.set(static_cast<State>(*which), true);
    }

    for (const std::string_view one : split(stated[0], ".")) {
        if (!named(one)) reading::refused(where, "the selector needs class names");

        into.classes.emplace_back(one);
    }

    into.weight = {many_states + (into.part.empty() ? 0 : 1), static_cast<int>(into.classes.size()),
                   into.ancestor.empty() ? 0 : 1};
}

// A whole non-negative number of cells, at most a thousand.
int counted(const Value& one) {
    if (one.kind != reading::Kind::number || one.number < 0 || one.number > 1000 || std::floor(one.number) != one.number)
        reading::refused(one, "a size is a whole number from 0 to 1000");

    return static_cast<int>(one.number);
}

// Sides from a number (all four) or from an array [top, right, bottom, left].
Sides sided(const Value& one) {
    if (one.kind == reading::Kind::number) {
        const int all = counted(one);

        return Sides{all, all, all, all};
    }

    if (one.kind != reading::Kind::array || one.items.size() != 4)
        reading::refused(one, "sides are a number or [top, right, bottom, left]");

    return Sides{counted(one.items[0]), counted(one.items[1]), counted(one.items[2]), counted(one.items[3])};
}

// What a style object says at one level. An unknown field is a mistake, since this format is ours.
Partial partial(const Value& one) {
    if (one.kind != reading::Kind::object) reading::refused(one, "a style is an object");

    Partial made;

    for (std::size_t at = 0; at < one.names.size(); ++at) {
        const std::string& name = one.names[at];
        const Value& given = one.items[at];

        if (name == "ink" || name == "paper") {
            if (given.kind != reading::Kind::string) reading::refused(given, "a color is a string");

            const std::optional<std::optional<Color>> color = colored(given.text);

            if (!color) reading::refused(given, "unknown color \"" + given.text + "\"");

            (name == "ink" ? made.ink : made.paper) = *color;
        } else if (const std::optional<std::size_t> flag = found(flag_names, name)) {
            if (given.kind != reading::Kind::boolean) reading::refused(given, "\"" + name + "\" is true or false");

            made.flags[*flag] = given.boolean;
        } else if (name == "border") {
            const std::optional<std::size_t> kind = given.kind == reading::Kind::string ? found(border_names, given.text)
                                                                                        : std::nullopt;

            if (!kind) reading::refused(given, "a border is none, thin, twin or thick");

            made.border = static_cast<Border>(*kind);
        } else if (name == "padding") {
            made.padding = sided(given);
        } else if (name == "margin") {
            made.margin = sided(given);
        } else {
            reading::refused(given, "unknown field \"" + name + "\"");
        }
    }

    return made;
}

// The levels of a style: one flat style for every level, or an object of levels, each the style
// from that level up to the next one given.
std::array<std::optional<Partial>, 4> leveled(const Value& one) {
    if (one.kind != reading::Kind::object) reading::refused(one, "a style is an object");

    std::array<std::optional<Partial>, 4> made;
    const bool by_level = std::any_of(one.names.begin(), one.names.end(),
                                      [](const std::string& name) { return found(levels, name).has_value(); });

    if (!by_level) {
        made[0] = partial(one);

        return made;
    }

    for (std::size_t at = 0; at < one.names.size(); ++at) {
        const std::optional<std::size_t> level = found(levels, one.names[at]);

        if (!level) reading::refused(one.items[at], "a style by levels holds only mono, 16, 256 and rgb");

        made[*level] = partial(one.items[at]);
    }

    return made;
}

// One glyph string: exactly one cluster of letters.
std::string lettered(const Value& one) {
    if (one.kind != reading::Kind::string || text::clusters(one.text).size() != 1)
        reading::refused(one, "a glyph is one character");

    return one.text;
}

// A glyph: one string for both levels, or an object with "ascii" and "unicode".
Sign glyphed(const Value& one) {
    if (one.kind == reading::Kind::string) {
        const std::string same = lettered(one);

        return Sign{same, same};
    }

    if (one.kind != reading::Kind::object || one.names.empty())
        reading::refused(one, "a glyph is a string or an object with ascii and unicode");

    Sign made;

    for (std::size_t at = 0; at < one.names.size(); ++at) {
        if (one.names[at] == "ascii") {
            made.ascii = lettered(one.items[at]);
        } else if (one.names[at] == "unicode") {
            made.unicode = lettered(one.items[at]);
        } else {
            reading::refused(one.items[at], "a glyph holds only ascii and unicode");
        }
    }

    return made;
}

// The layer a sheet's text describes.
Kept kept(std::string_view json) {
    const Value doc = reading::parsed(json);

    if (doc.kind != reading::Kind::object) reading::refused(doc, "a style sheet is an object");

    Kept made;

    for (std::size_t at = 0; at < doc.names.size(); ++at) {
        const std::string& name = doc.names[at];
        const Value& one = doc.items[at];

        if (name == "description") {
            if (one.kind != reading::Kind::string) reading::refused(one, "the description is a string");
        } else if (name == "rules") {
            if (one.kind != reading::Kind::array) reading::refused(one, "the rules are an array");

            for (const Value& given : one.items) {
                if (given.kind != reading::Kind::object) reading::refused(given, "a rule is an object");

                const Value* select = given.member("select");
                const Value* style = given.member("style");

                if (select == nullptr || style == nullptr || given.names.size() != 2)
                    reading::refused(given, "a rule holds \"select\" and \"style\"");

                Rule rule;

                selected(*select, rule);
                rule.levels = leveled(*style);

                made.by_class[rule.classes.front()].push_back(made.rules.size());
                made.rules.push_back(std::move(rule));
            }
        } else if (name == "glyphs") {
            if (one.kind != reading::Kind::object) reading::refused(one, "the glyphs are an object");

            for (std::size_t sign = 0; sign < one.names.size(); ++sign)
                made.glyphs.emplace(one.names[sign], glyphed(one.items[sign]));
        } else {
            reading::refused(one, "unknown member \"" + name + "\"");
        }
    }

    return made;
}

// Whether `rule` applies to `who`: its part, all its states, all its classes on the node, and its
// ancestor among the node's ancestors.
bool matches(const Rule& rule, const Selection& who) {
    if (rule.part != who.part) return false;

    for (std::size_t at = 0; at < state_names.size(); ++at)
        if (rule.states.has(static_cast<State>(at)) && !who.states.has(static_cast<State>(at))) return false;

    const std::vector<std::string>& mine = who.classes.front();

    for (const std::string& one : rule.classes)
        if (std::find(mine.begin(), mine.end(), one) == mine.end()) return false;

    if (rule.ancestor.empty()) return true;

    return std::any_of(who.classes.begin() + 1, who.classes.end(), [&rule](const std::vector<std::string>& above) {
        return std::find(above.begin(), above.end(), rule.ancestor) != above.end();
    });
}

// The variant of a rule at the highest level not above `level`, or nothing.
const Partial* fitted(const Rule& rule, Colors level) {
    for (int at = static_cast<int>(level); at >= 0; --at)
        if (rule.levels[static_cast<std::size_t>(at)]) return &*rule.levels[static_cast<std::size_t>(at)];

    return nullptr;
}

// Lays what a rule gives over the style made so far.
void laid(Style& made, const Partial& over) {
    if (over.ink) made.look.ink = *over.ink;
    if (over.paper) made.look.paper = *over.paper;

    bool* const flags[] = {&made.look.bold, &made.look.faint, &made.look.italic,
                           &made.look.under, &made.look.reverse, &made.look.crossed};

    for (std::size_t at = 0; at < over.flags.size(); ++at)
        if (over.flags[at]) *flags[at] = *over.flags[at];

    if (over.border) made.border = *over.border;
    if (over.padding) made.padding = *over.padding;
    if (over.margin) made.margin = *over.margin;
}

}

// A layer of a sheet over the one underneath it; the built-in sheet has nothing underneath.
struct StyleSheet::Held {
    Kept kept;
    std::shared_ptr<const Held> under;
};

// The states named.
States::States(std::initializer_list<State> some) {
    for (const State one : some) set(one, true);
}

// Whether the state `one` is in the set.
bool States::has(State one) const { return (held_ >> static_cast<unsigned>(one) & 1U) != 0; }

// Puts the state `one` in the set or takes it out.
void States::set(State one, bool on) {
    const std::uint32_t bit = 1U << static_cast<unsigned>(one);

    held_ = on ? held_ | bit : held_ & ~bit;
}

// The built-in sheet of tal. It is read once and never destroyed, so that a sheet made while the
// program ends still finds it.
StyleSheet::StyleSheet() {
    static const auto* const built = new std::shared_ptr<const Held>{std::make_shared<const Held>(Held{kept(built_sheet), nullptr})};

    held_ = *built;
}

// A sheet over a layer already made.
StyleSheet::StyleSheet(std::shared_ptr<const Held> held) : held_(std::move(held)) {}

// A sheet from JSON (`description`, `rules` of `select` and `style`, `glyphs`), laid over the
// built-in one: what it does not say comes from there. A text that is not such a sheet throws
// `Trouble` of kind `malformed` with the line and column of the mistake.
StyleSheet StyleSheet::parsed(std::string_view json) {
    return StyleSheet{std::make_shared<const Held>(Held{kept(json), StyleSheet{}.held_})};
}

// The style of `who` at the color level `level`: the matching rules of every layer from the least
// weight to the most, the built-in layer before this one at equal weight and a later rule after
// an earlier one; each rule gives only its fields, at its variant for the level. A rule without a
// variant for the level gives nothing. A node without classes matches nothing.
Style StyleSheet::style(const Selection& who, Colors level) const {
    Style made;

    if (who.classes.empty()) return made;

    std::vector<const Held*> layers;

    for (const Held* one = held_.get(); one != nullptr; one = one->under.get()) layers.push_back(one);

    std::reverse(layers.begin(), layers.end());

    std::vector<std::tuple<std::array<int, 3>, std::size_t, std::size_t, const Partial*>> applying;

    for (std::size_t layer = 0; layer < layers.size(); ++layer) {
        const Kept& kept = layers[layer]->kept;
        const std::size_t first = applying.size();

        for (const std::string& one : who.classes.front()) {
            const auto at = kept.by_class.find(one);

            if (at == kept.by_class.end()) continue;

            for (const std::size_t index : at->second) {
                const Rule& rule = kept.rules[index];
                const Partial* given = fitted(rule, level);

                if (given != nullptr && matches(rule, who)) applying.emplace_back(rule.weight, layer, index, given);
            }
        }

        std::sort(applying.begin() + static_cast<std::ptrdiff_t>(first), applying.end());
        applying.erase(std::unique(applying.begin() + static_cast<std::ptrdiff_t>(first), applying.end()), applying.end());
    }

    std::sort(applying.begin(), applying.end());

    for (const auto& one : applying) laid(made, *std::get<3>(one));

    return made;
}

// The glyph `name` for a terminal with Unicode or without: the Unicode form when there is one and
// the terminal shows it, otherwise the ASCII form, from this layer, else from the one underneath,
// else "?". The text lives as long as the sheet or any copy of it.
std::string_view StyleSheet::glyph(std::string_view name, bool unicode) const {
    for (const Held* one = held_.get(); one != nullptr; one = one->under.get()) {
        const auto at = one->kept.glyphs.find(name);

        if (at == one->kept.glyphs.end()) continue;

        if (unicode && at->second.unicode) return *at->second.unicode;

        if (at->second.ascii) return *at->second.ascii;
    }

    return "?";
}

// The eight glyphs of a frame of the kind `kind` (`line.thin.*`, `line.twin.*`, `line.thick.*`);
// `Border::none` gives the thin ones.
Lines StyleSheet::lines(Border kind, bool unicode) const {
    const std::string stem = std::string{"line."} + std::string{kind == Border::none ? "thin" : border_names[static_cast<std::size_t>(kind)]} + ".";
    const auto one = [&](std::string_view rest) { return glyph(stem + std::string{rest}, unicode); };

    return Lines{one("upper.left"), one("upper.right"), one("lower.left"), one("lower.right"),
                 one("across"),     one("upright"),     one("left.joint"), one("right.joint")};
}

}
