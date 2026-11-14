#include "lining.hpp"

#include <algorithm>
#include <optional>

// The core of editing one line, shared by a field and an editor: what a word is, where a word
// starts and ends, and which steps of the history join, so that both keep the same rules.
namespace tal::lining {

// The byte where `what`, standing at the byte `at` of one line, ends.
std::size_t ended(std::size_t at, std::string_view what) { return at + what.size(); }

// The place where `what`, standing at `at`, ends: its lines are broken at \n.
Place ended(Place at, std::string_view what) {
    const std::size_t last = what.rfind('\n');

    if (last == std::string_view::npos) return Place{at.line, at.byte + what.size()};

    return Place{at.line + static_cast<std::size_t>(std::count(what.begin(), what.end(), '\n')), what.size() - last - 1};
}

// Whether the cluster at the byte `at` of `line` starts with a character of a word; past the end,
// no.
bool lettered(std::string_view line, std::size_t at) {
    std::size_t next = at;
    const std::optional<char32_t> one = text::decoded(line, next);

    return one && text::in_word(*one);
}

// The end of the word after the byte `at` of `line` (`forth`) or the start of the word before it:
// past what is not a word first, then past the word, a cluster at a time; the ends of the line
// stop it.
std::size_t worded(std::string_view line, std::size_t at, bool forth, text::Rendering how) {
    std::size_t made = std::min(at, line.size());

    if (forth) {
        while (made < line.size() && !lettered(line, made)) made = text::after(line, made, how);
        while (made < line.size() && lettered(line, made)) made = text::after(line, made, how);

        return made;
    }

    while (made > 0 && !lettered(line, text::before(line, made, how))) made = text::before(line, made, how);
    while (made > 0 && lettered(line, text::before(line, made, how))) made = text::before(line, made, how);

    return made;
}

}
