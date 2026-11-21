#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "charsets.hpp"
#include "tal/docs.hpp"
#include "tal/text.hpp"
#include "tal/trouble.hpp"

// The text version of an HTML page (E6f), for when no engine shows it or the terminal shows no
// pictures. The bytes are decoded as a browser decodes them (byte order mark, a declaration in the
// first 1024 bytes with the labels of WHATWG, UTF-8 when the file is all UTF-8, else the encoding
// the caller falls back to), then read by a tokenizer that follows HTML5 in what matters for text:
// tags and their attributes, comments, character references (the whole table of names), the
// elements whose content is not markup, and SVG and MathML. There is no tree: the elements open
// and close on a stack, closed implicitly where HTML closes them (a paragraph by the next block,
// an item by the next item, a cell by the next cell), and the text goes into blocks as it comes:
// paragraphs, headings, items of lists, quotes, preformatted text and the rows of tables. CSS is
// not read and scripts never run, so what a page hides or shows by them stays as written; only the
// attribute hidden hides. Everything is bounded: the input, the depth of the stack, the text, the
// blocks and the links, and a page over the last three ends as far as it got, marked cut.
namespace tal::docs {

namespace {

// A named character reference and its text in UTF-8.
struct Entity {
    std::string_view name;
    std::string_view text;
};

#include "entities.inc"

constexpr std::size_t longest_name = 33;

// Whether `one` is ASCII whitespace as HTML counts it.
[[nodiscard]] constexpr bool space(char one) { return one == ' ' || one == '\t' || one == '\n' || one == '\f' || one == '\r'; }

// Whether `one` is an ASCII letter.
[[nodiscard]] constexpr bool letter(char one) { return (one >= 'a' && one <= 'z') || (one >= 'A' && one <= 'Z'); }

// Whether `one` is an ASCII letter or digit.
[[nodiscard]] constexpr bool alnum(char one) { return letter(one) || (one >= '0' && one <= '9'); }

// `one` in ASCII lowercase.
[[nodiscard]] constexpr char lowered(char one) { return one >= 'A' && one <= 'Z' ? static_cast<char>(one - 'A' + 'a') : one; }

// `text` in ASCII lowercase.
[[nodiscard]] std::string lowered(std::string_view text) {
    std::string made{text};

    for (char& one : made) one = lowered(one);

    return made;
}

// `text` without the ASCII whitespace around it.
[[nodiscard]] std::string_view trimmed(std::string_view text) {
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);

