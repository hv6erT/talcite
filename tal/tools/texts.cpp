#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "generating.hpp"

// tal-texts, the generator of a program's texts, which the build runs (`tal_texts` in CMake):
//
//   tal-texts generate <en-US.json> <namespace> <header> <source>
//   tal-texts check <stamp> <en-US.json> <language.json>...
//
// The first makes the C++ functions of the texts, the second checks every language against
// English and, when all is well, touches the stamp, which tells the build the check is done. A
// problem goes to the standard error, and the exit code is 1.

namespace {

// The whole file `name`, or none when it cannot be read.
std::optional<std::string> slurped(const std::string& name) {
    std::ifstream file{name, std::ios::binary};

    if (!file) return std::nullopt;

    std::string made{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};

    if (file.bad()) return std::nullopt;

    return made;
}

// Writes `text` to the file `name`, whole; false when it cannot.
bool spilled(const std::string& name, const std::string& text) {
    std::ofstream file{name, std::ios::binary | std::ios::trunc};

    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();

    return static_cast<bool>(file);
}

// The source `name` read whole, or none, with a word to the standard error.
std::optional<tal::generating::Source> source(const std::string& name) {
    std::optional<std::string> text = slurped(name);

    if (!text) {
        std::fprintf(stderr, "tal-texts: cannot read %s\n", name.c_str());

        return std::nullopt;
    }

    return tal::generating::Source{name, std::move(*text)};
}

// Writes `problems` to the standard error and gives the exit code: 1 when there are any.
int told(const std::vector<std::string>& problems) {
    for (const std::string& one : problems) std::fprintf(stderr, "%s\n", one.c_str());

    return problems.empty() ? 0 : 1;
}

// The base name of the path `path`.
std::string base_name(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");

    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Makes the header and the source of the texts.
int generate(const std::vector<std::string>& given) {
    const std::optional<tal::generating::Source> english = source(given[0]);

    if (!english) return 1;

    std::vector<std::string> problems;
    const tal::generating::Made made = tal::generating::generated(*english, given[1], base_name(given[2]), problems);

    if (told(problems) != 0) return 1;

    for (const auto& [name, text] : {std::pair{given[2], made.header}, std::pair{given[3], made.code}})
        if (!spilled(name, text)) {
            std::fprintf(stderr, "tal-texts: cannot write %s\n", name.c_str());

            return 1;
        }

    return 0;
}

// Checks the languages against English and touches the stamp.
int check(const std::vector<std::string>& given) {
    const std::optional<tal::generating::Source> english = source(given[1]);

    if (!english) return 1;

    std::vector<tal::generating::Source> languages;

    for (std::size_t at = 2; at < given.size(); ++at) {
        if (given[at] == given[1]) continue;

        std::optional<tal::generating::Source> one = source(given[at]);

        if (!one) return 1;

        languages.push_back(std::move(*one));
    }

    if (told(tal::generating::checked(*english, languages)) != 0) return 1;

    if (!spilled(given[0], "")) {
        std::fprintf(stderr, "tal-texts: cannot write %s\n", given[0].c_str());

        return 1;
    }

    return 0;
}

}

// Runs the command its arguments name.
int main(int count, char** arguments) {
    const std::vector<std::string> given(arguments + (count > 2 ? 2 : count), arguments + count);
    const std::string command = count > 1 ? arguments[1] : "";

    if (command == "generate" && given.size() == 4) return generate(given);

    if (command == "check" && given.size() >= 2) return check(given);

    std::fprintf(stderr, "usage: tal-texts generate <en-US.json> <namespace> <header> <source>\n"
                         "       tal-texts check <stamp> <en-US.json> <language.json>...\n");

    return 2;
}
