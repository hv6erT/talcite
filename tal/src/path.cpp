#include "tal/path.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace tal {

namespace {

// Whether paths of this system name drives and network shares and fold letter case (Windows).
#ifdef _WIN32
constexpr bool windows = true;
#else
constexpr bool windows = false;
#endif

// One code point read from UTF-8 at `at`, which moves past it; none for a byte that does not begin
// a well-formed sequence, and then `at` moves past the bytes of the broken sequence.
std::optional<char32_t> decoded(std::string_view text, std::size_t& at) {
    const auto lead = static_cast<unsigned char>(text[at]);

    if (lead < 0x80) {
        ++at;

        return lead;
    }

    std::size_t length = 0;
    char32_t point = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;

    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        point = lead & 0x1FU;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        point = lead & 0x0FU;
        low = lead == 0xE0 ? 0xA0 : 0x80;
        high = lead == 0xED ? 0x9F : 0xBF;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        point = lead & 0x07U;
        low = lead == 0xF0 ? 0x90 : 0x80;
        high = lead == 0xF4 ? 0x8F : 0xBF;
    } else {
        ++at;

        return std::nullopt;
    }

    std::size_t next = at + 1;

    for (std::size_t taken = 1; taken < length; ++taken, ++next) {
        if (next >= text.size()) {
            at = next;

            return std::nullopt;
        }

        const auto one = static_cast<unsigned char>(text[next]);

        if (one < low || one > high) {
            at = next;

            return std::nullopt;
        }

        low = 0x80;
        high = 0xBF;
        point = (point << 6U) | (one & 0x3FU);
    }

    at = next;

    return point;
}

// Appends a code point as UTF-8.
void encoded(std::string& into, char32_t point) {
    if (point < 0x80) {
        into += static_cast<char>(point);
    } else if (point < 0x800) {
        into += static_cast<char>(0xC0U | (point >> 6U));
        into += static_cast<char>(0x80U | (point & 0x3FU));
    } else if (point < 0x10000) {
        into += static_cast<char>(0xE0U | (point >> 12U));
        into += static_cast<char>(0x80U | ((point >> 6U) & 0x3FU));
        into += static_cast<char>(0x80U | (point & 0x3FU));
    } else {
        into += static_cast<char>(0xF0U | (point >> 18U));
        into += static_cast<char>(0x80U | ((point >> 12U) & 0x3FU));
        into += static_cast<char>(0x80U | ((point >> 6U) & 0x3FU));
        into += static_cast<char>(0x80U | (point & 0x3FU));
    }
}

// The small letter of a capital in the scripts whose case Windows folds in names: Latin with its
// first extensions, Greek and Cyrillic; any other code point stays.
char32_t lowered(char32_t point) {
    if (point >= U'A' && point <= U'Z') return point + 0x20;
    if (point >= 0xC0 && point <= 0xDE && point != 0xD7) return point + 0x20;
    if (point == 0x178) return 0xFF;

    const bool even = point % 2 == 0;

    if ((point >= 0x100 && point <= 0x12F) || (point >= 0x132 && point <= 0x137) || (point >= 0x14A && point <= 0x177))
        return even ? point + 1 : point;

    if ((point >= 0x139 && point <= 0x148) || (point >= 0x179 && point <= 0x17E)) return even ? point : point + 1;
    if (point >= 0x391 && point <= 0x3A9 && point != 0x3A2) return point + 0x20;
    if (point == 0x3C2) return 0x3C3;
    if (point >= 0x410 && point <= 0x42F) return point + 0x20;
    if (point >= 0x400 && point <= 0x40F) return point + 0x50;

    return point;
}

// The length of the root at the start of a path in the system's form: "/", "C:/" or
// "//server/share" on Windows, "/" elsewhere; 0 for a relative path.
std::size_t root_length(std::string_view text) {
    if constexpr (windows) {
        if (text.size() >= 2 && text[0] == '/' && text[1] == '/' && (text.size() == 2 || text[2] != '/')) {
            std::size_t end = text.find('/', 2);

            if (end != std::string_view::npos) end = text.find('/', end + 1);

            return end == std::string_view::npos ? text.size() : end;
        }

        if (text.size() >= 2 && text[1] == ':' && ((text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= 'a' && text[0] <= 'z')))
            return text.size() >= 3 && text[2] == '/' ? 3 : 2;
    }

    return !text.empty() && text[0] == '/' ? 1 : 0;
}

