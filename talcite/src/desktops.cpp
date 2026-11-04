#include "desktops.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "talcite/ascii.hpp"
#include "talcite/encoding.hpp"

// What the desktop of the system says about files, read from the text and the bytes alone, without
// touching the system: the pieces `desktop()` puts together, kept apart so that the trials check
// them on written fixtures instead of on whatever the machine running them has installed.
//
// On Linux three specifications of freedesktop.org meet here. The shared MIME-info database gives
// the type of a file by its name (globs2) and the types it descends from (subclasses). The MIME
// applications associations give the programs of a type (mimeapps.list in the order of the XDG
// directories, and mimeinfo.cache of each applications folder). The Desktop Entry specification
// says what a program is called, how its command line is written and whether it needs a terminal.
// Windows contributes two small readers: the subsystem of an executable, from its PE header, and
// the zone a downloaded file came from, from its Zone.Identifier stream.
namespace talcite::desktops {

namespace {

// Lower case is ASCII only, so a UTF-8 name is compared byte for byte outside ASCII, as
// shared-mime-info itself does.
using ascii::lowered;
using ascii::trimmed;

// The lines of the text, without their line ends.
std::vector<std::string_view> lines_of(std::string_view text) {
    std::vector<std::string_view> made;

    while (!text.empty()) {
        const std::size_t end = text.find('\n');

        made.push_back(text.substr(0, end));

        if (end == std::string_view::npos) break;

        text.remove_prefix(end + 1);
    }

    return made;
}

// The parts of the text between the separator, empty parts left out.
std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> made;

    while (!text.empty()) {
        const std::size_t end = text.find(separator);
        const std::string_view part = trimmed(text.substr(0, end));

        if (!part.empty()) made.emplace_back(part);

        if (end == std::string_view::npos) break;

        text.remove_prefix(end + 1);
    }

