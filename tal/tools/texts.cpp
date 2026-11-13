#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "files.hpp"
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

using namespace tal::generating;

// The name the program speaks by.
constexpr std::string_view program = "tal-texts";

// Makes the header and the source of the texts.
int generate(const std::vector<std::string>& given) {
    const std::optional<Source> english = sourced(program, given[0]);

    if (!english) return 1;

    std::vector<std::string> problems;
    const Made made = generated(*english, given[1], base_name(given[2]), problems);

    if (told(problems) != 0) return 1;

    return stored(program, made, given[2], given[3]);
}

// Checks the languages against English and touches the stamp.
int check(const std::vector<std::string>& given) {
    const std::optional<Source> english = sourced(program, given[1]);

    if (!english) return 1;

    std::vector<Source> languages;

    for (std::size_t at = 2; at < given.size(); ++at) {
        if (given[at] == given[1]) continue;

        std::optional<Source> one = sourced(program, given[at]);

        if (!one) return 1;

        languages.push_back(std::move(*one));
    }

    if (told(checked(*english, languages)) != 0) return 1;

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
