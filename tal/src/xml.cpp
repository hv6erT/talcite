#include "tal/xml.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "tal/text.hpp"
#include "tal/trouble.hpp"

// XML 1.0 as the files people keep write it, read by our own reader, which is meant to meet hostile
// text: its size is bounded before a byte is looked at, and while it is read how deep elements nest
// and how many nodes and attributes there are in all, so that neither the stack nor the memory
// grows with the attacker's wish. The DTD is never read: a doctype stays the raw text it was, its
// entities are not expanded, so there is no "billion laughs" and nothing is fetched (XXE). Known
// are the five entities of XML and references to characters; any other `&...;`, and a lone `&`,
// stay the text they were, as pugixml, which read these files before, left them; a comment may not
// hold "--", which the writer could not give back.
//
// The tree carries the whole file, so that whoever changes it writes back the rest untouched: the
// declaration, comments, processing instructions, the doctype, CDATA apart from text. Line ends
// become "\n" and white space in attribute values one space each, as the standard asks; text made
// of white space alone between tags is left out, so indentation does not grow a node at each read.
// UTF-8 only: a declaration that names another encoding is unsupported, since a file we do not
// understand must not go back to disk as something else. At the top level stand what pugixml took:
// any number of elements, and text, kept without the white space around it, not dropped; a text
// without an element is refused.
namespace tal::xml {

namespace {

// The byte order mark, which Notepad puts at the start of a file.
constexpr std::string_view marked = "\xEF\xBB\xBF";

// Whether `one` is white space in XML.
bool spacy(char one) { return one == ' ' || one == '\t' || one == '\r' || one == '\n'; }

// Whether `one` may start a name: a letter, '_', ':', or any byte of a character beyond ASCII.
bool name_start(char one) {
    const auto byte = static_cast<unsigned char>(one);

    return (one >= 'a' && one <= 'z') || (one >= 'A' && one <= 'Z') || one == '_' || one == ':' || byte >= 0x80;
}

// Whether `one` may stand in a name after its first character.
bool name_part(char one) { return name_start(one) || (one >= '0' && one <= '9') || one == '-' || one == '.'; }

// Whether `name` is a name of XML: not empty, of name characters, and valid UTF-8.
bool named_well(std::string_view name) {
    if (name.empty() || !name_start(name.front())) return false;

    return std::all_of(name.begin(), name.end(), name_part) && text::valid(name);
}

// Whether the code point `one` may stand in a document as a reference: XML 1.0's Char.
bool character(char32_t one) {
    if (one == 0x9 || one == 0xA || one == 0xD) return true;

    if (one < 0x20) return false;

    if (one >= 0xD800 && one <= 0xDFFF) return false;

    return one != 0xFFFE && one != 0xFFFF && one <= 0x10FFFF;
}

// Whether the declaration's `encoding` is UTF-8, in any letter case.
bool utf8_named(std::string_view encoding) {
    std::string made{encoding};

    std::transform(made.begin(), made.end(), made.begin(), [](char one) { return one >= 'A' && one <= 'Z' ? static_cast<char>(one - 'A' + 'a') : one; });

    return made == "utf-8" || made == "utf8";
}

// A reader of one document within its limits.
class Reader {
public:
    // The reader of `text` within `limits`.
    Reader(std::string_view text, Limits limits) : text_(text), limits_(limits) {}

