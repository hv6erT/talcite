#include "files.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <utility>

// The files of the generators of texts and settings: what they read and write, and how they say
// what went wrong, on the standard error, as compilers do.
namespace tal::generating {

// The file `name` read whole as a source, or none, with a word from `program` to the standard
// error.
std::optional<Source> sourced(std::string_view program, const std::string& name) {
    std::ifstream file{name, std::ios::binary};
    std::string made;

    if (file) made.assign(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});

    if (!file.is_open() || file.bad()) {
        std::fprintf(stderr, "%.*s: cannot read %s\n", static_cast<int>(program.size()), program.data(), name.c_str());

        return std::nullopt;
    }

    return Source{name, std::move(made)};
}

// Writes `text` to the file `name`, whole; false when it cannot.
bool spilled(const std::string& name, const std::string& text) {
    std::ofstream file{name, std::ios::binary | std::ios::trunc};

    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    file.close();

    return static_cast<bool>(file);
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

// Writes the header and the source of `made` to `header` and `code`, and gives the exit code, with
// a word from `program` when a file cannot be written.
int stored(std::string_view program, const Made& made, const std::string& header, const std::string& code) {
    for (const auto& [name, text] : {std::pair{header, made.header}, std::pair{code, made.code}})
        if (!spilled(name, text)) {
            std::fprintf(stderr, "%.*s: cannot write %s\n", static_cast<int>(program.size()), program.data(), name.c_str());

            return 1;
        }

    return 0;
}

}