    return made;
}

// Whether the pattern's character class starting at `at` (just after '[') takes the byte; `at` ends
// just after the closing ']'. A class with no closing bracket takes nothing.
bool classed(std::string_view pattern, std::size_t& at, char one) {
    bool negated = false;

    if (at < pattern.size() && (pattern[at] == '!' || pattern[at] == '^')) {
        negated = true;
        ++at;
    }

    bool taken = false;
    bool first = true;

    while (at < pattern.size() && (first || pattern[at] != ']')) {
        first = false;

        const char low = pattern[at];

        if (at + 2 < pattern.size() && pattern[at + 1] == '-' && pattern[at + 2] != ']') {
            if (one >= low && one <= pattern[at + 2]) taken = true;

            at += 3;
        } else {
            if (one == low) taken = true;

            ++at;
        }
    }

    if (at >= pattern.size()) return false;

    ++at;

    return taken != negated;
}

// Whether the name matches the glob as shell patterns do: '*' takes any run of bytes, '?' one byte,
// '[...]' one byte of a class. The star backtracks to the last place it stood, which is enough for
// patterns, since a later star can only take more.
bool matched(std::string_view pattern, std::string_view name) {
    std::size_t at = 0;
    std::size_t here = 0;
    std::size_t star = std::string_view::npos;
    std::size_t resumed = 0;

    while (here < name.size()) {
        if (at < pattern.size() && pattern[at] == '*') {
            star = ++at;
            resumed = here;
            continue;
        }

        if (at < pattern.size()) {
            std::size_t next = at + 1;
            bool fits = false;

            if (pattern[at] == '?') {
                fits = true;
            } else if (pattern[at] == '[') {
                fits = classed(pattern, next, name[here]);
            } else {
                fits = pattern[at] == name[here];
            }

            if (fits) {
                at = next;
                ++here;
                continue;
            }
        }

        if (star == std::string_view::npos) return false;

        at = star;
        here = ++resumed;
    }

    while (at < pattern.size() && pattern[at] == '*') ++at;

    return at == pattern.size();
}

// The value of a string key in a desktop file with its escapes resolved: \s, \n, \t, \r and \\.
// A backslash before anything else stays, as the specification leaves that undefined.
std::string unescaped(std::string_view value) {
    std::string made;

    for (std::size_t at = 0; at < value.size(); ++at) {
        if (value[at] != '\\' || at + 1 == value.size()) {
            made += value[at];
            continue;
        }

        const char next = value[++at];

        if (next == 's') made += ' ';
        else if (next == 'n') made += '\n';
        else if (next == 't') made += '\t';
        else if (next == 'r') made += '\r';
        else if (next == '\\') made += '\\';
        else {
            made += '\\';
            made += next;
        }
    }

    return made;
}

// A boolean value of a desktop file: only "true" is true.
bool truth(std::string_view value) { return trimmed(value) == "true"; }

// The groups of an ini-like file (mimeapps.list, mimeinfo.cache, a desktop file): for each line of
// the given group, the key and the value, both trimmed; comments and other groups left out.
std::vector<std::pair<std::string_view, std::string_view>> grouped(std::string_view text, std::string_view group) {
    std::vector<std::pair<std::string_view, std::string_view>> made;
    bool inside = false;

    for (const std::string_view raw : lines_of(text)) {
        const std::string_view line = trimmed(raw);

        if (line.empty() || line.front() == '#') continue;

        if (line.front() == '[') {
            inside = line.size() >= 2 && line.back() == ']' && line.substr(1, line.size() - 2) == group;
            continue;
        }

        if (!inside) continue;

        const std::size_t equals = line.find('=');

        if (equals == std::string_view::npos) continue;

        made.emplace_back(trimmed(line.substr(0, equals)), trimmed(line.substr(equals + 1)));
    }

    return made;
}

// The lists of one group of a mimeapps.list or mimeinfo.cache: type to desktop file ids, in the
// order written. A type written twice keeps both lists, the first one first.
Lists listed(std::string_view text, std::string_view group) {
    Lists made;

    for (const auto& [key, value] : grouped(text, group)) {
        std::vector<std::string>& into = made[std::string{key}];

        for (std::string& one : split(value, ';')) into.push_back(std::move(one));
    }

    return made;
}

// The locale's candidates for a localized key, best first, as the Desktop Entry specification
// orders them: lang_COUNTRY@MODIFIER, lang_COUNTRY, lang@MODIFIER, lang. The encoding (".UTF-8")
// plays no part. "C" and "POSIX" have none.
std::vector<std::string> candidates(std::string_view locale) {
    std::string_view rest = locale;
    std::string_view modifier;

    if (const std::size_t at = rest.find('@'); at != std::string_view::npos) {
        modifier = rest.substr(at + 1);
        rest = rest.substr(0, at);
    }

    if (const std::size_t dot = rest.find('.'); dot != std::string_view::npos) rest = rest.substr(0, dot);

    if (rest.empty() || rest == "C" || rest == "POSIX") return {};

    std::string_view lang = rest;
    std::string_view country;

    if (const std::size_t under = rest.find('_'); under != std::string_view::npos) {
        lang = rest.substr(0, under);
        country = rest.substr(under + 1);
    }

    std::vector<std::string> made;
    const std::string plain{lang};

    if (!country.empty() && !modifier.empty()) made.push_back(plain + "_" + std::string{country} + "@" + std::string{modifier});
    if (!country.empty()) made.push_back(plain + "_" + std::string{country});
    if (!modifier.empty()) made.push_back(plain + "@" + std::string{modifier});

    made.push_back(plain);

    return made;
}

// The little-endian number of `size` bytes at `at`.
std::uint32_t little(std::span<const unsigned char> bytes, std::size_t at, std::size_t size) {
    std::uint32_t made = 0;

    for (std::size_t one = size; one > 0; --one) made = (made << 8) | bytes[at + one - 1];

    return made;
}

// Whether the bytes start with the given ones.
bool begins(std::span<const unsigned char> bytes, std::initializer_list<unsigned char> with) {
    if (bytes.size() < with.size()) return false;

    return std::equal(with.begin(), with.end(), bytes.begin());
}

// One directory of an XDG list: the variable's value when it is an absolute path, otherwise the
// default under the home folder, or nothing without a home.
std::optional<fs::path> homed(const std::optional<std::string>& given, const std::optional<std::string>& home, std::string_view fallback) {
    if (given.has_value() && !given->empty() && given->front() == '/') return fs::from_string(*given);

    if (!home.has_value() || home->empty()) return std::nullopt;

    return fs::from_string(*home) / fs::from_string(fallback);
}

// The directories of an XDG list variable: its absolute entries, or the default list when it is
// unset or has none.
std::vector<fs::path> listed_dirs(const std::optional<std::string>& given, std::string_view fallback) {
    std::vector<fs::path> made;

    if (given.has_value())
        for (const std::string& one : split(*given, ':'))
            if (one.front() == '/') made.push_back(fs::from_string(one));

    if (made.empty())
        for (const std::string& one : split(fallback, ':')) made.push_back(fs::from_string(one));

    return made;
}

}