    // The document of the whole text.
    Document whole() {
        if (text_.size() > limits_.bytes) failed("the text is longer than the limit");

        if (text_.starts_with(marked)) at_ = marked.size();

        if (!text::valid(text_)) failed("the text is not UTF-8");

        Document made;
        bool element = false;
        bool doctype = false;

        if (text_.substr(at_).starts_with("<?xml?>") || (text_.substr(at_).starts_with("<?xml") && at_ + 5 < text_.size() && spacy(text_[at_ + 5])))
            made.nodes.push_back(declaration());

        while (at_ < text_.size()) {
            if (text_[at_] != '<') {
                std::optional<Node> one = texted(true);

                if (one) made.nodes.push_back(std::move(*one));

                continue;
            }

            if (text_.substr(at_).starts_with("<!DOCTYPE")) {
                if (doctype || element) failed("a doctype stands only once, before the first element");

                doctype = true;
                made.nodes.push_back(doctyped());
            } else if (text_.substr(at_).starts_with("<![CDATA[")) {
                failed("CDATA stands only inside an element");
            } else if (text_.substr(at_).starts_with("</")) {
                failed("a closing tag without its element");
            } else {
                Node one = node(0);

                element = element || one.kind == Kind::element;
                made.nodes.push_back(std::move(one));
            }
        }

        if (!element) failed("the document has no element");

        return made;
    }

private:
    // Throws `Trouble` (malformed) with the line and column where the reader stands.
    [[noreturn]] void failed(std::string_view why) const {
        const std::size_t upto = std::min(at_, text_.size());
        const std::string_view before = text_.substr(0, upto);
        const std::size_t start = before.rfind('\n') == std::string_view::npos ? 0 : before.rfind('\n') + 1;
        const std::size_t line = static_cast<std::size_t>(std::count(before.begin(), before.end(), '\n')) + 1;
        std::size_t column = 1;

        for (std::size_t at = start; at < upto; ++at)
            if ((static_cast<unsigned char>(text_[at]) & 0xC0) != 0x80) ++column;

        throw Trouble{::tal::Kind::malformed, "line " + std::to_string(line) + ", column " + std::to_string(column) + ": " + std::string{why}};
    }

    // Counts one more node or attribute against the limit.
    void counted() {
        if (++nodes_ > limits_.nodes) failed("the document holds more nodes than the limit");
    }

    // Steps past `what`, which must stand here.
    void expected(std::string_view what) {
        if (!text_.substr(at_).starts_with(what)) failed("\"" + std::string{what} + "\" is expected");

        at_ += what.size();
    }

    // Steps past white space; whether there was any.
    bool spaced() {
        const std::size_t from = at_;

        while (at_ < text_.size() && spacy(text_[at_])) ++at_;

        return at_ > from;
    }

    // A name.
    std::string name() {
        const std::size_t from = at_;

        if (at_ >= text_.size() || !name_start(text_[at_])) failed("a name is expected");

        while (at_ < text_.size() && name_part(text_[at_])) ++at_;

        return std::string{text_.substr(from, at_ - from)};
    }

    // The raw text up to `end`, which is stepped past; `what` names the construct in a refusal.
    std::string_view until(std::string_view end, std::string_view what) {
        const std::size_t found = text_.find(end, at_);

        if (found == std::string_view::npos) failed(std::string{what} + " does not end");

        const std::string_view made = text_.substr(at_, found - at_);

        at_ = found + end.size();

        return made;
    }

    // Appends `raw` to `into` with its line ends made "\n".
    static void lined(std::string& into, std::string_view raw) {
        for (std::size_t at = 0; at < raw.size(); ++at) {
            if (raw[at] == '\r') {
                into += '\n';

                if (at + 1 < raw.size() && raw[at + 1] == '\n') ++at;
            } else {
                into += raw[at];
            }
        }
    }

