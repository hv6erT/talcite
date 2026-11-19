#include "locating.hpp"

#include <algorithm>
#include <utility>

#include "hexing.hpp"

// Files named in text: as file URIs, which the clipboard carries in text/uri-list, and as what a
// terminal pastes when files are dropped on its window. Terminals write the paths for a shell:
// GNOME Terminal, Konsole and kitty in single quotes, iTerm2 with a backslash before a space or
// another sign the shell reads, Windows Terminal in double quotes when a path holds a space, some
// as file URIs, some a path to a line. The reading is a shell's quoting and nothing more: nothing
// is expanded, and text a shell would read otherwise (a control character, a quote left open) is
// no list of files, but text pasted.
namespace tal::locating {

namespace {

#ifdef _WIN32
constexpr bool windows_here = true;
#else
constexpr bool windows_here = false;
#endif

// Whether a byte goes into a file URI as it is: the unreserved characters of RFC 3986 and the
// slash.
[[nodiscard]] bool plain(unsigned char one) {
    return (one >= 'a' && one <= 'z') || (one >= 'A' && one <= 'Z') || (one >= '0' && one <= '9') || one == '-' || one == '.' ||
           one == '_' || one == '~' || one == '/';
}

// Whether the character parts words.
[[nodiscard]] bool blank(char one) { return one == ' ' || one == '\t' || one == '\n' || one == '\r'; }

// Whether the character is one no dropped name carries: a control character other than the blanks,
// or DEL.
[[nodiscard]] bool foreign(char one) {
    const auto code = static_cast<unsigned char>(one);

    return (code < 0x20 || code == 0x7F) && !blank(one);
}

// The file a word names: a file URI's local path, or the word as a path; nothing when it is not
// absolute, or on Windows when it lies on another machine (\\server\share): asking whether it is
// there would open a connection to that machine and give it the user's login, so a text pasted
// from anywhere could send it anywhere.
[[nodiscard]] std::optional<Path> named(std::string_view word) {
    std::optional<Path> made = word.starts_with("file:") ? unlocated(word) : std::optional<Path>{Path{word}};

    if (!made || !made->absolute()) return std::nullopt;

    if (windows_here && made->stored().starts_with("//")) return std::nullopt;

    return made;
}

// The files of `each`, every one of them absolute; nothing when one is not, or there are none.
[[nodiscard]] std::optional<std::vector<Path>> all_named(const std::vector<std::string>& each) {
    std::vector<Path> made;

    for (const std::string& word : each) {
        std::optional<Path> one = named(word);

        if (!one) return std::nullopt;

        made.push_back(std::move(*one));
    }

    if (made.empty()) return std::nullopt;

    return made;
}

// The lines of the text, blanks at their ends taken away (a terminal that writes a path to a
// line may add a space after it) and empty lines left out.
[[nodiscard]] std::vector<std::string> lines(std::string_view text) {
    std::vector<std::string> made;

    while (!text.empty()) {
        const std::size_t end = std::min(text.find('\n'), text.size());
        std::string_view line = text.substr(0, end);

        text.remove_prefix(std::min(end + 1, text.size()));

        while (!line.empty() && blank(line.back())) line.remove_suffix(1);

        if (!line.empty()) made.emplace_back(line);
    }

    return made;
}

}

// An absolute path as a file URI with an empty host, every other byte as %XX.
std::string located(const Path& where) {
    std::string made = "file://";

    for (const char each : where.stored()) {
        const auto one = static_cast<unsigned char>(each);

        if (plain(one)) made.push_back(each);
        else hex::escaped(one, made);
    }

    return made;
}

// The local path of a file URI (file:///path or file://localhost/path), its escapes decoded;
// nothing for another host or scheme, a broken escape or an escaped NUL.
std::optional<Path> unlocated(std::string_view uri) {
    constexpr std::string_view scheme = "file://";
    constexpr std::string_view local = "localhost";

    if (!uri.starts_with(scheme)) return std::nullopt;

    uri.remove_prefix(scheme.size());

    if (uri.starts_with(local)) uri.remove_prefix(local.size());

    if (uri.empty() || uri.front() != '/') return std::nullopt;

    std::string made;

    for (std::size_t at = 0; at < uri.size(); ++at) {
        if (uri[at] != '%') {
            made.push_back(uri[at]);

            continue;
        }

        if (at + 2 >= uri.size()) return std::nullopt;

        const int high = hex::digit(uri[at + 1]);
        const int low = hex::digit(uri[at + 2]);

        if (high < 0 || low < 0 || (high == 0 && low == 0)) return std::nullopt;

        made.push_back(static_cast<char>((high << 4) | low));
        at += 2;
    }

    return Path::restored(made);
}

// The local files of a text/uri-list (RFC 2483): a URI to a line ended by CR LF or LF; lines naming
// no local file, comments after # among them, are left out.
std::vector<Path> listed(std::string_view text) {
    std::vector<Path> made;

    while (!text.empty()) {
        const std::size_t end = std::min(text.find('\n'), text.size());
        std::string_view line = text.substr(0, end);

        text.remove_prefix(std::min(end + 1, text.size()));

        while (!line.empty() && (line.back() == '\r' || line.back() == '\0')) line.remove_suffix(1);

        if (std::optional<Path> one = unlocated(line)) made.push_back(std::move(*one));
    }

    return made;
}

// The words of the text as a shell parts and unquotes them: blanks between words, single quotes
// taking all to the next one as it is, double quotes taking all to the next one but a backslash
// before ", \, $ or `, and a backslash before any other character taking it as it is (before a
// line break, LF, CR LF or CR alone, taking it away). Pieces next to one another make one word, so
// 'it'\''s' is it's. On `windows` a backslash is a separator of folders and nothing else, outside
// quotes and in them, and a single quote is a letter, as Windows Terminal writes C:\Users with no
// escape and never quotes so. Nothing for a quote left open, a backslash at the end, a control
// character or a line break inside quotes, which no dropped name holds: an apostrophe on two lines
// is no quote.
std::optional<std::vector<std::string>> words(std::string_view text, bool windows) {
    std::vector<std::string> made;
    std::string word;
    bool begun = false;

    for (std::size_t at = 0; at < text.size(); ++at) {
        const char one = text[at];

        if (foreign(one)) return std::nullopt;

        if (blank(one)) {
            if (begun) made.push_back(std::exchange(word, {}));

            begun = false;

            continue;
        }

        const bool was = begun;

        begun = true;

        if (one == '\'' && !windows) {
            const std::size_t end = text.find('\'', at + 1);

            if (end == std::string_view::npos) return std::nullopt;

            const std::string_view inside = text.substr(at + 1, end - at - 1);

            if (std::ranges::any_of(inside, [](char each) { return foreign(each) || each == '\n' || each == '\r'; })) return std::nullopt;

            word += inside;
            at = end;
        } else if (one == '"') {
            for (++at;; ++at) {
                if (at >= text.size() || foreign(text[at]) || text[at] == '\n' || text[at] == '\r') return std::nullopt;

                if (text[at] == '"') break;

                const bool escaping = !windows && text[at] == '\\' && at + 1 < text.size() &&
                                      (text[at + 1] == '"' || text[at + 1] == '\\' || text[at + 1] == '$' || text[at + 1] == '`');

                if (escaping) ++at;

                word += text[at];
            }
        } else if (one == '\\' && !windows) {
            if (at + 1 >= text.size() || foreign(text[at + 1])) return std::nullopt;

            ++at;

            if (text[at] == '\r' && at + 1 < text.size() && text[at + 1] == '\n') ++at;

            if (text[at] == '\n' || text[at] == '\r') begun = was;
            else word += text[at];
        } else {
            word += one;
        }
    }

    if (begun) made.push_back(std::move(word));

    return made;
}

// The files a drop on the terminal pasted: the words of the text (see `words`) when each names an
// absolute path or a local file URI, or else the lines when each does, as a terminal that writes a
// path to a line leaves a space in a name unquoted. Nothing when neither reading names only files;
// whether they are there is for the caller to ask.
std::optional<std::vector<Path>> dropped(std::string_view text) {
    if (const std::optional<std::vector<std::string>> each = words(text, windows_here)) {
        if (std::optional<std::vector<Path>> made = all_named(*each)) return made;
    }

    if (std::ranges::any_of(text, foreign)) return std::nullopt;

    return all_named(lines(text));
}

}