// The globs of a globs2 file: one "weight:type:glob[:flags]" per line, # starting a comment. The
// flag "cs" makes the glob case-sensitive. A line that does not have this shape is skipped.
std::vector<Glob> globs(std::string_view text) {
    std::vector<Glob> made;

    for (const std::string_view raw : lines_of(text)) {
        const std::string_view line = trimmed(raw);

        if (line.empty() || line.front() == '#') continue;

        std::vector<std::string_view> parts;
        std::string_view rest = line;

        for (int field = 0; field < 3; ++field) {
            const std::size_t colon = rest.find(':');

            if (colon == std::string_view::npos) break;

            parts.push_back(rest.substr(0, colon));
            rest.remove_prefix(colon + 1);
        }

        parts.push_back(rest);

        if (parts.size() < 3 || parts[1].empty() || parts[2].empty()) continue;

        Glob one;

        int weight = 0;

        for (const char digit : parts[0]) {
            if (digit < '0' || digit > '9') {
                weight = -1;
                break;
            }

            weight = weight * 10 + (digit - '0');
        }

        if (weight < 0 || parts[0].empty()) continue;

        one.weight = weight;
        one.type = std::string{parts[1]};
        one.pattern = std::string{parts[2]};

        if (parts.size() == 4)
            for (const std::string& flag : split(parts[3], ','))
                if (flag == "cs") one.cased = true;

        made.push_back(std::move(one));
    }

    return made;
}

// The type of a file by its name: among the globs that match, the one with the greatest weight,
// and of equal weights the longest pattern, as shared-mime-info says; of equal lengths the one
// listed first. A glob is compared in lower case unless it is case-sensitive. The marker
// __NOGLOBS__ matches nothing. Nothing when no glob matches.
std::optional<std::string> type_of(std::string_view name, const std::vector<Glob>& known) {
    const std::string low = lowered(name);
    const Glob* best = nullptr;

    for (const Glob& one : known) {
        if (one.pattern == "__NOGLOBS__") continue;

        const bool fits = one.cased ? matched(one.pattern, name) : matched(lowered(one.pattern), low);

        if (!fits) continue;

        if (best == nullptr || one.weight > best->weight || (one.weight == best->weight && one.pattern.size() > best->pattern.size()))
            best = &one;
    }

    if (best == nullptr) return std::nullopt;

    return best->type;
}

// The parents of types from a subclasses file: "child parent" per line; a type may have several.
Lists parents(std::string_view text) {
    Lists made;

    for (const std::string_view raw : lines_of(text)) {
        const std::string_view line = trimmed(raw);

        if (line.empty() || line.front() == '#') continue;

        const std::size_t space = line.find(' ');

        if (space == std::string_view::npos) continue;

        const std::string_view parent = trimmed(line.substr(space + 1));

        if (!parent.empty()) made[std::string{line.substr(0, space)}].emplace_back(parent);
    }

    return made;
}

// The three groups of one mimeapps.list.
Associations associations(std::string_view text) {
    return {listed(text, "Default Applications"), listed(text, "Added Associations"), listed(text, "Removed Associations")};
}

// The type to programs lists of one mimeinfo.cache.
Lists cached(std::string_view text) { return listed(text, "MIME Cache"); }

// The [Desktop Entry] group of a desktop file, with the name in the best language of the locale.
// Nothing when the group is missing. Hidden means the entry was deleted; NoDisplay only keeps it
// off lists (`shown`), and it still opens files it is the default for.
std::optional<Entry> entry(std::string_view text, std::string_view locale) {
    const auto keys = grouped(text, "Desktop Entry");

    bool found = false;

    for (const std::string_view raw : lines_of(text))
        if (trimmed(raw) == "[Desktop Entry]") found = true;

    if (!found) return std::nullopt;

    const std::vector<std::string> wanted = candidates(locale);

    Entry made;
    std::size_t rank = wanted.size() + 1;

    for (const auto& [key, value] : keys) {
        if (key == "Name" && rank == wanted.size() + 1) {
            made.name = unescaped(value);
            rank = wanted.size();
        } else if (key.size() > 6 && key.substr(0, 5) == "Name[" && key.back() == ']') {
            const std::string_view tag = key.substr(5, key.size() - 6);

            for (std::size_t at = 0; at < wanted.size() && at < rank; ++at)
                if (wanted[at] == tag) {
                    made.name = unescaped(value);
                    rank = at;
                }
        } else if (key == "Type") {
            made.application = value == "Application";
        } else if (key == "Exec") {
            made.exec = unescaped(value);
        } else if (key == "TryExec") {
            made.try_exec = unescaped(value);
        } else if (key == "Icon") {
            made.icon = unescaped(value);
        } else if (key == "MimeType") {
            made.types = split(value, ';');
        } else if (key == "Hidden") {
            made.hidden = truth(value);
        } else if (key == "NoDisplay") {
            made.shown = !truth(value);
        } else if (key == "Terminal") {
            made.terminal = truth(value);
        }
    }

    return made;
}

