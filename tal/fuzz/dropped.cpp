#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tal/path.hpp"

#include "locating.hpp"

// What a terminal pastes when files are dropped on it, from libFuzzer: the input read as words,
// both as a shell does and as Windows Terminal writes, and as dropped files, may never crash, hang
// or read out of bounds; no word holds a line break, and every file named is an absolute path.
// Then the input, cut at NUL into names without control characters, is written as a shell would
// quote them (single quotes, and double quotes for Windows) and must read back as the same names.
namespace {

// The names of the input: pieces between NUL bytes, every control character but the tab left out.
std::vector<std::string> named(std::string_view given) {
    std::vector<std::string> made(1);

    for (const char one : given) {
        const auto code = static_cast<unsigned char>(one);

        if (code == 0) made.emplace_back();
        else if (code == '\t' || (code >= 0x20 && code != 0x7F)) made.back() += one;
    }

    return made;
}

// The names in single quotes, a quote inside as '\'' , parted by spaces.
std::string quoted(const std::vector<std::string>& names) {
    std::string made;

    for (const std::string& one : names) {
        made += '\'';

        for (const char each : one) {
            if (each == '\'') made += "'\\''";
            else made += each;
        }

        made += "' ";
    }

    return made;
}

// The names in double quotes as Windows Terminal writes them, parted by spaces; nothing when a
// name holds a double quote, which a name on Windows cannot.
std::optional<std::string> windowed(const std::vector<std::string>& names) {
    std::string made;

    for (const std::string& one : names) {
        if (one.find('"') != std::string::npos) return std::nullopt;

        made += '"' + one + "\" ";
    }

    return made;
}

// Whether no word holds a line break.
bool unbroken(const std::optional<std::vector<std::string>>& words) {
    if (!words) return true;

    for (const std::string& one : *words)
        if (one.find_first_of("\r\n") != std::string::npos) return false;

    return true;
}

}

// One input of libFuzzer (see above).
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text{reinterpret_cast<const char*>(data), size};

    if (!unbroken(tal::locating::words(text, false)) || !unbroken(tal::locating::words(text, true))) std::terminate();

    if (const std::optional<std::vector<tal::Path>> files = tal::locating::dropped(text)) {
        if (files->empty()) std::terminate();

        for (const tal::Path& one : *files)
            if (!one.absolute()) std::terminate();
    }

    const std::vector<std::string> names = named(text);

    if (tal::locating::words(quoted(names), false) != names) std::terminate();

    if (const std::optional<std::string> written = windowed(names)) {
        if (tal::locating::words(*written, true) != names) std::terminate();
    }

    return 0;
}
