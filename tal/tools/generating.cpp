#include "generating.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <utility>

#include "tal/json.hpp"
#include "tal/text.hpp"
#include "tal/trouble.hpp"
#include "tal/wording.hpp"

// The generator of a program's texts: out of the English file it makes a header and a source of
// C++ functions, one for each text, named by the text's road (`menu.left.text` is
// `texts::menu::left::text()`), and it checks every other language against English. A problem is
// reported as "file:line:column: why", the way compilers report theirs, so that an editor can go
// to it.
namespace tal::generating {

namespace {

// The words of C++, which a name cannot be.
constexpr std::string_view keywords[] = {
    "alignas",      "alignof",     "and",        "and_eq",       "asm",          "auto",         "bitand",      "bitor",
    "bool",         "break",       "case",       "catch",        "char",         "char8_t",      "char16_t",    "char32_t",
    "class",        "compl",       "concept",    "const",        "consteval",    "constexpr",    "constinit",   "const_cast",
    "continue",     "co_await",    "co_return",  "co_yield",     "decltype",     "default",      "delete",      "do",
    "double",       "dynamic_cast", "else",      "enum",         "explicit",     "export",       "extern",      "false",
    "float",        "for",         "friend",     "goto",         "if",           "inline",       "int",         "long",
    "mutable",      "namespace",   "new",        "noexcept",     "not",          "not_eq",       "nullptr",     "operator",
    "or",           "or_eq",       "private",    "protected",    "public",       "register",     "reinterpret_cast",
    "requires",     "return",      "short",      "signed",       "sizeof",       "static",       "static_assert",
    "static_cast",  "struct",      "switch",     "template",     "this",         "thread_local", "throw",       "true",
    "try",          "typedef",     "typeid",     "typename",     "union",        "unsigned",     "using",       "virtual",
    "void",         "volatile",    "wchar_t",    "while",        "xor",          "xor_eq"};

// Small names that compilers or system headers define as macros, which would swallow a function
// of that name: `near` and `far` in windows.h, `linux` in GNU C++, `major` in glibc.
constexpr std::string_view macros[] = {"assert",  "cdecl",  "errno",  "far",      "getc",     "hyper",    "i386",     "interface",
                                       "linux",   "major",  "makedev", "max",     "min",      "minor",    "near",     "offsetof",
                                       "pascal",  "putc",   "setjmp", "small",    "st_atime", "st_ctime", "st_mtime", "stderr",
                                       "stdin",   "stdout", "unix",   "va_arg",   "va_copy",  "va_end",   "va_start"};

// The names of the values a text takes, in the order of its {}.
constexpr std::string_view ordinals[] = {"first", "second", "third", "fourth", "fifth", "sixth", "seventh", "eighth", "ninth"};

// A problem in `file` at `line` and `column`.
std::string located(std::string_view file, int line, int column, std::string_view why) {
    return std::string{file} + ":" + std::to_string(line) + ":" + std::to_string(column) + ": " + std::string{why};
}

// The refusal `why` of the JSON reader for `file`: its "line L, column C: ..." made into a place.
std::string refused(std::string_view file, const Trouble& why) {
    const std::string_view said = why.what();
    int line = 0;
    int column = 0;
    std::size_t at = 0;

    const auto number = [&said, &at](std::string_view before, int& into) {
        if (!said.substr(at).starts_with(before)) return false;

        at += before.size();

        const std::size_t from = at;

        while (at < said.size() && said[at] >= '0' && said[at] <= '9' && at - from < 9) into = into * 10 + (said[at++] - '0');

        return at > from;
    };

    if (number("line ", line) && number(", column ", column) && said.substr(at).starts_with(": "))
        return located(file, line, column, said.substr(at + 2));

    return std::string{file} + ": " + std::string{said};
}

// Whether `name` can name a text: small letters, digits, `_` and `-`, starting with a letter.
bool usable(std::string_view name) {
    if (name.empty() || name.front() < 'a' || name.front() > 'z') return false;

    return std::all_of(name.begin(), name.end(), [](char one) { return (one >= 'a' && one <= 'z') || (one >= '0' && one <= '9') || one == '_' || one == '-'; });
}

// The C++ name of the name `name`: `-` is `_`, and a word of C++ or a macro takes `_` at the end.
std::string mangled(std::string_view name) {
    std::string made{name};

    std::replace(made.begin(), made.end(), '-', '_');

    if (std::find(std::begin(keywords), std::end(keywords), made) != std::end(keywords) ||
        std::find(std::begin(macros), std::end(macros), made) != std::end(macros))
        made += '_';

    return made;
}

// Whether `space` can be the namespace of the texts, with its parts put in `parts`: C++ names
// joined by "::", none of them a word of C++ or kept by C++, and not inside `std` or `tal`.
bool spaced(std::string_view space, std::vector<std::string>& parts) {
    for (std::size_t from = 0;;) {
        const std::size_t to = space.find("::", from);
        const std::string_view part = space.substr(from, to == std::string_view::npos ? std::string_view::npos : to - from);
        const bool letters = !part.empty() && !(part.front() >= '0' && part.front() <= '9') &&
                             std::all_of(part.begin(), part.end(), [](char one) {
                                 return (one >= 'a' && one <= 'z') || (one >= 'A' && one <= 'Z') || (one >= '0' && one <= '9') || one == '_';
                             });

        if (!letters || part.starts_with('_') || part.find("__") != std::string_view::npos ||
            std::find(std::begin(keywords), std::end(keywords), part) != std::end(keywords))
            return false;

        parts.emplace_back(part);

        if (to == std::string_view::npos) break;

        from = to + 2;
    }

    return parts.front() != "std" && parts.front() != "tal";
}

// The file name of the path `path`, without the folders before it.
std::string_view base_name(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");

    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// The declaration of the function `name` for `entry`, without the semicolon or the body.
std::string declared(const wording::Entry& entry, std::string_view name) {
    if (entry.shape == wording::Shape::letter) return "[[nodiscard]] char32_t " + std::string{name} + "()";

    std::string made = "[[nodiscard]] ::std::string " + std::string{name} + "(";
    bool first = true;

    if (entry.shape == wording::Shape::counted) {
        made += "::std::uint64_t many";
        first = false;
    }

    for (int at = 0; at < entry.slots; ++at) {
        made += first ? "" : ", ";
        made += "::std::string_view " + std::string{ordinals[at]};
        first = false;
    }

    return made + ")";
}

// The body of the function for `entry`, the text `index` of the dictionary of `space`.
std::string body(const wording::Entry& entry, std::size_t index, std::string_view space) {
    const std::string dictionary = "::" + std::string{space} + "::dictionary()";
    const std::string place = std::to_string(index);

    if (entry.shape == wording::Shape::letter) return "{ return " + dictionary + ".letter(" + place + "); }";

    const std::string text =
        entry.shape == wording::Shape::counted ? dictionary + ".counted(" + place + ", many)" : dictionary + ".said(" + place + ")";

    if (entry.slots == 0) return "{ return " + text + "; }";

    std::string values;

    for (int at = 0; at < entry.slots; ++at) values += (at == 0 ? "" : ", ") + std::string{ordinals[at]};

    return "{ return ::tal::filled(" + text + ", {" + values + "}); }";
}

// One line for each of `names` (`line` makes the line of the one at a place), in blocks of their
// namespaces under `space`: a run of names of one namespace shares one block.
template <typename Line>
std::string blocks(const std::vector<std::vector<std::string>>& names, std::string_view space, Line line) {
    std::string made;
    std::string open;

    for (std::size_t at = 0; at < names.size(); ++at) {
        std::string scope{space};

        for (std::size_t part = 0; part + 1 < names[at].size(); ++part) scope += "::" + names[at][part];

        if (scope != open) {
            if (!open.empty()) made += "\n}\n";

            made += "\nnamespace " + scope + " {\n\n";
            open = scope;
        }

        made += line(at);
    }

    if (!open.empty()) made += "\n}\n";

    return made;
}

// The functions of `entries`, named `names`, in blocks of their namespaces under `space`: the
// declarations, or the definitions when `defined` is true.
std::string functions(const std::vector<wording::Entry>& entries, const std::vector<std::vector<std::string>>& names, std::string_view space,
                      bool defined) {
    return blocks(names, space, [&](std::size_t at) {
        return declared(entries[at], names[at].back()) + (defined ? " " + body(entries[at], at, space) + "\n" : std::string{";\n"});
    });
}

// The document of `source` in `into`, or false, with the reader's refusal among `problems`.
bool read(const Source& source, json::Value& into, std::vector<std::string>& problems) {
    try {
        into = json::parsed(source.text);
    } catch (const Trouble& why) {
        problems.push_back(refused(source.name, why));

        return false;
    }

    return true;
}


// The C++ names of `roads` in `document`, the text of `source`, each part by `mangled`: a part must
// be usable, no two may become one C++ name (`a-b` and `a_b`), and none at the top may take the
// place of the function `reserved` of `space`. Each problem is told once, at the value of the
// first road it hit.
std::vector<std::vector<std::string>> named(const Source& source, const json::Value& document, const std::vector<std::vector<std::string>>& roads,
                                            std::string_view space, std::string_view reserved, std::vector<std::string>& problems) {
    std::map<std::pair<std::string, std::string>, std::string> taken{{{"", std::string{reserved}}, ""}};
    std::set<std::string> told;
    std::vector<std::vector<std::string>> names(roads.size());

    for (std::size_t at = 0; at < roads.size(); ++at) {
        std::vector<std::string> road;
        std::string scope;

        for (const std::string& part : roads[at]) {
            road.push_back(part);

            const std::string whole = wording::joined(road);
            const json::Value& value = *wording::found(document, road);
            const std::string name = mangled(part);
            const auto problem = [&](std::string why) {
                if (told.insert(whole).second) problems.push_back(located(source.name, value.line(), value.column(), why));
            };

            if (!usable(part)) problem("\"" + whole + "\": a name is small letters, digits, _ and -, from a letter");
            else if (name.find("__") != std::string::npos) problem("\"" + whole + "\": the name becomes " + name + ", which C++ keeps for itself");

            const auto [held, fresh] = taken.emplace(std::pair{scope, name}, whole);

            if (!fresh && held->second != whole) {
                if (held->second.empty()) problem("\"" + whole + "\" would hide " + std::string{space} + "::" + std::string{reserved} + "()");
                else problem("\"" + held->second + "\" and \"" + whole + "\" are both " + (scope.empty() ? "" : scope + "::") + name + " in C++");
            }

            names[at].push_back(name);
            scope += (scope.empty() ? "" : "::") + name;
        }
    }

    return names;
}

// `text` as the elements of an array of bytes, 24 to a line, which no compiler limits as it does
// string literals.
std::string byted(std::string_view text) {
    std::string made;

    for (std::size_t at = 0; at < text.size(); ++at) {
        made += std::to_string(static_cast<unsigned char>(text[at]));
        made += at + 1 == text.size() ? "\n" : at % 24 == 23 ? ",\n" : ",";
    }

    return made;
}

// A setting of the built-in values: its road and the C++ type of its handle.
struct Kept {
    std::vector<std::string> road;
    std::string type;
};

// The settings under the object `node` at `road`, in the order of the text, with the problems of
// their values. At the top every value is a section: an object of settings, or a list of records
// (objects of texts), which is one setting; or "version", the version of the file (a whole number
// from 1), which is no setting; deeper a value is a boolean, a whole number within
// `int`, a text, a list of texts or an object of more settings.
void kept(const Source& source, const json::Value& node, std::vector<std::string>& road, std::vector<Kept>& made, std::vector<std::string>& problems) {
    for (std::size_t at = 0; at < node.size(); ++at) {
        const json::Value& value = node.at(at);

        road.push_back(node.names()[at]);

        const std::string quoted = "\"" + wording::joined(road) + "\"";
        const auto problem = [&](std::string why) { problems.push_back(located(source.name, value.line(), value.column(), quoted + why)); };
        const bool top = road.size() == 1;

        if (top && road.back() == "version") {
            if (value.kind() != json::Kind::integer || value.integer() < 1 || value.integer() > std::numeric_limits<int>::max()) problem(" is the version of the file, a whole number from 1");
        } else if (value.kind() == json::Kind::object) {
            if (value.size() == 0) problem(" holds no settings");
            else kept(source, value, road, made, problems);
        } else if (top && value.kind() == json::Kind::array) {
            bool records = true;

            for (std::size_t item = 0; item < value.size(); ++item) {
                const json::Value& one = value.at(item);

                if (one.kind() != json::Kind::object) records = false;
                else
                    for (std::size_t field = 0; field < one.size(); ++field) records = records && one.at(field).kind() == json::Kind::string;
            }

            if (records) made.push_back(Kept{road, "::tal::Records"});
            else problem(" is a list of records, which are objects of texts");
        } else if (top) {
            problem(" is a section: an object of settings, or a list of records");
        } else if (value.kind() == json::Kind::boolean) {
            made.push_back(Kept{road, "bool"});
        } else if (value.kind() == json::Kind::integer) {
            if (value.integer() >= std::numeric_limits<int>::min() && value.integer() <= std::numeric_limits<int>::max()) made.push_back(Kept{road, "int"});
            else problem(" does not fit an int");
        } else if (value.kind() == json::Kind::number) {
            problem(" has a fraction; a setting holds whole numbers");
        } else if (value.kind() == json::Kind::string) {
            made.push_back(Kept{road, "::std::string"});
        } else if (value.kind() == json::Kind::array) {
            bool texts = true;

            for (std::size_t item = 0; item < value.size(); ++item) texts = texts && value.at(item).kind() == json::Kind::string;

            if (texts) made.push_back(Kept{road, "::std::vector<::std::string>"});
            else problem(" is a list of something else than texts");
        } else {
            problem(" is null, which has no type");
        }

        road.pop_back();
    }
}
}

// The header and the source of the texts of `english` in the namespace `space`, the source
// including the header by the name `header`. English is checked first (`wording::entries`), then
// the names: each must be usable, and no two may become one C++ name (`a-b` and `a_b`), nor take
// the place of `dictionary()`. A text takes at most nine values. With any problem nothing is made.
// The source holds English itself, compact, as bytes, which no compiler limits as it does string
// literals, and the keys of the texts in their order.
Made generated(const Source& english, std::string_view space, std::string_view header, std::vector<std::string>& problems) {
    const std::size_t before = problems.size();
    std::vector<std::string> parts;

    if (!spaced(space, parts)) {
        problems.push_back("tal-texts: \"" + std::string{space} + "\" cannot be the namespace of the texts");

        return {};
    }

    json::Value document;

    if (!read(english, document, problems)) return {};

    std::vector<wording::Problem> shape;
    const std::vector<wording::Entry> entries = wording::entries(document, shape);

    for (const wording::Problem& one : shape) problems.push_back(located(english.name, one.line, one.column, one.why));

    std::vector<std::vector<std::string>> roads;

    for (const wording::Entry& one : entries) roads.push_back(one.road);

    const std::vector<std::vector<std::string>> names = named(english, document, roads, space, "dictionary", problems);

    for (std::size_t at = 0; at < entries.size(); ++at) {
        if (entries[at].slots > static_cast<int>(std::size(ordinals))) {
            const json::Value& value = *entries[at].english;

            problems.push_back(located(english.name, value.line(), value.column(),
                                       "\"" + wording::joined(entries[at].road) + "\" takes more than " + std::to_string(std::size(ordinals)) + " values"));
        }
    }

    if (problems.size() != before) return {};

    Made made;

    made.header = "#pragma once\n\n#include <cstdint>\n#include <string>\n#include <string_view>\n\n#include \"tal/texts.hpp\"\n\nnamespace " +
                  std::string{space} + " {\n\n[[nodiscard]] ::tal::Texts& dictionary();\n\n}\n" + functions(entries, names, space, false);

    const std::string bytes = byted(json::written(document));
    std::string keys;

    for (const wording::Entry& one : entries) keys += "    \"" + wording::joined(one.road) + "\",\n";

    made.code = "#include \"" + std::string{header} + "\"\n\nnamespace {\n\nconstexpr unsigned char tal_texts_english[] = {\n" + bytes +
                "};\n\nconstexpr ::std::string_view tal_texts_keys[] = {\n" + keys + "};\n\n}\n\nnamespace " + std::string{space} +
                " {\n\n::tal::Texts& dictionary() {\n    static ::tal::Texts made{::tal_texts_keys, ::std::string_view{reinterpret_cast<const char*>(::tal_texts_english), "
                "sizeof ::tal_texts_english}};\n\n    return made;\n}\n\n}\n" +
                functions(entries, names, space, true);

    return made;
}

// The problems of the texts: of English, and of every language of `languages` beside it, each file
// named by the code of its language ("pl-PL.json"). Empty when all is well.
std::vector<std::string> checked(const Source& english, const std::vector<Source>& languages) {
    std::vector<std::string> problems;
    json::Value base;

    if (!read(english, base, problems)) return problems;

    std::vector<wording::Problem> shape;

    (void)wording::entries(base, shape);

    for (const wording::Problem& one : shape) problems.push_back(located(english.name, one.line, one.column, one.why));

    if (!problems.empty()) return problems;

    for (const Source& one : languages) {
        std::string_view code = base_name(one.name);

        if (code.ends_with(".json")) code.remove_suffix(5);

        if (!wording::code(code)) {
            problems.push_back(one.name + ": the name of the file is not the code of a language");

            continue;
        }

        json::Value given;

        if (!read(one, given, problems)) continue;

        for (const wording::Problem& problem : wording::compared(given, base, text::Language{code}))
            problems.push_back(located(one.name, problem.line, problem.column, problem.why));
    }

    return problems;
}

// The header and the source of the settings of `defaults`, the built-in values, in the namespace
// `space`, the source including the header by the name `header`: a handle with its type for each
// setting (`kept`), named as the texts are (`named`), and the function `builtin()`, which gives the
// built-in values, compact. With any problem nothing is made.
Made settled(const Source& defaults, std::string_view space, std::string_view header, std::vector<std::string>& problems) {
    const std::size_t before = problems.size();
    std::vector<std::string> parts;

    if (!spaced(space, parts)) {
        problems.push_back("tal-settings: \"" + std::string{space} + "\" cannot be the namespace of the settings");

        return {};
    }

    json::Value document;

    if (!read(defaults, document, problems)) return {};

    if (document.kind() != json::Kind::object) {
        problems.push_back(located(defaults.name, document.line(), document.column(), "the settings are not an object"));

        return {};
    }

    std::vector<Kept> settings;
    std::vector<std::string> road;

    kept(defaults, document, road, settings, problems);

    std::vector<std::vector<std::string>> roads;

    for (const Kept& one : settings) roads.push_back(one.road);

    const std::vector<std::vector<std::string>> names = named(defaults, document, roads, space, "builtin", problems);

    if (problems.size() != before) return {};

    Made made;

    made.header = "#pragma once\n\n#include <string>\n#include <string_view>\n#include <vector>\n\n#include \"tal/settings.hpp\"\n\nnamespace " +
                  std::string{space} + " {\n\n[[nodiscard]] ::std::string_view builtin();\n\n}\n" + blocks(names, space, [&](std::size_t at) {
                      return "inline constexpr ::tal::Setting<" + settings[at].type + "> " + names[at].back() + "{\"" + wording::joined(settings[at].road) +
                             "\"};\n";
                  });

    made.code = "#include \"" + std::string{header} + "\"\n\nnamespace {\n\nconstexpr unsigned char tal_settings_builtin[] = {\n" +
                byted(json::written(document)) + "};\n\n}\n\nnamespace " + std::string{space} +
                " {\n\n::std::string_view builtin() { return {reinterpret_cast<const char*>(::tal_settings_builtin), sizeof ::tal_settings_builtin}; }\n\n}\n";

    return made;
}

}
