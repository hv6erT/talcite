#include "wording.hpp"

#include <algorithm>
#include <map>
#include <utility>

// The shape of a program's texts, shared by the dictionary (`Texts`) and the generator of texts:
// what English holds, and whether a text of another language can stand for an English one. English
// is a tree of objects whose leaves are texts; a leaf `key` beside a leaf `text` is the letter of a
// menu item or a button, and an object whose names are all plural forms is one text for a number.
namespace tal::wording {

namespace {

// The names of the plural forms, in the order of `text::Plural`.
constexpr std::string_view plural_names[] = {"zero", "one", "two", "few", "many", "other"};

// The road `road` in quotes, for a message.
std::string quoted(const std::vector<std::string>& road) { return "\"" + joined(road) + "\""; }

// The name of the plural form `one`.
std::string_view spoken(text::Plural one) { return plural_names[static_cast<std::size_t>(one)]; }

// A problem with `one`, at the place where it stands in its text.
Problem placed(const json::Value& one, std::string why) { return Problem{one.line(), one.column(), std::move(why)}; }

// Whether `given` holds every plural form `language` needs, as texts with `many` {}; empty when it
// does, otherwise why not.
std::string unfit_forms(const std::vector<std::string>& road, const json::Value& given, int many, const text::Language& language) {
    if (given.kind() != json::Kind::object) return quoted(road) + " is not an object of plural forms";

    for (const text::Plural one : text::plurals(language)) {
        const std::string form{spoken(one)};
        const json::Value* found = given.find(form);

        if (found == nullptr) return quoted(road) + " lacks the plural form \"" + form + "\"";

        if (found->kind() != json::Kind::string) return quoted(road) + " has the plural form \"" + form + "\" not as a text";

        if (const int has = slots(found->string()); has != many)
            return quoted(road) + " has " + std::to_string(has) + " {} in the plural form \"" + form + "\" where en-US has " +
                   std::to_string(many);
    }

    return {};
}

// The names in the object of plural forms `given` that are not forms `language` uses, each as a
// problem.
void unused_forms(const std::vector<std::string>& road, const json::Value& given, const text::Language& language, std::vector<Problem>& problems) {
    const std::vector<text::Plural> used = text::plurals(language);

    for (std::size_t at = 0; at < given.size(); ++at) {
        const std::string& name = given.names()[at];

        if (std::none_of(used.begin(), used.end(), [&name](text::Plural one) { return spoken(one) == name; }))
            problems.push_back(placed(given.at(at), quoted(road) + " has \"" + name + "\", which is not a plural form of " +
                                                        std::string{language.tag()}));
    }
}

// The entries under the English object `one` at `road`, in the order of the text, with the
// problems of their shape.
void walked(const json::Value& one, std::vector<std::string>& road, std::vector<Entry>& made, std::vector<Problem>& problems) {
    const text::Language english{"en-US"};

    for (std::size_t at = 0; at < one.size(); ++at) {
        const std::string& name = one.names()[at];
        const json::Value& value = one.at(at);

        road.push_back(name);

        if (value.kind() == json::Kind::string) {
            const json::Value* beside = one.find("text");
            const bool key = name == "key" && beside != nullptr && beside->kind() == json::Kind::string;

            if (key && !letter(value.string())) problems.push_back(placed(value, quoted(road) + " is the letter of a key: one character"));

            made.push_back(Entry{road, key ? Shape::letter : Shape::text, slots(value.string()), &value});
        } else if (value.kind() != json::Kind::object) {
            problems.push_back(placed(value, quoted(road) + " is neither a text nor an object"));
        } else if (value.size() == 0) {
            problems.push_back(placed(value, quoted(road) + " holds no texts"));
        } else if (forms(value)) {
            const json::Value* other = value.find("other");
            const int many = other != nullptr && other->kind() == json::Kind::string ? slots(other->string()) : 0;

            if (const std::string why = unfit_forms(road, value, many, english); !why.empty()) problems.push_back(placed(value, why));

            unused_forms(road, value, english, problems);
            made.push_back(Entry{road, Shape::counted, many, &value});
        } else {
            walked(value, road, made, problems);
        }

        road.pop_back();
    }
}

// The texts `given` of `language` beside the English object `english` at `road`: names English
// does not have, texts it has and `given` lacks, and texts that cannot stand for the English ones.
void matched(const json::Value& english, const json::Value& given, std::vector<std::string>& road, const std::map<std::string, const Entry*>& known,
             const text::Language& language, std::vector<Problem>& problems) {
    if (given.kind() != json::Kind::object) {
        problems.push_back(placed(given, road.empty() ? std::string{"the texts are not an object"} : quoted(road) + " is not an object"));

        return;
    }

    for (std::size_t at = 0; at < given.size(); ++at)
        if (english.find(given.names()[at]) == nullptr) {
            road.push_back(given.names()[at]);
            problems.push_back(placed(given.at(at), quoted(road) + " is not in en-US"));
            road.pop_back();
        }

    for (std::size_t at = 0; at < english.size(); ++at) {
        const json::Value& theirs = english.at(at);

        road.push_back(english.names()[at]);

        if (const json::Value* ours = given.find(road.back()); ours == nullptr) {
            problems.push_back(placed(given, quoted(road) + " is missing"));
        } else if (theirs.kind() == json::Kind::object && !forms(theirs)) {
            matched(theirs, *ours, road, known, language, problems);
        } else if (const auto entry = known.find(joined(road)); entry != known.end()) {
            if (const std::string why = unfit(*entry->second, *ours, language); !why.empty()) problems.push_back(placed(*ours, why));
            else if (entry->second->shape == Shape::counted) unused_forms(road, *ours, language, problems);
        }

        road.pop_back();
    }
}

// The name of the language in `document` (`meta.name`), when it is there as a text but empty, as
// a problem.
void named(const json::Value& document, std::vector<Problem>& problems) {
    const json::Value* name = found(document, {"meta", "name"});

    if (name != nullptr && name->kind() == json::Kind::string && name->string().empty())
        problems.push_back(placed(*name, "\"meta.name\" is empty"));
}

// `problems` in the order of the text.
std::vector<Problem> ordered(std::vector<Problem> problems) {
    std::stable_sort(problems.begin(), problems.end(),
                     [](const Problem& one, const Problem& two) { return std::pair{one.line, one.column} < std::pair{two.line, two.column}; });

    return problems;
}

}

// How many values `said` takes: its {}, counted from the left as `filled` fills them.
int slots(std::string_view said) {
    int made = 0;

    for (std::size_t at = said.find("{}"); at != std::string_view::npos; at = said.find("{}", at + 2)) ++made;

    return made;
}

// Whether `said` is one character: the letter of a key.
bool letter(std::string_view said) {
    std::size_t at = 0;

    return text::decoded(said, at).has_value() && at == said.size();
}

// The road `road` as one key: its names joined by points.
std::string joined(const std::vector<std::string>& road) {
    std::string made;

    for (const std::string& one : road) {
        if (!made.empty()) made += '.';

        made += one;
    }

    return made;
}

// Whether `one` is an object of plural forms: not empty, and every name a plural form.
bool forms(const json::Value& one) {
    if (one.kind() != json::Kind::object || one.size() == 0) return false;

    return std::all_of(one.names().begin(), one.names().end(), [](const std::string& name) {
        return std::find(std::begin(plural_names), std::end(plural_names), name) != std::end(plural_names);
    });
}

// The texts of English in the order of the text, with the problems of their shape: a leaf that is
// not a text, an empty object, a key that is not one letter, plural forms that are not those of
// English, and a name of the language (`meta.name`) that is missing or empty.
std::vector<Entry> entries(const json::Value& english, std::vector<Problem>& problems) {
    std::vector<Entry> made;
    std::vector<Problem> found_now;

    if (english.kind() != json::Kind::object) {
        problems.push_back(placed(english, "the texts are not an object"));

        return made;
    }

    std::vector<std::string> road;

    walked(english, road, made, found_now);

    const json::Value* name = found(english, {"meta", "name"});

    if (name == nullptr) found_now.push_back(placed(english, "\"meta.name\" is missing"));
    else if (name->kind() != json::Kind::string) found_now.push_back(placed(*name, "\"meta.name\" is not a text"));
    else named(english, found_now);

    for (Problem& one : ordered(std::move(found_now))) problems.push_back(std::move(one));

    return made;
}

// The value at `road` in `document`, or none when a step is missing or not an object.
const json::Value* found(const json::Value& document, const std::vector<std::string>& road) {
    const json::Value* here = &document;

    for (const std::string& name : road) {
        if (here->kind() != json::Kind::object) return nullptr;

        here = here->find(name);

        if (here == nullptr) return nullptr;
    }

    return here;
}

// Whether `given`, a value of `language`, can stand for the English `entry`; empty when it can,
// otherwise why not. A text must have as many {} as English, so that no value is lost or left
// over; the letter of a key must be one character; a text for a number needs every plural form of
// its language. Forms `language` does not use do not matter here.
std::string unfit(const Entry& entry, const json::Value& given, const text::Language& language) {
    if (entry.shape == Shape::counted) return unfit_forms(entry.road, given, entry.slots, language);

    if (given.kind() != json::Kind::string) return quoted(entry.road) + " is not a text";

    if (const int has = slots(given.string()); has != entry.slots)
        return quoted(entry.road) + " has " + std::to_string(has) + " {} where en-US has " + std::to_string(entry.slots);

    if (entry.shape == Shape::letter && !letter(given.string())) return quoted(entry.road) + " is the letter of a key: one character";

    return {};
}

// What is wrong with the texts `given` of `language` beside `english`, in the order of the text:
// everything `unfit` finds, and also names English does not have, plural forms the language does
// not use and an empty name of the language. English is taken as it is; `entries` checks it.
std::vector<Problem> compared(const json::Value& given, const json::Value& english, const text::Language& language) {
    std::vector<Problem> problems;
    std::vector<Problem> ignored;
    const std::vector<Entry> all = entries(english, ignored);
    std::map<std::string, const Entry*> known;

    for (const Entry& one : all) known.emplace(joined(one.road), &one);

    std::vector<std::string> road;

    if (english.kind() == json::Kind::object) matched(english, given, road, known, language, problems);

    if (given.kind() == json::Kind::object) named(given, problems);

    return ordered(std::move(problems));
}

// Whether `said` can be the code of a language and the name of its file: a letter, then letters,
// digits and hyphens, at most 35 in all, as BCP 47 tags are. Nothing else may name a file to read.
bool code(std::string_view said) {
    const auto alpha = [](char one) { return (one >= 'a' && one <= 'z') || (one >= 'A' && one <= 'Z'); };

    if (said.empty() || said.size() > 35 || !alpha(said.front())) return false;

    return std::all_of(said.begin(), said.end(), [&alpha](char one) { return alpha(one) || (one >= '0' && one <= '9') || one == '-'; });
}

}