    // Appends to `into` the reference that starts at `raw[at]`, an '&', and steps `at` past it; a
    // reference to no known entity, or an '&' that starts none, stays as it stands. The ';' is
    // looked for among the next 40 bytes only, or many '&' and no ';' would take quadratic time.
    void referred(std::string& into, std::string_view raw, std::size_t& at) const {
        std::size_t end = raw.substr(at, 40).find(';');

        if (end != std::string_view::npos) end += at;

        if (end == std::string_view::npos) {
            into += '&';
            ++at;

            return;
        }

        const std::string_view inner = raw.substr(at + 1, end - at - 1);
        static constexpr std::pair<std::string_view, char> known[] = {{"lt", '<'}, {"gt", '>'}, {"amp", '&'}, {"apos", '\''}, {"quot", '"'}};

        for (const auto& [entity, sign] : known) {
            if (inner == entity) {
                into += sign;
                at = end + 1;

                return;
            }
        }

        if (inner.starts_with('#')) {
            const bool hex = inner.size() > 1 && inner[1] == 'x';
            const std::string_view digits = inner.substr(hex ? 2 : 1);
            std::uint32_t point = 0;

            if (digits.empty()) failed("a reference to a character has no digits");

            for (const char one : digits) {
                int value = -1;

                if (one >= '0' && one <= '9') value = one - '0';
                else if (hex && one >= 'a' && one <= 'f') value = one - 'a' + 10;
                else if (hex && one >= 'A' && one <= 'F') value = one - 'A' + 10;

                if (value < 0) failed("a reference to a character has a wrong digit");

                point = point * (hex ? 16 : 10) + static_cast<std::uint32_t>(value);

                if (point > 0x10FFFF) failed("a reference to a character beyond Unicode");
            }

            if (!character(point)) failed("a reference to a character XML does not allow");

            text::encoded(into, point);
            at = end + 1;

            return;
        }

        into += '&';
        ++at;
    }

    // `raw` with its line ends made "\n" and its references read; `attribute` turns white space
    // into spaces as well, but not what references wrote.
    std::string decoded(std::string_view raw, bool attribute) const {
        std::string made;

        made.reserve(raw.size());

        for (std::size_t at = 0; at < raw.size();) {
            const char one = raw[at];

            if (one == '&') {
                referred(made, raw, at);
            } else if (one == '\r') {
                made += attribute ? ' ' : '\n';
                at += at + 1 < raw.size() && raw[at + 1] == '\n' ? 2 : 1;
            } else {
                made += attribute && spacy(one) ? ' ' : one;
                ++at;
            }
        }

        return made;
    }

    // The attributes up to `>`, `/>` or `?>`, each name once, counted against the limit.
    std::vector<Attribute> attributes() {
        std::vector<Attribute> made;

        while (true) {
            const bool space = spaced();

            if (at_ >= text_.size() || text_[at_] == '>' || text_[at_] == '/' || text_[at_] == '?') return made;

            if (!space) failed("white space is expected before an attribute");

            counted();

            Attribute one;

            one.name = name();
            spaced();
            expected("=");
            spaced();

            if (at_ >= text_.size() || (text_[at_] != '"' && text_[at_] != '\'')) failed("an attribute's value in quotes is expected");

            const char quote = text_[at_++];
            const std::size_t end = text_.find(quote, at_);

            if (end == std::string_view::npos) failed("an attribute's value does not end");

            const std::string_view raw = text_.substr(at_, end - at_);

            if (raw.find('<') != std::string_view::npos) failed("'<' in an attribute's value");

            one.value = decoded(raw, true);
            at_ = end + 1;

            for (const Attribute& before : made)
                if (before.name == one.name) failed("the attribute \"" + one.name + "\" stands twice");

            made.push_back(std::move(one));
        }
    }

    // The declaration at the very start: its attributes, of which an encoding is UTF-8 or nothing.
    Node declaration() {
        Node made;

        counted();
        at_ += 2;
        made.kind = Kind::declaration;
        made.name = name();
        made.attributes = attributes();
        expected("?>");

        for (const Attribute& one : made.attributes)
            if (one.name == "encoding" && !utf8_named(one.value)) throw Trouble{::tal::Kind::unsupported, "The document is in " + one.value + ", not UTF-8."};

        return made;
    }