// The path in its one stored form: separators as '/', no empty or "." names, no separator at the
// end but the root's own; ".." stays, since resolving it needs the disk.
std::string normalized(std::string_view given) {
    std::string text{given};

    if constexpr (windows)
        for (char& one : text)
            if (one == '\\') one = '/';

    const std::size_t rooted = root_length(text);
    std::string made = text.substr(0, rooted);

    if (rooted == 2 && made[1] == ':') made += '/';

    std::vector<std::string_view> names;
    const std::string_view rest = std::string_view{text}.substr(rooted);

    for (std::size_t at = 0; at <= rest.size();) {
        std::size_t end = rest.find('/', at);

        if (end == std::string_view::npos) end = rest.size();

        const std::string_view name = rest.substr(at, end - at);

        if (!name.empty() && name != ".") names.push_back(name);

        at = end + 1;
    }

    for (std::size_t at = 0; at < names.size(); ++at) {
        if (!made.empty() && made.back() != '/') made += '/';

        made += names[at];
    }

    if (made.empty() && !text.empty()) made = ".";

    return made;
}

}

namespace detail {

// The text with letter case folded the way Windows compares names; broken UTF-8 stays as it is.
std::string folded(std::string_view text) {
    std::string made;

    made.reserve(text.size());

    for (std::size_t at = 0; at < text.size();) {
        const std::size_t from = at;

        if (const auto point = decoded(text, at)) {
            encoded(made, lowered(*point));
        } else {
            made.append(text.substr(from, at - from));
        }
    }

    return made;
}

}

// A path from UTF-8 text; on Windows '\' separates names as '/' does.
Path::Path(std::string_view utf8) : stored_(normalized(utf8)) {}

// A path from the form `stored` gave, without loss even for names that are not UTF-8.
Path Path::restored(std::string_view stored) { return Path{stored}; }

// The path as UTF-8 to show a person: bytes that are not UTF-8 become U+FFFD.
std::string Path::text() const {
    std::string made;

    made.reserve(stored_.size());

    for (std::size_t at = 0; at < stored_.size();) {
        const std::size_t from = at;

        if (decoded(stored_, at)) {
            made.append(stored_, from, at - from);
        } else {
            encoded(made, 0xFFFD);
        }
    }

    return made;
}

// The path of `name` inside this one; `name` may hold several names.
Path Path::operator/(std::string_view name) const {
    while (!name.empty() && (name.front() == '/' || (windows && name.front() == '\\'))) name.remove_prefix(1);

    if (name.empty()) return *this;

    if (empty() || stored_ == ".") return Path{name};

    std::string joined = stored_;

    if (joined.back() != '/') joined += '/';

    joined += name;

    return Path{joined};
}

// The folder holding the path: a root is its own parent, and a single relative name has none.
Path Path::parent() const {
    const std::size_t rooted = root_length(stored_);

    if (root() || empty()) return *this;

    const std::size_t last = stored_.rfind('/');

    if (last == std::string::npos) return {};

    Path made;

    made.stored_ = stored_.substr(0, std::max(last, rooted));

    return made;
}

// The last name of the path; a root has none.
std::string_view Path::name() const {
    const std::size_t rooted = root_length(stored_);

    if (rooted >= stored_.size()) return {};

    const std::size_t last = stored_.rfind('/');
    const std::size_t from = last == std::string::npos || last < rooted ? rooted : last + 1;

    return std::string_view{stored_}.substr(from);
}

// The name without its extension; a name that starts with its only dot has no extension.
std::string_view Path::stem() const {
    const std::string_view whole = name();
    const std::size_t dot = whole.rfind('.');

    return dot == std::string_view::npos || dot == 0 || dot + 1 == whole.size() ? whole : whole.substr(0, dot);
}

// The extension of the name, without the dot.
std::string_view Path::extension() const {
    const std::string_view whole = name();
    const std::size_t dot = whole.rfind('.');

    return dot == std::string_view::npos || dot == 0 || dot + 1 == whole.size() ? std::string_view{} : whole.substr(dot + 1);
}

// Whether the path is a root: "/", a drive or a network share.
bool Path::root() const { return !stored_.empty() && root_length(stored_) == stored_.size(); }

// Whether the path starts at a root.
bool Path::absolute() const { return root_length(stored_) > 0; }

// Whether the path is `folder` itself or lies below it, compared as the system compares names.
bool Path::within(const Path& folder) const {
    if (folder.empty() || empty()) return false;

    const std::string mine = windows ? detail::folded(stored_) : stored_;
    std::string theirs = windows ? detail::folded(folder.stored_) : folder.stored_;

    if (mine == theirs) return true;

    if (theirs.back() != '/') theirs += '/';

    return mine.starts_with(theirs);
}

// Whether both paths name the same place as the system compares names: exactly, or on Windows
// regardless of letter case.
bool Path::same(const Path& other) const {
    if constexpr (windows) return detail::folded(stored_) == detail::folded(other.stored_);

    return stored_ == other.stored_;
}

}
