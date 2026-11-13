#include "tal/texts.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

#include "tal/bytes.hpp"
#include "tal/executor.hpp"
#include "tal/fs.hpp"
#include "tal/json.hpp"
#include "tal/text.hpp"
#include "tal/trouble.hpp"

#include "wording.hpp"

// The dictionary of a program: English underneath, compiled in by the generator of texts, and the
// chosen language over it, read from a file. A translation from outside the program is hostile
// text: the JSON reader bounds it, and every text of it that cannot stand for the English one stays
// English, so that no value is lost and nothing throws where the program asks for a text.
namespace tal {

namespace {

// The code of English, the language underneath.
constexpr std::string_view base = "en-US";

// The ending of a language's file.
constexpr std::string_view ending = ".json";

// The road of the key `key`, split at its points.
std::vector<std::string> split(std::string_view key) {
    std::vector<std::string> made;

    for (std::size_t from = 0;;) {
        const std::size_t to = key.find('.', from);

        made.emplace_back(key.substr(from, to == std::string_view::npos ? std::string_view::npos : to - from));

        if (to == std::string_view::npos) return made;

        from = to + 1;
    }
}

// The names of the plural forms, in the order of `text::Plural`.
constexpr std::string_view plural_names[] = {"zero", "one", "two", "few", "many", "other"};

// The index of the plural form named `name` in the order of `text::Plural`.
std::size_t form_of(std::string_view name) {
    return static_cast<std::size_t>(std::find(std::begin(plural_names), std::end(plural_names), name) - std::begin(plural_names));
}

// The codes of the languages whose files lie in `folders`, each once, sorted, with English among
// them even without a file. A folder that cannot be listed adds nothing, and a file whose name is
// not a code is no language.
Task<std::vector<std::string>> listed(std::vector<Path> folders) {
    std::vector<std::string> made{std::string{base}};

    for (const Path& folder : folders) {
        std::vector<fs::Entry> found;

        try {
            found = co_await fs::list(folder);
        } catch (const Trouble&) {
            continue;
        }

        for (const fs::Entry& one : found) {
            const std::string_view name = one.where.name();

            if (one.reach != fs::Type::file || !name.ends_with(ending)) continue;

            const std::string_view code = name.substr(0, name.size() - ending.size());

            if (wording::code(code)) made.emplace_back(code);
        }
    }

    std::sort(made.begin(), made.end());
    made.erase(std::unique(made.begin(), made.end()), made.end());

    co_return made;
}

// The text of the file of the language `code` in the first of `folders` that has one, at most one
// byte longer than JSON may be, or none when no folder has it or the first one found cannot be
// read.
Task<std::optional<std::string>> fetched(std::vector<Path> folders, std::string code) {
    const std::uint64_t most = json::Limits{}.bytes + 1;

    for (const Path& folder : folders) {
        try {
            co_return text::of(co_await fs::read(folder / (code + std::string{ending}), fs::Range{0, most}));
        } catch (const Trouble& why) {
            if (why.kind() != Kind::missing) co_return std::nullopt;
        }
    }

    co_return std::nullopt;
}

}

// The dictionary with the English texts `english` and their keys `keys`, both from the generator
// of texts: the keys name the texts in the order of the English file, and the generated functions
// ask for a text by its place among them. English that does not match its keys, or does not have
// the shape of texts, is a mistake of the program, not of its user.
Texts::Texts(std::span<const std::string_view> keys, std::string_view english) : code_{base} {
    const json::Value document = json::parsed(english);
    std::vector<wording::Problem> problems;
    const std::vector<wording::Entry> entries = wording::entries(document, problems);

    if (!problems.empty()) throw std::logic_error{"tal::Texts: the English texts are broken: " + problems.front().why};

    if (entries.size() != keys.size()) throw std::logic_error{"tal::Texts: the keys do not match the English texts"};

    for (std::size_t at = 0; at < keys.size(); ++at) {
        const wording::Entry& entry = entries[at];

        if (wording::joined(entry.road) != keys[at]) throw std::logic_error{"tal::Texts: the keys do not match the English texts"};

        Said one;

        one.counted = entry.shape == wording::Shape::counted;
        one.letter = entry.shape == wording::Shape::letter;

        if (one.counted)
            for (std::size_t form = 0; form < entry.english->size(); ++form)
                one.forms[form_of(entry.english->names()[form])] = entry.english->at(form).string();
        else
            one.forms.back() = entry.english->string();

        keys_.emplace_back(keys[at]);
        base_.push_back(std::move(one));
    }

    name_ = wording::found(document, {"meta", "name"})->string();
    said_ = base_;
}

// Reads the language `code` over English: from the first of `folders` that has its file, so that
// the user's folder, given first, beats the program's. "auto" is the system's language, matched
// against the files (`nearest`), or English. A code that cannot name a file, a file that is
// missing, cannot be read or is not JSON leaves English alone, and `code` says en-US; a file of
// English itself goes over the English compiled in. The pool reads and compares, and the dictionary
// changes at once at the end, where the caller is, so that no text is half old.
Task<void> Texts::read(std::vector<Path> folders, std::string code) {
    Executor& back = co_await detail::Here{};

    co_await to(pool());

    if (code == "auto") {
        const std::optional<text::Language> system = text::proposed();
        const std::string found = system ? nearest(system->tag(), co_await listed(folders)) : std::string{};

        code = found.empty() ? std::string{base} : found;
    }

    std::vector<Said> made = base_;
    std::string spoken{base};

    if (wording::code(code)) {
        if (const std::optional<std::string> given = co_await fetched(folders, code)) {
            try {
                const json::Value document = json::parsed(*given);
                const text::Language language{code};

                if (document.kind() == json::Kind::object) {
                    for (std::size_t at = 0; at < keys_.size(); ++at) {
                        const Said& english = base_[at];
                        const wording::Shape shape = english.counted  ? wording::Shape::counted
                                                     : english.letter ? wording::Shape::letter
                                                                      : wording::Shape::text;
                        const wording::Entry entry{split(keys_[at]), shape, wording::slots(english.forms.back()), nullptr};
                        const json::Value* found = wording::found(document, entry.road);

                        if (found == nullptr || !wording::unfit(entry, *found, language).empty()) continue;

                        Said one = english;

                        one.forms = {};
                        one.english = false;

                        if (one.counted)
                            for (const text::Plural form : text::plurals(language)) {
                                const std::size_t index = static_cast<std::size_t>(form);

                                one.forms[index] = found->find(plural_names[index])->string();
                            }
                        else
                            one.forms.back() = found->string();

                        made[at] = std::move(one);
                    }

                    spoken = code;
                }
            } catch (const Trouble&) {
            }
        }
    }

    co_await to(back);

    said_ = std::move(made);
    code_ = std::move(spoken);
}

// The text under `key`, in the chosen language or in English. Valid until the next `read` ends.
const std::string& Texts::said(std::size_t key) const {
    const Said& one = said_.at(key);

    if (one.counted) throw std::logic_error{"tal::Texts::said: the text is for a number; ask counted"};

    return one.forms.back();
}

// The text under `key` for the number `many`: its plural form in the language the text came from,
// so that a text English stood in for takes the English form.
const std::string& Texts::counted(std::size_t key, std::uint64_t many) const {
    const Said& one = said_.at(key);

    if (!one.counted) throw std::logic_error{"tal::Texts::counted: the text is not for a number; ask said"};

    return one.forms[static_cast<std::size_t>(text::plural(many, text::Language{one.english ? base : std::string_view{code_}}))];
}

// The letter of the key under `key`, which the dictionary made sure is one character.
char32_t Texts::letter(std::size_t key) const {
    const Said& one = said_.at(key);

    if (!one.letter) throw std::logic_error{"tal::Texts::letter: the text is not the letter of a key"};

    std::size_t at = 0;

    return *text::decoded(one.forms.back(), at);
}

// The languages a program can speak: English first, with the name compiled in, then those with
// files in `folders`, sorted by code, each with its name in its own language (`meta.name`), or an
// empty one when its file has none or cannot be read. The pool reads the files.
Task<std::vector<Spoken>> Texts::languages(std::vector<Path> folders) const {
    Executor& back = co_await detail::Here{};

    co_await to(pool());

    std::vector<Spoken> made{Spoken{std::string{base}, name_}};

    for (std::string& code : co_await listed(folders)) {
        if (code == base) continue;

        Spoken one{code, {}};

        if (const std::optional<std::string> given = co_await fetched(folders, code)) {
            try {
                const json::Value document = json::parsed(*given);
                const json::Value* name = wording::found(document, {"meta", "name"});

                if (name != nullptr && name->kind() == json::Kind::string) one.name = name->string();
            } catch (const Trouble&) {
            }
        }

        made.push_back(std::move(one));
    }

    co_await to(back);

    co_return made;
}

// The language from `known` closest to `wanted`: the same tag, ignoring case, or else the first
// one of the same language in the order of `known`; none gives an empty string. "pl" and "pl-PL"
// find Polish, "en-AU" the first English.
std::string Texts::nearest(std::string_view wanted, const std::vector<std::string>& known) {
    const std::string sought = text::lowered(wanted);

    for (const std::string& one : known)
        if (text::lowered(one) == sought) return one;

    const std::string tongue = text::lowered(wanted.substr(0, wanted.find('-')));

    for (const std::string& one : known)
        if (text::lowered(std::string_view{one}.substr(0, one.find('-'))) == tongue) return one;

    return {};
}

// `said` with each {} filled by one value of `with` in turn, from the left. When values run out the
// {} stays; values left over are lost. The search goes on after a value, so that a {} inside a
// value is not filled.
std::string filled(std::string_view said, std::initializer_list<std::string_view> with) {
    std::string made{said};
    std::size_t from = 0;

    for (const std::string_view one : with) {
        const std::size_t at = made.find("{}", from);

        if (at == std::string::npos) break;

        made.replace(at, 2, one);
        from = at + one.size();
    }

    return made;
}

}