    // A doctype: the raw text after its keyword up to the '>' that ends it, an internal subset in
    // brackets and quoted strings stepped over whole.
    Node doctyped() {
        Node made;

        counted();
        at_ += 9;
        made.kind = Kind::doctype;

        const std::size_t from = at_;
        int brackets = 0;

        while (true) {
            if (at_ >= text_.size()) failed("the doctype does not end");

            const char one = text_[at_];

            if (one == '"' || one == '\'') {
                const std::size_t end = text_.find(one, at_ + 1);

                if (end == std::string_view::npos) failed("a string of the doctype does not end");

                at_ = end + 1;
            } else if (one == '[') {
                ++brackets;
                ++at_;
            } else if (one == ']') {
                --brackets;
                ++at_;
            } else if (one == '>' && brackets <= 0) {
                break;
            } else {
                ++at_;
            }
        }

        std::string_view raw = text_.substr(from, at_ - from);

        ++at_;

        while (!raw.empty() && spacy(raw.front())) raw.remove_prefix(1);
        while (!raw.empty() && spacy(raw.back())) raw.remove_suffix(1);

        lined(made.value, raw);

        return made;
    }

    // Text up to the next '<', or nothing when it is white space alone; at the `top` level without
    // the white space around it, which the lines of the nodes around it put there.
    std::optional<Node> texted(bool top) {
        const std::size_t end = std::min(text_.find('<', at_), text_.size());
        std::string_view raw = text_.substr(at_, end - at_);

        at_ = end;

        if (std::all_of(raw.begin(), raw.end(), spacy)) return std::nullopt;

        while (top && spacy(raw.front())) raw.remove_prefix(1);
        while (top && spacy(raw.back())) raw.remove_suffix(1);

        counted();

        Node made;

        made.kind = Kind::text;
        made.value = decoded(raw, false);

        return made;
    }

    // A node that starts with '<' here, `depth` elements down: a comment, an instruction or an
    // element with what it holds.
    Node node(int depth) {
        Node made;

        counted();

        if (text_.substr(at_).starts_with("<!--")) {
            at_ += 4;
            made.kind = Kind::comment;
            lined(made.value, until("-->", "a comment"));

            if (made.value.find("--") != std::string::npos || made.value.ends_with('-')) failed("\"--\" in a comment");

            return made;
        }

        if (text_.substr(at_).starts_with("<?")) {
            at_ += 2;
            made.kind = Kind::instruction;
            made.name = name();

            std::string lowered = made.name;

            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char one) { return one >= 'A' && one <= 'Z' ? static_cast<char>(one - 'A' + 'a') : one; });

            if (lowered == "xml") failed("the declaration stands only at the start");

            if (!spaced() && !text_.substr(at_).starts_with("?>")) failed("white space is expected after an instruction's target");

            lined(made.value, until("?>", "an instruction"));

            return made;
        }

        if (text_.substr(at_).starts_with("<!")) failed("an unknown declaration");

        if (depth >= limits_.depth) failed("the elements nest deeper than the limit");

        ++at_;
        made.kind = Kind::element;
        made.name = name();
        made.attributes = attributes();

        if (text_.substr(at_).starts_with("/>")) {
            at_ += 2;

            return made;
        }

        expected(">");

        while (true) {
            if (at_ >= text_.size()) failed("the element \"" + made.name + "\" does not end");

            if (text_[at_] != '<') {
                std::optional<Node> one = texted(false);

                if (one) made.children.push_back(std::move(*one));
            } else if (text_.substr(at_).starts_with("</")) {
                at_ += 2;

                if (name() != made.name) failed("the closing tag is not of \"" + made.name + "\"");

                spaced();
                expected(">");

                return made;
            } else if (text_.substr(at_).starts_with("<![CDATA[")) {
                Node one;

                counted();
                at_ += 9;
                one.kind = Kind::cdata;
                lined(one.value, until("]]>", "CDATA"));
                made.children.push_back(std::move(one));
            } else {
                made.children.push_back(node(depth + 1));
            }
        }
    }

    std::string_view text_;
    Limits limits_;
    std::size_t at_ = 0;
    std::size_t nodes_ = 0;
};

// Throws std::invalid_argument saying `why`: the tree cannot be written as XML.
[[noreturn]] void unwritable(const std::string& why) { throw std::invalid_argument{"XML cannot hold " + why}; }