// The command line of the entry for one file, program first, by the Exec rules of the Desktop
// Entry specification. Arguments are separated by spaces; a quoted one keeps its spaces, and in it
// a backslash escapes ", `, $ and \. The field codes: %f and %u give the file (a local path, which
// %u allows too), %F and %U the file as the only member of the list, %i the icon as two arguments
// ("--icon" and its name, nothing without an icon), %c the name, %k the desktop file's location,
// %% a percent sign. The deprecated codes and unknown ones disappear, and an argument made of
// nothing else disappears with them. A command line that names no file gets the file at its end,
// as GLib does, so that a program written without a code still receives what was opened. Nothing
// when the line is empty or a quote is left open.
std::optional<std::vector<std::string>> expanded(const Entry& one, std::string_view file, std::string_view location) {
    std::vector<std::string> words;
    std::string word;
    bool started = false;
    bool quoted = false;

    for (std::size_t at = 0; at < one.exec.size(); ++at) {
        const char here = one.exec[at];

        if (quoted) {
            if (here == '\\' && at + 1 < one.exec.size()) {
                const char next = one.exec[at + 1];

                if (next == '"' || next == '`' || next == '$' || next == '\\') {
                    word += next;
                    ++at;
                    continue;
                }
            }

            if (here == '"') quoted = false;
            else word += here;

            continue;
        }

        if (here == ' ' || here == '\t' || here == '\n') {
            if (started) words.push_back(std::move(word));

            word.clear();
            started = false;
            continue;
        }

        started = true;

        if (here == '"') quoted = true;
        else word += here;
    }

    if (quoted) return std::nullopt;

    if (started) words.push_back(std::move(word));

    std::vector<std::string> made;
    bool filed = false;

    for (const std::string& raw : words) {
        if (raw == "%F" || raw == "%U") {
            if (!file.empty()) made.emplace_back(file);

            filed = true;
            continue;
        }

        if (raw == "%i") {
            if (!one.icon.empty()) {
                made.emplace_back("--icon");
                made.push_back(one.icon);
            }

            continue;
        }

        std::string done;
        bool coded = false;
        bool dropped = false;

        for (std::size_t at = 0; at < raw.size(); ++at) {
            if (raw[at] != '%' || at + 1 == raw.size()) {
                done += raw[at];
                continue;
            }

            const char code = raw[++at];

            coded = true;

            if (code == '%') {
                done += '%';
            } else if (code == 'f' || code == 'u') {
                if (file.empty()) dropped = true;
                else done += file;

                filed = true;
            } else if (code == 'c') {
                done += one.name;
            } else if (code == 'k') {
                done += location;
            }
        }

        if (dropped || (coded && done.empty())) continue;

        made.push_back(std::move(done));
    }

    if (made.empty()) return std::nullopt;

    if (!filed && !file.empty()) made.emplace_back(file);

    return made;
}

// The XDG directories the desktop reads, most important first. The mimeapps.list files go in the
// order of the MIME applications associations specification: the user's configuration, the
// system's configuration, then the applications folders of the user and of the system (a place
// the specification keeps for compatibility); in each, the lists of the current desktops first
// ("kde-mimeapps.list"), in lower case, then the common one. Applications and MIME folders go user
// first.
Places places(const Environment& from) {
    Places made;

    std::vector<std::string> desktops;

    if (from.current.has_value())
        for (const std::string& one : split(*from.current, ':')) desktops.push_back(lowered(one));

    std::vector<fs::path> listing;

    if (const auto one = homed(from.config_home, from.home, ".config")) listing.push_back(*one);

    for (const fs::path& one : listed_dirs(from.config_dirs, "/etc/xdg")) listing.push_back(one);

    std::vector<fs::path> data;

    if (const auto one = homed(from.data_home, from.home, ".local/share")) data.push_back(*one);

    for (const fs::path& one : listed_dirs(from.data_dirs, "/usr/local/share:/usr/share")) data.push_back(one);

    for (const fs::path& one : data) listing.push_back(one / "applications");

    for (const fs::path& folder : listing) {
        for (const std::string& desktop : desktops) made.lists.push_back(folder / fs::from_string(desktop + "-mimeapps.list"));

        made.lists.push_back(folder / "mimeapps.list");
    }

    for (const fs::path& one : data) {
        made.applications.push_back(one / "applications");
        made.mime.push_back(one / "mime");
    }

    return made;
}

