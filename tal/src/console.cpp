#include "tal/console.hpp"

#include <utility>

namespace tal {

// A console that plays `said` back as one reply, or as replies of `piece` bytes when `piece` is
// not zero; everything else is set in its fields.
ScriptedConsole::ScriptedConsole(std::string said, std::size_t piece) {
    if (said.empty()) return;

    if (piece == 0) {
        replies.push_back(std::move(said));

        return;
    }

    for (std::size_t at = 0; at < said.size(); at += piece) replies.push_back(said.substr(at, piece));
}

// The variable from `environment`, or nothing.
std::optional<std::string> ScriptedConsole::variable(std::string_view name) const {
    const auto found = environment.find(name);

    if (found == environment.end()) return std::nullopt;

    return found->second;
}

// Counts the nesting of the raw mode in `depth` and every entry in `entered`; leaving below zero
// is ignored.
void ScriptedConsole::raw(bool on) {
    if (on) {
        ++depth;
        ++entered;
    } else if (depth > 0) {
        --depth;
    }
}

// Notes the round and its patience, then gives the next queued reply, or at once nothing when
// none is left: trials do not sit through the patience.
std::string ScriptedConsole::hear(std::optional<std::chrono::milliseconds> waited) {
    ++rounds;

    patience.push_back(waited);

    if (replies.empty()) return {};

    std::string made = std::move(replies.front());

    replies.pop_front();

    return made;
}

// A resume after a stop, told once, as the real console tells it.
bool ScriptedConsole::resumed() {
    const bool made = comeback;

    comeback = false;

    return made;
}

}