// `value` as text or, with `attribute`, as an attribute's value in double quotes.
void escaped(std::string& into, std::string_view value, bool attribute) {
    if (!text::valid(value)) unwritable("text that is not UTF-8");

    for (const char one : value) {
        switch (one) {
            case '&': into += "&amp;"; break;
            case '<': into += "&lt;"; break;
            case '>': into += "&gt;"; break;
            case '\r': into += "&#13;"; break;
            case '"': into += attribute ? "&quot;" : "\""; break;
            case '\t': into += attribute ? "&#9;" : "\t"; break;
            case '\n': into += attribute ? "&#10;" : "\n"; break;
            default: into += one;
        }
    }
}

// `value` as text with its white space at both ends as references, which the reader keeps where it
// drops white space: around text at the top level, and text of nothing else.
void edged(std::string& into, std::string_view value) {
    const std::size_t first = std::min(value.find_first_not_of(" \t\r\n"), value.size());
    const std::size_t last = value.find_last_not_of(" \t\r\n") == std::string_view::npos ? value.size() : value.find_last_not_of(" \t\r\n") + 1;

    for (const char one : value.substr(0, first)) into += "&#" + std::to_string(static_cast<int>(one)) + ";";

    escaped(into, value.substr(first, last - first), false);

    for (const char one : value.substr(last)) into += "&#" + std::to_string(static_cast<int>(one)) + ";";
}

// `name` after a check that it is a name.
void put_name(std::string& into, const std::string& name) {
    if (!named_well(name)) unwritable("the name \"" + name + "\"");

    into += name;
}

// The attributes `all`, each after a space.
void put_attributes(std::string& into, const std::vector<Attribute>& all) {
    for (const Attribute& one : all) {
        into += ' ';
        put_name(into, one.name);
        into += "=\"";
        escaped(into, one.value, true);
        into += '"';
    }
}

// `one` written at `depth` levels of two spaces; `flat` writes it and what it holds without line
// breaks or indentation, as the content of an element that holds text must stay.
void put(std::string& into, const Node& one, int depth, bool flat) {
    if (!flat) into.append(static_cast<std::size_t>(depth) * 2, ' ');

    switch (one.kind) {
        case Kind::text:
            if (!flat || std::all_of(one.value.begin(), one.value.end(), spacy)) edged(into, one.value);
            else escaped(into, one.value, false);

            break;
        case Kind::cdata: {
            if (!text::valid(one.value)) unwritable("CDATA that is not UTF-8");

            into += "<![CDATA[";

            for (std::size_t at = 0;;) {
                const std::size_t end = one.value.find("]]>", at);

                if (end == std::string::npos) {
                    into.append(one.value, at);

                    break;
                }

                into.append(one.value, at, end + 2 - at);
                into += "]]><![CDATA[";
                at = end + 2;
            }

            into += "]]>";

            break;
        }
        case Kind::comment:
            if (!text::valid(one.value) || one.value.find("--") != std::string::npos || one.value.ends_with('-')) unwritable("a comment with \"--\"");

            into += "<!--" + one.value + "-->";

            break;
        case Kind::declaration:
            into += "<?";
            put_name(into, one.name.empty() ? std::string{"xml"} : one.name);
            put_attributes(into, one.attributes);
            into += "?>";

            break;
        case Kind::instruction:
            if (!text::valid(one.value) || one.value.find("?>") != std::string::npos) unwritable("an instruction with \"?>\"");

            into += "<?";
            put_name(into, one.name);

            if (!one.value.empty()) into += ' ' + one.value;

            into += "?>";

            break;
        case Kind::doctype:
            if (!text::valid(one.value)) unwritable("a doctype that is not UTF-8");

            into += "<!DOCTYPE " + one.value + ">";

            break;
        case Kind::element: {
            into += '<';
            put_name(into, one.name);
            put_attributes(into, one.attributes);

            if (one.children.empty()) {
                into += " />";

                break;
            }

            into += '>';

            const bool texty = flat || std::any_of(one.children.begin(), one.children.end(), [](const Node& each) {
                                   return each.kind == Kind::text || each.kind == Kind::cdata;
                               });

            for (const Node& each : one.children) {
                if (!texty) into += '\n';

                put(into, each, depth + 1, texty);
            }

            if (!texty) {
                into += '\n';
                into.append(static_cast<std::size_t>(depth) * 2, ' ');
            }

            into += "</" + one.name + ">";

            break;
        }
    }
}

}

