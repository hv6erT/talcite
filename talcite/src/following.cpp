#include "talcite/following.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Following an address is one thing in the whole library, whoever does it: a link (`Link`), a mark
// with an address in a label (`Span`) or in a page (`Page`). `FollowingMixin` holds it for them:
// `FollowEvent` goes up the tree first, so that the application may do something else with the
// address (show a page of its own, ask first), and whoever takes it takes the address with it.
// Otherwise the launcher opens it (`Launcher::open`): the one the node was given, or the system's
// (`launcher`), in the browser, or onto the clipboard over ssh and without a graphical session.
// When its answer comes, `FollowedEvent` says how it went, so that the application can tell the
// user that the address was copied rather than opened. The answer comes back on the loop of the
// thread that follows (`Promise::here`), which is the loop of the screen; the node holds the way
// there, because a chain nobody holds is given up, so a node that has gone says nothing to anyone.
// Following again before the answer came waits for the new answer only; the address already went
// its way either way.
//
// The mixin lives in its header, as every mixin of the library does (`EnabledMixin`, `MarkedMixin`),
// and so do the two events: they belong to following, not to the event loop.
//
// Here lives what text needs to have its addresses found (`addressed`).
namespace talcite::ui {

namespace {

// Whether the text has the scheme at `at`, letter case aside, as schemes are compared.
bool schemed(std::string_view text, std::size_t at, std::string_view scheme) {
    if (text.size() - at < scheme.size()) return false;

    for (std::size_t one = 0; one < scheme.size(); ++one) {
        char letter = text[at + one];

        if (letter >= 'A' && letter <= 'Z') letter = static_cast<char>(letter - 'A' + 'a');

        if (letter != scheme[one]) return false;
    }

    return true;
}

// Whether a byte belongs to a word, so that an address glued to it ("xhttps://") is not one.
bool wordy(char one) {
    return (one >= 'a' && one <= 'z') || (one >= 'A' && one <= 'Z') || (one >= '0' && one <= '9');
}

// Whether a byte may stand inside an address found in text: not a space or a control character,
// and none of the ASCII marks text puts around an address (quotes, angle brackets, a backtick).
// Bytes of other characters stay, since an address may hold letters of any script; a typographic
// quote glued to its end goes in `trimmed`.
bool inside(char one) {
    const auto byte = static_cast<unsigned char>(one);

    if (byte <= 0x20 || byte == 0x7F) return false;

    return one != '"' && one != '<' && one != '>' && one != '`';
}

// The marks that close a sentence or a quotation, which stand after an address more often than in
// it: the end of an address is taken back past them. Each is spelled in UTF-8.
constexpr std::string_view closing[] = {".", ",", ";", ":", "!", "?", "'", "”", "’", "»", "“", "„"};

// How many times `one` occurs in the text.
std::size_t counted(std::string_view text, char one) { return static_cast<std::size_t>(std::count(text.begin(), text.end(), one)); }

// The end of an address that starts at `from` and could run up to `upto`, taken back past the marks
// of the sentence (`closing`) and past a closing parenthesis or bracket the address did not open:
// "(see https://example.org/a)" ends before the parenthesis, while
// "https://en.wikipedia.org/wiki/Heat_(1995_film)" keeps its own.
std::size_t trimmed(std::string_view text, std::size_t from, std::size_t upto) {
    for (bool shorter = true; shorter && upto > from;) {
        shorter = false;

        const std::string_view now = text.substr(from, upto - from);

        for (const std::string_view mark : closing)
            if (now.ends_with(mark)) {
                upto -= mark.size();
                shorter = true;

                break;
            }

        if (shorter) continue;

        for (const auto& [open, close] : {std::pair{'(', ')'}, std::pair{'[', ']'}})
            if (now.back() == close && counted(now, close) > counted(now, open)) {
                --upto;
                shorter = true;

                break;
            }
    }

    return upto;
}

}

// The addresses in text, as marks of the role `role` that lead to them: every http and https
// address, starting where a word may start and ending where the address does (`inside`, `trimmed`).
// A mark counts its line from the start of the text, and its bytes from the start of the line, as
// labels and pages count them, so the marks go into either as they are. An address of a scheme
// alone ("https://") is no address.
std::vector<Mark> addressed(std::string_view text, const std::string& role) {
    std::vector<Mark> made;

    std::size_t line = 0;
    std::size_t start = 0;

    for (std::size_t at = 0; at < text.size();) {
        if (text[at] == '\n') {
            ++line;
            start = at + 1;
            ++at;

            continue;
        }

        const std::size_t scheme = schemed(text, at, "https://") ? 8 : schemed(text, at, "http://") ? 7 : 0;

        if (scheme == 0 || (at > start && wordy(text[at - 1]))) {
            ++at;

            continue;
        }

        std::size_t end = at;

        while (end < text.size() && inside(text[end])) ++end;

        end = trimmed(text, at, end);

        if (end > at + scheme) {
            Mark one;

            one.line = line;
            one.from = at - start;
            one.upto = end - start;
            one.role = role;
            one.address = std::string{text.substr(at, end - at)};

            made.push_back(std::move(one));
        }

        at = std::max(end, at + scheme);
    }

    return made;
}

}
