#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "files.hpp"
#include "generating.hpp"

// tal-settings, the generator of a program's settings, which the build runs (`tal_settings` in
// CMake):
//
//   tal-settings generate <defaults.json> <namespace> <header> <source>
//
// It makes the handles of the settings, with their types, and the function that gives the
// built-in values. A problem goes to the standard error, and the exit code is 1.

namespace {

using namespace tal::generating;

// The name the program speaks by.
constexpr std::string_view program = "tal-settings";

// Makes the header and the source of the settings.
int generate(const std::vector<std::string>& given) {
    const std::optional<Source> defaults = sourced(program, given[0]);

    if (!defaults) return 1;

    std::vector<std::string> problems;
    const Made made = settled(*defaults, given[1], base_name(given[2]), problems);

    if (told(problems) != 0) return 1;

    return stored(program, made, given[2], given[3]);
}

}

// Runs the command its arguments name.
int main(int count, char** arguments) {
    const std::vector<std::string> given(arguments + (count > 2 ? 2 : count), arguments + count);
    const std::string command = count > 1 ? arguments[1] : "";

    if (command == "generate" && given.size() == 4) return generate(given);

    std::fprintf(stderr, "usage: tal-settings generate <defaults.json> <namespace> <header> <source>\n");

    return 2;
}