    return text;
}

// Whether `text` at `at` begins with `word`, ignoring ASCII case.
[[nodiscard]] bool begins(std::string_view text, std::size_t at, std::string_view word) {
    if (text.size() - std::min(at, text.size()) < word.size()) return false;

    for (std::size_t one = 0; one < word.size(); ++one)
        if (lowered(text[at + one]) != word[one]) return false;

    return true;
}

// The entity named exactly `name`, or none.
[[nodiscard]] const Entity* entity(std::string_view name) {
    const auto found = std::lower_bound(entities.begin(), entities.end(), name, [](const Entity& one, std::string_view sought) { return one.name < sought; });

    return found != entities.end() && found->name == name ? &*found : nullptr;
}

// Whether `code` is a control character that a terminal could take for a command: C0 but for
// tab, line feed, form feed and carriage return, DEL and C1.
[[nodiscard]] constexpr bool controlling(char32_t code) {
    return (code < 0x20 && code != '\t' && code != '\n' && code != '\f' && code != '\r') || (code >= 0x7F && code <= 0x9F);
}

// The character a numeric reference stands for: 0x80 to 0x9F as windows-1252 has them, nothing
// and what is no character U+FFFD, and so a control character, which shows as U+FFFD anyway.
[[nodiscard]] char32_t numbered(std::uint32_t code) {
    if (code == 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return 0xFFFD;

    if (code >= 0x80 && code <= 0x9F) code = charsets::single(static_cast<std::byte>(code), Encoding::windows_1252);

    return controlling(code) ? 0xFFFD : code;
}

// The character reference at `at` (which holds '&') in `text`: its UTF-8, `at` moved past it.
// What is no reference is the '&' alone. In an attribute a name without its semicolon that runs
// on into '=' or a letter or digit is left as written, as HTML5 leaves it.
[[nodiscard]] std::string referenced(std::string_view text, std::size_t& at, bool attribute) {
    const std::size_t start = at + 1;

    if (start < text.size() && text[start] == '#') {
        std::size_t next = start + 1;
        const bool hex = next < text.size() && (text[next] == 'x' || text[next] == 'X');

        if (hex) ++next;

        const std::size_t digits = next;
        std::uint32_t code = 0;

        while (next < text.size()) {
            const char one = text[next];
            int value = -1;

            if (one >= '0' && one <= '9') value = one - '0';
            else if (hex && one >= 'a' && one <= 'f') value = one - 'a' + 10;
            else if (hex && one >= 'A' && one <= 'F') value = one - 'A' + 10;

            if (value < 0) break;

            code = std::min<std::uint32_t>(code * (hex ? 16 : 10) + static_cast<std::uint32_t>(value), 0x110000);
            ++next;
        }

        if (next == digits) {
            ++at;

            return "&";
        }

        if (next < text.size() && text[next] == ';') ++next;

        at = next;

        std::string made;

        text::encoded(made, numbered(code));

        return made;
    }

    std::size_t end = start;

    while (end < text.size() && end - start < longest_name && alnum(text[end])) ++end;

    std::string sought{text.substr(start, end - start)};

    if (end < text.size() && text[end] == ';') sought += ';';

    for (std::size_t much = sought.size(); much > 0; --much) {
        const Entity* found = entity(std::string_view{sought}.substr(0, much));

        if (found == nullptr) continue;

        const std::size_t after = start + much;

        if (attribute && found->name.back() != ';' && after < text.size() && (text[after] == '=' || alnum(text[after]))) break;

        at = after;

        return std::string{found->text};
    }

    ++at;

    return "&";
}

// `text` with its character references replaced, as in the value of an attribute.
[[nodiscard]] std::string unreferenced(std::string_view text, bool attribute) {
    std::string made;
    std::size_t at = 0;

    while (at < text.size()) {
        const std::size_t amp = text.find('&', at);

        made.append(text.substr(at, amp - at));

        if (amp == std::string_view::npos) break;

        at = amp;
        made += referenced(text, at, attribute);
    }

    return made;
}

// One attribute of a tag.
struct Attribute {
    std::string name;
    std::string value;
};

// A tag as the tokenizer read it.
struct Tag {
    std::string name;
    std::vector<Attribute> attributes;
    bool closing = false;
    bool selfclosing = false;

    // The value of the attribute `wanted`, or none.
    [[nodiscard]] const std::string* attribute(std::string_view wanted) const {
        for (const Attribute& one : attributes)
            if (one.name == wanted) return &one.value;

        return nullptr;
    }
};

// The tag that starts at `at` in `text` (just past '<' and an optional '/'), `at` moved past its
// '>'; none when the text ends inside the tag, which HTML then drops.
[[nodiscard]] std::optional<Tag> tagged(std::string_view text, std::size_t& at, bool closing, bool decode) {
    Tag made;

    made.closing = closing;

    std::size_t next = at;

    while (next < text.size() && !space(text[next]) && text[next] != '/' && text[next] != '>') made.name += lowered(text[next++]);

    while (next < text.size()) {
        while (next < text.size() && (space(text[next]) || text[next] == '/')) {
            made.selfclosing = text[next] == '/';
            ++next;
        }

        if (next >= text.size()) break;

        if (text[next] == '>') {
            at = next + 1;

            return made;
        }

        made.selfclosing = false;

        Attribute one;

        do {
            one.name += lowered(text[next++]);
        } while (next < text.size() && !space(text[next]) && text[next] != '/' && text[next] != '>' && text[next] != '=');

        while (next < text.size() && space(text[next])) ++next;

        if (next < text.size() && text[next] == '=') {
            ++next;

            while (next < text.size() && space(text[next])) ++next;

            std::size_t from = next;
            std::size_t upto = next;

            if (next < text.size() && (text[next] == '"' || text[next] == '\'')) {
                const std::size_t close = text.find(text[next], next + 1);

                if (close == std::string_view::npos) break;

                from = next + 1;
                upto = close;
                next = close + 1;
            } else {
                while (next < text.size() && !space(text[next]) && text[next] != '>') ++next;

                upto = next;
            }

            one.value = decode ? unreferenced(text.substr(from, upto - from), true) : std::string{text.substr(from, upto - from)};
        }

        if (std::none_of(made.attributes.begin(), made.attributes.end(), [&](const Attribute& known) { return known.name == one.name; }))
            made.attributes.push_back(std::move(one));
    }

    at = text.size();

    return std::nullopt;
}

// The encoding a label names, as WHATWG maps the labels of the encodings read here; UTF-16 in a
// declaration means UTF-8, since a file that says so in ASCII is not UTF-16. None for any other.
[[nodiscard]] std::optional<std::optional<Encoding>> labelled(std::string_view label) {
    const std::string name = lowered(trimmed(label));
    constexpr std::array<std::string_view, 7> utf8{"utf-8", "utf8", "unicode-1-1-utf-8", "unicode11utf8", "unicode20utf8", "x-unicode20utf8", "utf-16"};
    constexpr std::array<std::string_view, 5> utf16{"utf-16le", "utf-16be", "ucs-2", "unicode", "csunicode"};
    constexpr std::array<std::string_view, 17> latin{"windows-1252", "ansi_x3.4-1968", "ascii",      "cp1252",     "cp819",     "csisolatin1",
                                                     "ibm819",       "iso-8859-1",     "iso-ir-100", "iso8859-1",  "iso88591",  "iso_8859-1",
                                                     "iso_8859-1:1987", "l1",          "latin1",     "us-ascii",   "x-cp1252"};
    constexpr std::array<std::string_view, 3> central{"windows-1250", "cp1250", "x-cp1250"};
    constexpr std::array<std::string_view, 9> latin2{"iso-8859-2", "csisolatin2", "iso-ir-101", "iso8859-2", "iso88592",
                                                     "iso_8859-2", "iso_8859-2:1987", "l2", "latin2"};
    const auto in = [&](const auto& names) { return std::find(names.begin(), names.end(), name) != names.end(); };

    if (in(utf8) || in(utf16)) return std::optional<Encoding>{};
    if (in(latin)) return std::optional<Encoding>{Encoding::windows_1252};
    if (in(central)) return std::optional<Encoding>{Encoding::windows_1250};
    if (in(latin2)) return std::optional<Encoding>{Encoding::iso_8859_2};

    return std::nullopt;
}

// The charset that the value of a content attribute of a meta element names, or none.
[[nodiscard]] std::optional<std::string> charset_in(std::string_view content) {
    const std::string low = lowered(content);
    std::size_t at = low.find("charset");

    while (at != std::string::npos) {
        std::size_t next = at + 7;

        while (next < low.size() && space(low[next])) ++next;

        if (next < low.size() && low[next] == '=') {
            ++next;

            while (next < low.size() && space(low[next])) ++next;

            if (next < low.size() && (low[next] == '"' || low[next] == '\'')) {
                const std::size_t close = low.find(low[next], next + 1);

                if (close == std::string::npos) return std::nullopt;

                return low.substr(next + 1, close - next - 1);
            }

            std::size_t end = next;

            while (end < low.size() && !space(low[end]) && low[end] != ';') ++end;

            return end > next ? std::optional<std::string>{low.substr(next, end - next)} : std::nullopt;
        }

        at = low.find("charset", at + 7);
    }

    return std::nullopt;
}

// The encoding declared by a meta element in `head`, the first 1024 bytes: an outer none when no
// known label is declared, an inner none for UTF-8.
[[nodiscard]] std::optional<std::optional<Encoding>> declared(std::string_view head) {
    std::size_t at = 0;

    while (at < head.size()) {
        const std::size_t open = head.find('<', at);

        if (open == std::string_view::npos) break;

        if (head.substr(open, 4) == "<!--") {
            const std::size_t close = head.find("-->", open + 4);

            if (close == std::string_view::npos) break;

            at = close + 3;

            continue;
        }

        at = open + 1;

        if (!begins(head, at, "meta") || at + 4 >= head.size() || !(space(head[at + 4]) || head[at + 4] == '/')) continue;

        std::size_t inside = at;
        const std::optional<Tag> tag = tagged(head, inside, false, false);

        if (!tag) break;

        at = inside;

        std::optional<std::string> label;

        if (const std::string* charset = tag->attribute("charset")) label = *charset;
        else if (const std::string* content = tag->attribute("content"); content != nullptr && tag->attribute("http-equiv") != nullptr)
            label = charset_in(*content);

        if (label)
            if (const auto known = labelled(*label)) return known;
    }

    return std::nullopt;
}

// `html` decoded into UTF-8 as a browser decodes it.
[[nodiscard]] std::string decoded(std::span<const std::byte> html, Encoding fallback) {
    const auto at = [&](std::size_t where) { return where < html.size() ? std::to_integer<unsigned>(html[where]) : 256U; };

    if (at(0) == 0xEF && at(1) == 0xBB && at(2) == 0xBF) return charsets::from_utf8(html.subspan(3));
    if (at(0) == 0xFF && at(1) == 0xFE) return charsets::from_utf16(html.subspan(2), false);
    if (at(0) == 0xFE && at(1) == 0xFF) return charsets::from_utf16(html.subspan(2), true);

    const std::span<const std::byte> head = html.first(std::min<std::size_t>(html.size(), 1024));

    if (const auto known = declared({reinterpret_cast<const char*>(head.data()), head.size()}))
        return *known ? charsets::from_single(html, **known) : charsets::from_utf8(html);

    return charsets::whole_utf8(html) ? charsets::from_utf8(html) : charsets::from_single(html, fallback);
}

// The parts of an address: everything before the path, the path, and the query and fragment.
struct Parts {
    std::string front;
    std::string path;
    std::string tail;
};

// The scheme of `address` with its colon ("https:"), or empty when it has none.
[[nodiscard]] std::string_view scheme_of(std::string_view address) {
    if (address.empty() || !letter(address.front())) return {};

    for (std::size_t at = 1; at < address.size(); ++at) {
        const char one = address[at];

        if (one == ':') return address.substr(0, at + 1);

        if (!alnum(one) && one != '+' && one != '-' && one != '.') return {};
    }

    return {};
}

// `address` split into its parts.
[[nodiscard]] Parts parts_of(std::string_view address) {
    Parts made;
    const std::string_view scheme = scheme_of(address);
    std::size_t at = scheme.size();

    if (address.substr(at, 2) == "//") at = std::min(address.find_first_of("/?#", at + 2), address.size());

    made.front = address.substr(0, at);

    const std::size_t end = std::min(address.find_first_of("?#", at), address.size());

    made.path = address.substr(at, end - at);
    made.tail = address.substr(end);

    return made;
}

// `path` without its dot segments, as RFC 3986 (5.2.4) removes them.
[[nodiscard]] std::string undotted(std::string_view path) {
    std::vector<std::string_view> kept;
    std::size_t at = 0;
    const bool rooted = path.starts_with('/');
    bool directory = false;

    while (at <= path.size()) {
        const std::size_t end = std::min(path.find('/', at), path.size());
        const std::string_view segment = path.substr(at, end - at);

        directory = segment == "." || segment == "..";

        if (segment == "..") {
            if (!kept.empty()) kept.pop_back();
        } else if (segment != "." && !(segment.empty() && end < path.size())) {
            kept.push_back(segment);
        }

        at = end + 1;
    }

    std::string made = rooted ? "/" : "";

    for (std::size_t one = 0; one < kept.size(); ++one) {
        if (kept[one].empty() && one + 1 == kept.size()) break;

        made += kept[one];

        if (one + 1 < kept.size()) made += '/';
    }

    if ((directory || (!kept.empty() && kept.back().empty())) && !made.empty() && made.back() != '/') made += '/';

    return made;
}

// The address that `reference` names, read against `base`, as RFC 3986 (5.2) resolves it; a
// reference with a scheme, or any one against a base without a scheme, stands as it is.
[[nodiscard]] std::string resolved(std::string_view base, std::string_view reference) {
    std::string clean;

    for (const char one : trimmed(reference))
        if (one != '\t' && one != '\n' && one != '\r') clean += one;

    if (!scheme_of(clean).empty() || scheme_of(base).empty()) return clean;

    const Parts at = parts_of(base);
    const std::string_view without = std::string_view{base}.substr(0, base.find('#'));

    if (clean.empty()) return std::string{without};
    if (clean.starts_with('#')) return std::string{without} + clean;
    if (clean.starts_with("//")) return std::string{scheme_of(base)} + clean;

    const std::size_t end = std::min(clean.find_first_of("?#"), clean.size());
    const std::string_view path = std::string_view{clean}.substr(0, end);
    const std::string_view tail = std::string_view{clean}.substr(end);

    if (path.empty()) return at.front + at.path + std::string{tail};
    if (path.starts_with('/')) return at.front + undotted(path) + std::string{tail};

    const std::string folder = at.path.substr(0, at.path.rfind('/') + 1);
    const bool authority = at.front.size() > scheme_of(base).size();

    return at.front + undotted((folder.empty() && authority ? "/" : folder) + std::string{path}) + std::string{tail};
}

// The elements that start a block: the text before them ends a paragraph.
[[nodiscard]] bool blocking(std::string_view name) {
    constexpr std::array<std::string_view, 44> names{
        "address", "article", "aside",   "blockquote", "body",    "caption", "center", "dd",      "details", "dialog", "dir",
        "div",     "dl",      "dt",      "fieldset",   "figcaption", "figure", "footer", "form",  "h1",      "h2",     "h3",
        "h4",      "h5",      "h6",      "header",     "hgroup",  "hr",      "li",     "listing", "main",    "menu",   "nav",
        "ol",      "p",       "pre",     "section",    "summary", "table",   "td",     "th",      "tr",      "ul",     "legend"};

    return std::find(names.begin(), names.end(), name) != names.end();
}

// The elements that have no end tag.
[[nodiscard]] bool empty_element(std::string_view name) {
    constexpr std::array<std::string_view, 15> names{"area", "base",  "br",   "col",  "embed", "hr",    "img",   "input",
                                                     "link", "meta", "param", "source", "track", "wbr", "keygen"};

    return std::find(names.begin(), names.end(), name) != names.end();
}

// The elements whose content is not markup and is not shown: everything up to the end tag goes.
[[nodiscard]] bool hidden_raw(std::string_view name) {
    return name == "script" || name == "style" || name == "xmp" || name == "iframe" || name == "noembed" || name == "noframes" || name == "template";
}

// The elements of the head, which do not end it.
[[nodiscard]] bool of_head(std::string_view name) {
    return name == "title" || name == "meta" || name == "link" || name == "base" || name == "style" || name == "script" || name == "noscript" ||
           name == "template";
}

// The level of a heading element (1 to 6), or 0 for any other.
[[nodiscard]] int heading_of(std::string_view name) { return name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6' ? name[1] - '0' : 0; }

// An open element on the stack.
struct Frame {
    std::string name;
    bool muted = false;
    bool used = false;
    std::string mark;
    int counter = 1;
    bool ordered = false;
};

// Where the text of a block or of a cell gathers, with its links.
struct Gathering {
    std::string text;
    std::vector<Anchor> anchors;
    bool spaced = false;
};

// The reading of one page.
class Reading {
public:
    Reading(std::string_view text, std::string_view address, const Bounds& bounds) : text_(prepared(text)), base_(address), bounds_(bounds) {}

    // The page read.
    [[nodiscard]] Plain read() {
        while (at_ < text_.size() && !made_.cut) step();

        while (!stack_.empty()) popped();

        celled();
        unrowed();
        flushed();

        return std::move(made_);
    }

private:
    // `text` (UTF-8) as the tokenizer reads it: its line ends made "\n", as HTML makes them, its
    // NUL dropped and its control characters, which a terminal could take for commands, U+FFFD.
    [[nodiscard]] static std::string prepared(std::string_view text) {
        std::string made;
        std::size_t at = 0;

        made.reserve(text.size());

        while (at < text.size()) {
            const char one = text[at];

            if (one >= 0x20 && one < 0x7F) {
                made += one;
                ++at;

                continue;
            }

            if (one == '\r') {
                made += '\n';
                at += at + 1 < text.size() && text[at + 1] == '\n' ? 2 : 1;

                continue;
            }

            const char32_t point = text::decoded(text, at).value_or(char32_t{0xFFFD});

            if (point != 0) text::encoded(made, controlling(point) ? char32_t{0xFFFD} : point);
        }

        return made;
    }

    // One step of the tokenizer: a run of text, a reference, a tag or a comment.
    void step() {
        const std::string_view all = text_;
        const char one = all[at_];

        if (one == '&') {
            texted(referenced(all, at_, false));

            return;
        }

        if (one != '<') {
            const std::size_t end = std::min(all.find_first_of("<&", at_), all.size());

            texted(all.substr(at_, end - at_));
            at_ = end;

            return;
        }

        const char next = at_ + 1 < all.size() ? all[at_ + 1] : '\0';

        if (letter(next)) {
            ++at_;

            if (std::optional<Tag> tag = tagged(all, at_, false, true)) started(*tag);

            return;
        }

        if (next == '/' && at_ + 2 < all.size() && letter(all[at_ + 2])) {
            at_ += 2;

            if (std::optional<Tag> tag = tagged(all, at_, true, false)) ended(tag->name);

            return;
        }

        if (all.substr(at_, 4) == "<!--") {
            skipped_comment();

            return;
        }

        if (next == '!' && foreign() && all.substr(at_, 9) == "<![CDATA[") {
            const std::size_t close = all.find("]]>", at_ + 9);

            texted(all.substr(at_ + 9, std::min(close, all.size()) - at_ - 9));
            at_ = close == std::string_view::npos ? all.size() : close + 3;

            return;
        }

        if (next == '!' || next == '?' || next == '/') {
            const std::size_t close = all.find('>', at_ + 2);

            at_ = close == std::string_view::npos ? all.size() : close + 1;

            return;
        }

        texted("<");
        ++at_;
    }

    // Past the comment at the cursor: up to "-->" or "--!>", "<!-->" and "<!--->" being empty
    // comments, or to the end.
    void skipped_comment() {
        const std::string_view all = text_;
        const std::size_t inside = at_ + 4;

        if (all.substr(inside, 1) == ">") {
            at_ = inside + 1;

            return;
        }

        if (all.substr(inside, 2) == "->") {
            at_ = inside + 2;

            return;
        }

        const std::size_t plain = all.find("-->", inside);
        const std::size_t banged = all.find("--!>", inside);

        if (plain == std::string_view::npos && banged == std::string_view::npos) at_ = all.size();
        else if (banged < plain) at_ = banged + 4;
        else at_ = plain + 3;
    }

    // The text up to the end tag of the element `name` (whose start tag the cursor is past), the
    // cursor moved to that end tag.
    [[nodiscard]] std::string_view raw(std::string_view name) {
        const std::string_view all = text_;
        std::size_t look = at_;

        while (true) {
            const std::size_t open = all.find("</", look);

            if (open == std::string_view::npos) {
                const std::string_view content = all.substr(at_);

                at_ = all.size();

                return content;
            }

            const std::size_t after = open + 2 + name.size();

            if (begins(all, open + 2, name) && (after >= all.size() || space(all[after]) || all[after] == '/' || all[after] == '>')) {
                const std::string_view content = all.substr(at_, open - at_);

                at_ = open;

                return content;
            }

            look = open + 2;
        }
    }

    // Whether the cursor is inside SVG or MathML.
    [[nodiscard]] bool foreign() const {
        return std::any_of(stack_.begin(), stack_.end(), [](const Frame& one) { return one.name == "svg" || one.name == "math"; });
    }

    // Whether what comes now is shown.
    [[nodiscard]] bool shown() const { return stack_.empty() || !stack_.back().muted; }

    // The index of the nearest open element named `name` above any named in `walls`, or none.
    [[nodiscard]] std::optional<std::size_t> open(std::string_view name, std::initializer_list<std::string_view> walls = {}) const {
        for (std::size_t at = stack_.size(); at-- > 0;) {
            if (stack_[at].name == name) return at;

            if (std::find(walls.begin(), walls.end(), stack_[at].name) != walls.end()) return std::nullopt;
        }

        return std::nullopt;
    }

    // The number of open elements named in `names`.
    [[nodiscard]] int counted(std::initializer_list<std::string_view> names) const {
        return static_cast<int>(
            std::count_if(stack_.begin(), stack_.end(), [&](const Frame& one) { return std::find(names.begin(), names.end(), one.name) != names.end(); }));
    }

    // The elements down to and with the one at `index` closed.
    void closed_to(std::size_t index) {
        while (stack_.size() > index) popped();
    }

    // The top element closed.
    void popped() {
        closing(stack_.back());
        stack_.pop_back();
    }

    // The start tag `tag` read.
    void started(const Tag& tag) {
        const std::string& name = tag.name;

        if (hidden_raw(name)) {
            (void)raw(name);

            return;
        }

        if (name == "title" || name == "textarea") {
            const std::string content = unreferenced(raw(name), false);

            if (name == "title" && made_.title.empty() && !foreign()) made_.title = collapsed(content);
            else if (name == "textarea" && shown() && !headed()) bracketed(collapsed(content));

            return;
        }

        if (name == "plaintext") {
            flushed();

            const std::string_view rest = std::string_view{text_}.substr(at_);

            at_ = text_.size();

            if (shown()) {
                gathering().text.append(rest);
                emitted(Block::Kind::preformatted, 0, {});
            }

            return;
        }

        if (name == "html") return;

        if (name == "head") {
            if (!headed() && pushing()) stack_.push_back(framed("head", true));

            return;
        }

        if (headed() && !of_head(name)) {
            if (const auto at = open("head")) closed_to(*at);
        }

        if (name == "body") return;

        implied(name);

        const bool muted = !shown() || tag.attribute("hidden") != nullptr;
        const bool shows = !muted;

        if (shows) acted(tag);

        if (empty_element(name) || (tag.selfclosing && foreign()) || !pushing()) return;

        Frame frame = framed(name, muted);

        if (shows) prepared(frame, tag);

        stack_.push_back(std::move(frame));

        if ((name == "pre" || name == "listing") && at_ < text_.size() && text_[at_] == '\n') ++at_;
    }

    // A frame of the element `name`, shown or not.
    [[nodiscard]] static Frame framed(std::string name, bool muted) {
        Frame made;

        made.name = std::move(name);
        made.muted = muted;

        return made;
    }

    // Whether another element may go on the stack.
    [[nodiscard]] bool pushing() const { return stack_.size() < bounds_.depth; }

    // Whether the head is open.
    [[nodiscard]] bool headed() const { return !stack_.empty() && std::any_of(stack_.begin(), stack_.end(), [](const Frame& one) { return one.name == "head"; }); }

    // The elements that the start of `name` closes, as HTML closes them.
    void implied(const std::string& name) {
        if (blocking(name))
            while (!stack_.empty() && stack_.back().name == "p") popped();

        if (name == "li") {
            if (const auto at = open("li", {"ul", "ol", "menu", "dir"})) closed_to(*at);
        } else if (name == "dt" || name == "dd") {
            if (const auto at = open("dd", {"dl"})) closed_to(*at);
            if (const auto at = open("dt", {"dl"})) closed_to(*at);
        } else if (name == "tr") {
            if (const auto at = open("tr", {"table"})) closed_to(*at);
        } else if (name == "td" || name == "th") {
            if (const auto at = open("td", {"tr", "table"})) closed_to(*at);
            if (const auto at = open("th", {"tr", "table"})) closed_to(*at);
        } else if (name == "option" || name == "optgroup") {
            if (const auto at = open("option", {"select"})) closed_to(*at);
        } else if (heading_of(name) > 0 && !stack_.empty() && heading_of(stack_.back().name) > 0) {
            popped();
        } else if (name == "a") {
            if (const auto at = open("a")) closed_to(*at);
        } else if (name == "select") {
            if (const auto at = open("select")) closed_to(*at);
        }
    }

    // What the start of a shown element does at once.
    void acted(const Tag& tag) {
        const std::string& name = tag.name;

        if (blocking(name) && name != "td" && name != "th" && name != "tr" && name != "caption") flushed();

        if (name == "br") {
            broken();
        } else if (name == "hr") {
            flushed();

            if (!in_cell()) emitted(Block::Kind::rule, 0, {});
        } else if (name == "img") {
            if (const std::string* alt = tag.attribute("alt"); alt != nullptr && !trimmed(*alt).empty()) bracketed(collapsed(*alt));
        } else if (name == "input") {
            inputted(tag);
        } else if (name == "base") {
            if (const std::string* href = tag.attribute("href"); href != nullptr && !based_) {
                base_ = resolved(base_, *href);
                based_ = true;
            }
        } else if (name == "tr" && outer()) {
            rowed();
        } else if ((name == "td" || name == "th") && outer()) {
            celled();

            if (!row_) rowed();

            emptied(cell_);
            in_cell_ = true;
        }
    }

    // The frame of a shown element filled before it goes on the stack.
    void prepared(Frame& frame, const Tag& tag) {
        if (frame.name == "ol") {
            frame.ordered = true;

            if (const std::string* start = tag.attribute("start")) frame.counter = number(*start).value_or(1);
        } else if (frame.name == "li") {
            const auto list = std::find_if(stack_.rbegin(), stack_.rend(), [](const Frame& one) { return one.name == "ul" || one.name == "ol" || one.name == "menu" || one.name == "dir"; });

            if (list != stack_.rend() && list->ordered) {
                if (const std::string* value = tag.attribute("value")) list->counter = number(*value).value_or(list->counter);

                frame.mark = std::to_string(list->counter++) + ".";
            } else {
                frame.mark = "•";
            }
        } else if (frame.name == "a") {
            if (const std::string* href = tag.attribute("href")) {
                link_ = Link{resolved(base_, *href), std::nullopt};
            }
        } else if (frame.name == "select") {
            options_.clear();
            option_.reset();
        } else if (frame.name == "option") {
            option_ = Option{{}, tag.attribute("selected") != nullptr};
        } else if (frame.name == "button") {
            Gathering& into = gathering();
            const bool spaced = into.spaced;

            texted_raw("[");
            button_ = Button{&into, into.text.size(), spaced, emptied_};
        }
    }

    // A whole number in `text`, or none.
    [[nodiscard]] static std::optional<int> number(std::string_view text) {
        const std::string_view digits = trimmed(text);
        int made = 0;

        if (digits.empty()) return std::nullopt;

        for (const char one : digits) {
            if (one < '0' || one > '9') return std::nullopt;

            made = std::min(made * 10 + (one - '0'), 1000000);
        }

        return made;
    }

    // An input field shown as its value in brackets.
    void inputted(const Tag& tag) {
        const std::string type = lowered(tag.attribute("type") != nullptr ? *tag.attribute("type") : "text");

        if (type == "hidden") return;

        if (type == "checkbox" || type == "radio") {
            bracketed(tag.attribute("checked") != nullptr ? "x" : " ");

            return;
        }

        const std::string* value = tag.attribute("value");

        if ((value == nullptr || trimmed(*value).empty()) && type != "submit" && type != "reset" && type != "button" && type != "image")
            value = tag.attribute("placeholder");

        if (type == "image" && (value == nullptr || trimmed(*value).empty())) value = tag.attribute("alt");

        bracketed(value != nullptr && !trimmed(*value).empty() ? collapsed(*value) : " ");
    }

    // The end tag of `name` read: the nearest open element of that name closed with all above it;
    // an end tag of nothing open is dropped, but </br> is a break and </p> ends a paragraph.
    void ended(const std::string& name) {
        if (name == "br") {
            if (shown()) broken();

            return;
        }

        if (name == "body" || name == "html") return;

        if (const auto at = open(name)) {
            closed_to(*at);
        } else if (name == "p" && shown()) {
            flushed();
        }
    }

    // What closing the element `frame` (still on the stack) does.
    void closing(Frame& frame) {
        if (frame.muted) return;

        const std::string& name = frame.name;

        if (name == "a") {
            anchored();
            link_.reset();
        } else if (name == "option") {
            if (option_) options_.push_back(std::move(*option_));

            option_.reset();
        } else if (name == "select") {
            if (option_) options_.push_back(std::move(*option_));

            option_.reset();

            const auto chosen = std::find_if(options_.begin(), options_.end(), [](const Option& one) { return one.selected; });
            std::string shown = chosen != options_.end() ? std::move(chosen->text) : (options_.empty() ? std::string{} : std::move(options_.front().text));

            options_.clear();

            if (!trimmed(shown).empty()) bracketed(collapsed(shown));
        } else if (name == "button") {
            Gathering& into = gathering();
            const bool whole = button_ && button_->into == &into && button_->emptied == emptied_ && button_->from >= 1 && button_->from <= into.text.size();

            if (whole && trimmed(std::string_view{into.text}.substr(button_->from)).empty()) {
                into.text.resize(button_->from - 1);

                while (!into.text.empty() && into.text.back() == ' ') into.text.pop_back();

                into.spaced = button_->spaced;
            } else if (button_ && !whole && !made_.cut && into.text.empty() && !made_.blocks.empty() && holds_text(made_.blocks.back())) {
                made_.blocks.back().text += ']';
            } else if (button_) {
                texted_raw("]");
            }

            button_.reset();
        } else if ((name == "td" || name == "th") && outer_at(frame)) {
            celled();
        } else if (name == "tr" && outer_at(frame)) {
            celled();
            unrowed();
        } else if (name == "table" && counted({"table"}) == 1) {
            celled();
            unrowed();
            flushed();
        } else if (blocking(name) || heading_of(name) > 0) {
            flushed();
        }
    }

    // Whether the cursor is in the outermost table.
    [[nodiscard]] bool outer() const { return counted({"table"}) == 1; }

    // Whether the element `frame` (on the stack) is in the outermost table.
    [[nodiscard]] bool outer_at(const Frame& frame) const {
        int tables = 0;

        for (const Frame& one : stack_) {
            if (&one == &frame) return tables == 1;

            if (one.name == "table") ++tables;
        }

        return false;
    }

    // Whether text now goes into a cell of the outermost table.
    [[nodiscard]] bool in_cell() const { return in_cell_ && !open("caption", {"td", "th"}); }

    // Where text goes now.
    [[nodiscard]] Gathering& gathering() { return in_cell() ? cell_ : block_; }

    // Text added, its whitespace collapsed outside preformatted text.
    void texted(std::string_view given) {
        if (!shown() || headed() || made_.cut) return;

        if (open("select")) {
            if (option_) option_->text += given;

            return;
        }

        if (open("pre") || open("listing")) {
            texted_raw(given);

            return;
        }

        Gathering& into = gathering();

        for (std::size_t at = 0; at < given.size(); ++at) {
            const char one = given[at];

            if (space(one)) {
                into.spaced = true;

                continue;
            }

            if (into.spaced && !into.text.empty() && into.text.back() != '\n') into.text += ' ';

            into.spaced = false;
            started_link(into);

            const std::size_t from = at;

            while (at + 1 < given.size() && !space(given[at + 1])) ++at;

            into.text += given.substr(from, at - from + 1);
        }
    }

    // Text added as it is, a link that waits for its first character started.
    void texted_raw(std::string_view given) {
        if (given.empty()) return;

        Gathering& into = gathering();

        if (into.spaced && !into.text.empty() && into.text.back() != '\n' && !open("pre") && !open("listing")) into.text += ' ';

        into.spaced = false;
        started_link(into);
        into.text += given;
    }

    // `content` shown in brackets, as a field or a picture is.
    void bracketed(std::string_view content) {
        if (!shown() || headed() || made_.cut) return;

        texted_raw("[" + std::string{content} + "]");
    }

    // A line break: a new line in a block, a space in a cell.
    void broken() {
        Gathering& into = gathering();

        if (in_cell()) {
            into.spaced = true;
        } else if (!into.text.empty()) {
            into.text += '\n';
            into.spaced = false;
        }
    }

    // `given` with its whitespace collapsed and trimmed.
    [[nodiscard]] static std::string collapsed(std::string_view given) {
        std::string made;
        bool gap = false;

        for (const char one : given) {
            if (space(one)) {
                gap = !made.empty();

                continue;
            }

            if (gap) made += ' ';

            gap = false;
            made += one;
        }

        return made;
    }

    // The link that waits for its first character starts where the text now ends.
    void started_link(Gathering& into) {
        if (link_ && !link_->from) link_->from = std::pair{&into, into.text.size()};
    }

    // The link ends where the text it started in now ends.
    void anchored() {
        if (!link_ || !link_->from) return;

        Gathering& into = *link_->from->first;
        const std::size_t from = link_->from->second;

        if (into.text.size() > from && into.anchors.size() < bounds_.anchors && anchors_ < bounds_.anchors) {
            into.anchors.push_back(Anchor{0, from, into.text.size(), link_->address});
            ++anchors_;
        }

        link_->from.reset();
    }

    // The block gathered so far emitted, in the kind the open elements give it; in a cell it is a
    // space instead.
    void flushed() {
        if (in_cell()) {
            cell_.spaced = true;

            return;
        }

        if (link_ && link_->from && link_->from->first == &block_) anchored();

        if (block_.text.empty()) {
            block_.spaced = false;

            return;
        }

        int level = 0;
        std::string mark;
        Block::Kind kind = Block::Kind::paragraph;

        for (std::size_t at = stack_.size(); at-- > 0;) {
            Frame& one = stack_[at];

            if (const int heading = heading_of(one.name); heading > 0) {
                kind = Block::Kind::heading;
                level = heading;

                break;
            }

            if (one.name == "li") {
                kind = Block::Kind::item;
                level = std::max(1, counted({"ul", "ol", "menu", "dir"}));
                mark = one.used ? "" : one.mark;
                one.used = true;

                break;
            }

            if (one.name == "dd") {
                kind = Block::Kind::item;
                level = std::max(1, counted({"dl"}));

                break;
            }

            if (one.name == "blockquote") {
                kind = Block::Kind::quote;
                level = counted({"blockquote"});

                break;
            }
        }

        if (open("pre") || open("listing")) {
            kind = Block::Kind::preformatted;
            level = 0;
            mark.clear();
        }

        emitted(kind, level, std::move(mark));
    }

    // Whether `block` is text that a bracket can close: not a row, a rule or the start of a page.
    [[nodiscard]] static bool holds_text(const Block& block) {
        return block.kind != Block::Kind::row && block.kind != Block::Kind::rule && block.kind != Block::Kind::page;
    }

    // `one` emptied for what follows; a button whose bracket opened in it can no longer take its
    // bracket back.
    void emptied(Gathering& one) {
        one = Gathering{};
        ++emptied_;
    }

    // The block gathered so far emitted as `kind`, unless a bound is reached, which cuts the page.
    void emitted(Block::Kind kind, int level, std::string mark) {
        std::string body = std::move(block_.text);
        std::vector<Anchor> anchors = std::move(block_.anchors);

        emptied(block_);

        if (kind == Block::Kind::preformatted) {
            while (!body.empty() && body.back() == '\n') body.pop_back();
        } else {
            while (!body.empty() && (body.back() == ' ' || body.back() == '\n')) body.pop_back();
        }

        for (Anchor& one : anchors) one.upto = std::min(one.upto, body.size());

        std::erase_if(anchors, [](const Anchor& one) { return one.from >= one.upto; });

        if (body.empty() && kind != Block::Kind::rule) return;

        Block made;

        made.kind = kind;
        made.level = level;
        made.mark = std::move(mark);
        made.text = std::move(body);
        made.anchors = std::move(anchors);
        added(std::move(made));
    }

    // `block` added to the page, unless a bound is reached, which cuts it.
    void added(Block block) {
        std::size_t size = block.text.size();

        for (const std::string& one : block.cells) size += one.size();

        if (made_.blocks.size() >= bounds_.blocks || text_size_ + size > bounds_.text) {
            made_.cut = true;

            return;
        }

        text_size_ += size;
        made_.blocks.push_back(std::move(block));
    }

    // A row of the outermost table started, the one before it ended.
    void rowed() {
        celled();
        unrowed();
        flushed_before_table();
        row_ = Block{};
        row_->kind = Block::Kind::row;
    }

    // The text before a table emitted before its first row.
    void flushed_before_table() {
        if (!in_cell_ && !block_.text.empty()) flushed();
    }

    // The open cell of the outermost table ended.
    void celled() {
        if (!in_cell_) return;

        if (link_ && link_->from && link_->from->first == &cell_) anchored();

        in_cell_ = false;

        if (!row_) return;

        const std::size_t first = cell_.text.find_first_not_of(" \n");
        const std::size_t lead = first == std::string::npos ? cell_.text.size() : first;
        std::string body = cell_.text.substr(lead);

        while (!body.empty() && (body.back() == ' ' || body.back() == '\n')) body.pop_back();

        for (Anchor one : cell_.anchors) {
            one.cell = row_->cells.size();
            one.from = one.from > lead ? one.from - lead : 0;
            one.upto = std::min(one.upto - std::min(one.upto, lead), body.size());

            if (one.from < one.upto) row_->anchors.push_back(std::move(one));
        }

        row_->cells.push_back(std::move(body));
        emptied(cell_);
    }

    // The open row of the outermost table emitted, if it has any text.
    void unrowed() {
        if (!row_) return;

        Block made = std::move(*row_);

        row_.reset();

        while (!made.cells.empty() && made.cells.back().empty()) made.cells.pop_back();

        if (std::any_of(made.cells.begin(), made.cells.end(), [](const std::string& one) { return !one.empty(); })) added(std::move(made));
    }

    // A link being read: its address and where its text starts, once it has some.
    struct Link {
        std::string address;
        std::optional<std::pair<Gathering*, std::size_t>> from;
    };

    // A button being read: where its text goes, where its bracket opened, whether a space waited
    // before it, and how many times text gathered had been emptied then.
    struct Button {
        Gathering* into = nullptr;
        std::size_t from = 0;
        bool spaced = false;
        std::size_t emptied = 0;
    };

    // An option of a select being read.
    struct Option {
        std::string text;
        bool selected = false;
    };

    std::string text_;
    std::size_t at_ = 0;
    std::string base_;
    bool based_ = false;
    Bounds bounds_;
    Plain made_;
    std::vector<Frame> stack_;
    Gathering block_;
    Gathering cell_;
    bool in_cell_ = false;
    std::optional<Block> row_;
    std::optional<Link> link_;
    std::vector<Option> options_;
    std::optional<Option> option_;
    std::optional<Button> button_;
    std::size_t emptied_ = 0;
    std::size_t text_size_ = 0;
    std::size_t anchors_ = 0;
};

}

// The text version of the page `html`, its links resolved against `address`, its bytes decoded
// with `fallback` when they declare no encoding and are not all UTF-8. Throws `Trouble` full for a
// page over `bounds.bytes`.
Plain plain(std::span<const std::byte> html, std::string_view address, Encoding fallback, Bounds bounds) {
    if (html.size() > bounds.bytes) throw Trouble{Kind::full, "The page is too large for its text version."};

    return Reading{decoded(html, fallback), address, bounds}.read();
}

}