// The document of `text` within `limits`. Throws `Trouble`: malformed for a mistake, with its line
// and column, or a limit passed; unsupported for an encoding other than UTF-8.
Document parsed(std::string_view text, Limits limits) { return Reader{text, limits}.whole(); }

// `one` as XML in UTF-8: a node a line, elements indented by two spaces, what an element holding
// text holds in one piece; `<?xml version="1.0"?>` first when the document has no declaration.
// Throws std::invalid_argument for what XML cannot hold: a wrong name, text that is not UTF-8, a
// comment with "--", an instruction with "?>", a declaration after the start.
std::string written(const Document& one) {
    std::string made;

    if (std::none_of(one.nodes.begin(), one.nodes.end(), [](const Node& each) { return each.kind == Kind::declaration; }))
        made += "<?xml version=\"1.0\"?>\n";

    for (std::size_t at = 0; at < one.nodes.size(); ++at) {
        if (at > 0 && one.nodes[at].kind == Kind::declaration) unwritable("a declaration after the start");

        put(made, one.nodes[at], 0, false);
        made += '\n';
    }

    return made;
}

// The first element of the document, or `nullptr` when it has none.
Node* root(Document& one) {
    for (Node& node : one.nodes)
        if (node.kind == Kind::element) return &node;

    return nullptr;
}

// The first element of a read-only document.
const Node* root(const Document& one) { return root(const_cast<Document&>(one)); }

// The first child element called `name`, or `nullptr`.
Node* child(Node& one, std::string_view name) {
    for (Node& node : one.children)
        if (node.kind == Kind::element && node.name == name) return &node;

    return nullptr;
}

// The first child element called `name` of a read-only node.
const Node* child(const Node& one, std::string_view name) { return child(const_cast<Node&>(one), name); }

// All child elements called `name`, in document order.
std::vector<Node*> children(Node& one, std::string_view name) {
    std::vector<Node*> made;

    for (Node& node : one.children)
        if (node.kind == Kind::element && node.name == name) made.push_back(&node);

    return made;
}

// All child elements called `name` of a read-only node, in document order.
std::vector<const Node*> children(const Node& one, std::string_view name) {
    std::vector<const Node*> made;

    for (const Node& node : one.children)
        if (node.kind == Kind::element && node.name == name) made.push_back(&node);

    return made;
}

// The text of an element: its text and CDATA children joined in order, as <plot>a<![CDATA[b]]>c
// </plot> means abc.
std::string text(const Node& one) {
    std::string made;

    for (const Node& node : one.children)
        if (node.kind == Kind::text || node.kind == Kind::cdata) made += node.value;

    return made;
}

// Sets the text of `one` to `given`: text and CDATA children go, child elements stay, and the new
// text goes first, as in <actor>Name<role>...</role></actor>; an empty text adds nothing.
void text(Node& one, std::string given) {
    std::erase_if(one.children, [](const Node& node) { return node.kind == Kind::text || node.kind == Kind::cdata; });

    if (given.empty()) return;

    Node made;

    made.kind = Kind::text;
    made.value = std::move(given);
    one.children.insert(one.children.begin(), std::move(made));
}

// The value of the attribute `name`, or `nullptr` when the element has none.
const std::string* attribute(const Node& one, std::string_view name) {
    for (const Attribute& each : one.attributes)
        if (each.name == name) return &each.value;

    return nullptr;
}

// Sets the attribute `name`, replacing its value or adding it at the end.
void attribute(Node& one, std::string_view name, std::string value) {
    for (Attribute& each : one.attributes)
        if (each.name == name) {
            each.value = std::move(value);

            return;
        }

    one.attributes.push_back(Attribute{std::string{name}, std::move(value)});
}

}