// The subsystem of a Windows executable from the first bytes of the file: the MZ header points
// (e_lfanew, at 0x3C) to the PE signature, after it come the 20 bytes of the file header and then
// the optional header, whose magic says PE32 (0x10B) or PE32+ (0x20B) and whose Subsystem field
// stands 68 bytes in, in both. 2 is a program with windows, 3 a console program. Nothing when the
// bytes are not a PE file or end too early.
std::optional<std::uint16_t> subsystem(std::span<const unsigned char> head) {
    if (head.size() < 0x40 || !begins(head, {'M', 'Z'})) return std::nullopt;

    const std::size_t signature = little(head, 0x3C, 4);

    if (signature > head.size() || head.size() - signature < 4 + 20 + 70) return std::nullopt;

    if (!begins(head.subspan(signature), {'P', 'E', 0, 0})) return std::nullopt;

    const std::size_t optional = signature + 24;
    const std::uint32_t magic = little(head, optional, 2);

    if (magic != 0x10B && magic != 0x20B) return std::nullopt;

    return static_cast<std::uint16_t>(little(head, optional + 68, 2));
}

// How a file runs on Windows, by its extension (any case, without the dot) and first bytes. An
// .exe is a console program when its subsystem says so (3) and a program with windows otherwise,
// also when the header cannot be read: the system then says what is wrong with it. A .com is
// always a console program; .bat, .cmd and .ps1 are scripts. Everything else is no program.
Runnable windows_runnable(std::string_view extension, std::span<const unsigned char> head) {
    const std::string kind = lowered(extension);

    if (kind == "exe") return subsystem(head) == std::uint16_t{3} ? Runnable::console : Runnable::window;
    if (kind == "com") return Runnable::console;
    if (kind == "bat" || kind == "cmd" || kind == "ps1") return Runnable::script;

    return Runnable::no;
}

// How a file runs on Linux and macOS: only with the execute bit, and only when its first bytes say
// it is a program (ELF, Mach-O in either byte order, 32 or 64 bits, or a fat binary) or a script
// (#!). A program cannot be told apart from one with windows without guessing, so it counts as a
// console program; the terminal starts both. The execute bit alone is no program: files on shares
// often carry it by accident.
Runnable posix_runnable(std::span<const unsigned char> head, bool executable) {
    if (!executable) return Runnable::no;

    if (begins(head, {'#', '!'})) return Runnable::script;

    if (begins(head, {0x7F, 'E', 'L', 'F'}) || begins(head, {0xFE, 0xED, 0xFA, 0xCE}) || begins(head, {0xFE, 0xED, 0xFA, 0xCF}) ||
        begins(head, {0xCE, 0xFA, 0xED, 0xFE}) || begins(head, {0xCF, 0xFA, 0xED, 0xFE}) || begins(head, {0xCA, 0xFE, 0xBA, 0xBE}))
        return Runnable::console;

    return Runnable::no;
}

// Where a file came from by its Zone.Identifier stream: the Internet zone (3) and the restricted
// one (4) are the internet, the local machine (0), the intranet (1) and trusted sites (2) are here.
// A stream without a zone says nothing, so here.
Origin zone_of(std::string_view text) {
    for (const auto& [key, value] : grouped(text, "ZoneTransfer")) {
        if (lowered(key) != "zoneid") continue;

        int zone = 0;

        for (const char digit : value) {
            if (digit < '0' || digit > '9') return Origin::here;

            zone = zone * 10 + (digit - '0');

            if (zone > 100) return Origin::here;
        }

        return zone == 3 || zone == 4 ? Origin::internet : Origin::here;
    }

    return Origin::here;
}

// Whether the first bytes of a file are text: no NUL and well-formed UTF-8 by the one rule
// (`utf8::span`), a sequence cut off by the end of the bytes read aside. An empty file is text.
bool textual(std::span<const unsigned char> head) {
    const std::string_view text{reinterpret_cast<const char*>(head.data()), head.size()};

    for (std::size_t at = 0; at < text.size();) {
        if (text[at] == '\0') return false;

        const utf8::Span one = utf8::span(text.substr(at));

        if (one.kind == utf8::Span::Kind::bad) return false;

        if (one.kind == utf8::Span::Kind::partial) return true;

        at += one.size;
    }

    return true;
}

}
