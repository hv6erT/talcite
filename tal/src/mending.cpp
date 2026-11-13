#include "mending.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tal {

// The settings text after a person's hand edits, corrected where this can be done without guessing
// what they meant:
//
// - comments // and /* */ outside strings disappear;
// - a comma before } or ] disappears;
// - a cut-off end is closed: the string, then the brackets in reverse order. A pair cut off before
//   its value, or in the middle of the word true, false or null, is dropped whole; we never make up
//   a value.
//
// What cannot be fixed this way the JSON reader will not read, and the repair ends in a refusal.
// The BOM is left alone: the reader skips it by itself, so a file with a BOM is not damaged.
//
// The state is a stack of frames, each knowing what it waits for now: a key, a value or the next
// item. The colon has no state of its own: after a key the frame waits for a value, colon or not.
std::string mended(std::string_view given) {
    enum class Want : std::uint8_t { key, value, next };

    struct Frame {
        char open;
        Want want;
        std::size_t mark;
    };

    std::string made;
    std::vector<Frame> frames;

    bool quoted = false;
    bool escaped = false;
    std::size_t word = std::string::npos;

    // The start of a value: in an object a pair starts with its key (the mark is there), in an
    // array with the value itself.
    const auto valued = [&frames, &made] {
        if (frames.empty()) return;

        Frame& top = frames.back();

        if (top.open == '[' && top.want == Want::value) top.mark = made.size();

        top.want = Want::next;
    };

    const auto dropped_comma = [&made] {
        while (!made.empty() && std::isspace(static_cast<unsigned char>(made.back())) != 0) made.pop_back();

        if (!made.empty() && made.back() == ',') made.pop_back();
    };

    for (std::size_t at = 0; at < given.size(); ++at) {
        const char one = given[at];

        if (quoted) {
            made += one;

            if (escaped) escaped = false;
            else if (one == '\\') escaped = true;
            else if (one == '"') quoted = false;

            continue;
        }

        if (one == '/' && at + 1 < given.size() && given[at + 1] == '/') {
            while (at < given.size() && given[at] != '\n') ++at;

            continue;
        }

        if (one == '/' && at + 1 < given.size() && given[at + 1] == '*') {
            const std::size_t end = given.find("*/", at + 2);

            if (end == std::string_view::npos) break;

            at = end + 1;

            continue;
        }

        const bool letter = std::isalnum(static_cast<unsigned char>(one)) != 0 || one == '-' || one == '+' || one == '.';

        if (!letter) word = std::string::npos;

        switch (one) {
            case '{':
            case '[': {
                const std::size_t mark = made.size();

                valued();

                frames.push_back(Frame{one, one == '{' ? Want::key : Want::value, mark});

                break;
            }
            case '}':
            case ']':
                dropped_comma();

                if (!frames.empty()) frames.pop_back();

                break;
            case ',':
                if (!frames.empty()) frames.back().want = frames.back().open == '{' ? Want::key : Want::value;

                break;
            case '"':
                quoted = true;

                if (!frames.empty() && frames.back().open == '{' && frames.back().want == Want::key) {
                    frames.back().mark = made.size();
                    frames.back().want = Want::value;
                } else {
                    valued();
                }

                break;
            default:
                if (letter && word == std::string::npos) {
                    word = made.size();

                    valued();
                }
        }

        made += one;
    }

    if (quoted) {
        if (escaped) made.pop_back();

        made += '"';
    }

    // A word cut off in the middle (tru, fals, nul) is not a value.
    bool torn = false;

    if (word != std::string::npos)
        for (const std::string_view whole : {"true", "false", "null"}) {
            const std::string_view tail = std::string_view{made}.substr(word);

            if (tail.size() < whole.size() && whole.starts_with(tail)) torn = true;
        }

    while (!frames.empty()) {
        const Frame top = frames.back();

        // A pair without a value (a key without a colon or without a value) and a cut-off word are
        // dropped.
        if (torn || (top.open == '{' && top.want == Want::value)) made.resize(top.mark);

        torn = false;

        dropped_comma();

        made += top.open == '{' ? '}' : ']';

        frames.pop_back();

        if (!frames.empty()) frames.back().want = Want::next;
    }

    return made;
}

}
