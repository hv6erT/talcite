#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/trouble.hpp"
#include "tal/xml.hpp"

#include "tagging.hpp"

// XMP (ISO 16684-1): RDF in XML, read through our `xml`. Names resolve through the namespaces the
// packet declares, so a property is known by its namespace, whatever prefix the file gave it. The
// properties of every rdf:Description under rdf:RDF become fields keyed as exiv2 keys them:
// "Xmp." and the prefix exiv2 gives the namespace (the file's own prefix for a namespace exiv2 does
// not know), a dot and the name. A simple value is a field; a Bag or a Seq gives a field for each
// item, as a frame of several values in ID3 does; an Alt of languages gives the "x-default" item,
// or else the first; a structure gives its parts under a path, as exiv2 writes it
// ("Xmp.iptcExt.LocationShown[1]/Iptc4xmpExt:City"), the parts named with the file's prefixes.
// Properties may stand as attributes of a description or of a structure, too.
//
// What a photo is, from XMP: the orientation (tiff:Orientation), the camera (tiff:Make,
// tiff:Model), the time it was taken (photoshop:DateCreated, else exif:DateTimeOriginal, written in
// ISO 8601 already), the place (exif:GPSLatitude and exif:GPSLongitude as "54,21.12N" or
// "54,21,7.2N", exif:GPSAltitude as a fraction, exif:GPSAltitudeRef). The GUID of an extended
// packet (xmpNote:HasExtendedXMP) is kept for the container, which joins the extension's pieces.
//
// A packet that is not well-formed XML gives what was gathered before the parse failed, which is
// nothing, and the file is damaged. Nesting deeper than `most_depth` is not followed, and a key
// that would grow past `most_key` along its path is no field. Scopes of namespaces are chained, not
// copied, so a packet of many declarations and many elements costs no more than its size.
namespace tal::meta::tagging {

namespace {

constexpr std::string_view rdf = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
constexpr std::string_view xml_space = "http://www.w3.org/XML/1998/namespace";
constexpr std::string_view tiff_space = "http://ns.adobe.com/tiff/1.0/";
constexpr std::string_view exif_space = "http://ns.adobe.com/exif/1.0/";
constexpr std::string_view photoshop_space = "http://ns.adobe.com/photoshop/1.0/";
constexpr std::string_view note_space = "http://ns.adobe.com/xmp/note/";

// How deep structures and arrays inside each other are followed.
constexpr std::size_t most_depth = 16;

// How long a key may grow along the path of structures; a longer one is no field.
constexpr std::size_t most_key = 4096;

// The namespaces in scope: those a node declares, prefix to URI, and the scope around it, which is
// looked up after them and never copied.
struct Scope {
    const Scope* outer = nullptr;
    std::map<std::string, std::string, std::less<>> own;
};

// A name resolved: its namespace, the prefix the file gave it and the local name.
struct Name {
    std::string space;
    std::string prefix;
    std::string local;
};

// The scope of a node: the one around it with its own xmlns declarations.
[[nodiscard]] Scope scoped(const xml::Node& node, const Scope& outer) {
    Scope made{&outer, {}};

    for (const xml::Attribute& one : node.attributes) {
        if (one.name.starts_with("xmlns:")) made.own[one.name.substr(6)] = one.value;
        else if (one.name == "xmlns") made.own[""] = one.value;
    }

    return made;
}

// A qualified name resolved in a scope; "xml" is always bound, and a name of an attribute without
// a prefix is in no namespace, as XML has it.
[[nodiscard]] Name resolved(std::string_view qualified, const Scope& scope, bool element = true) {
    const std::size_t colon = qualified.find(':');
    const std::string prefix{colon == std::string_view::npos ? std::string_view{} : qualified.substr(0, colon)};
    const std::string local{colon == std::string_view::npos ? qualified : qualified.substr(colon + 1)};

    if (prefix == "xml") return {std::string{xml_space}, prefix, local};

    if (colon == std::string_view::npos && !element) return {std::string{}, prefix, local};

    for (const Scope* at = &scope; at != nullptr; at = at->outer) {
        const auto bound = at->own.find(prefix);

        if (bound != at->own.end()) return {bound->second, prefix, local};
    }

    return {std::string{}, prefix, local};
}

// Whether a name is the RDF name `local`.
[[nodiscard]] bool is_rdf(const Name& name, std::string_view local) { return name.space == rdf && name.local == local; }

// The value of the RDF attribute `local` of a node, whatever prefix the file bound RDF to; null
// when there is none.
[[nodiscard]] const std::string* rdf_attribute(const xml::Node& node, const Scope& scope, std::string_view local) {
    for (const xml::Attribute& one : node.attributes)
        if (is_rdf(resolved(one.name, scope, false), local)) return &one.value;

    return nullptr;
}

// Degrees from a coordinate of XMP: "DDD,MM,SSk" or "DDD,MM.mmk", k one of N, S, E and W; none
// for anything else.
[[nodiscard]] std::optional<double> coordinate(std::string_view text) {
    if (text.empty()) return std::nullopt;

    const char side = text.back();

    if (side != 'N' && side != 'S' && side != 'E' && side != 'W') return std::nullopt;

    text.remove_suffix(1);

    double made = 0;
    double scale = 1;
    std::size_t parts = 0;

    while (!text.empty() && parts < 3) {
        const std::size_t comma = text.find(',');
        const std::string_view part = text.substr(0, comma);
        double value = 0;
        double fraction = 0;
        bool dot = false;
        bool digits = false;

        for (const char one : part) {
            if (one == '.' && !dot) {
                dot = true;
                fraction = 1;
            } else if (one >= '0' && one <= '9') {
                digits = true;

                if (dot) {
                    fraction /= 10;
                    value += (one - '0') * fraction;
                } else {
                    value = value * 10 + (one - '0');
                }
            } else {
                return std::nullopt;
            }
        }

        if (!digits) return std::nullopt;

        made += value / scale;
        scale *= 60;
        ++parts;
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
    }

    if (parts < 2 || !text.empty()) return std::nullopt;

    return side == 'S' || side == 'W' ? -made : made;
}

// A fraction of XMP ("1234/10") or a plain number as a number; none for anything else.
[[nodiscard]] std::optional<double> fraction(std::string_view text) {
    const std::size_t slash = text.find('/');
    const auto whole = [](std::string_view digits) -> std::optional<double> {
        if (digits.empty() || digits.size() > 18) return std::nullopt;

        double made = 0;

        for (const char one : digits) {
            if (one < '0' || one > '9') return std::nullopt;

            made = made * 10 + (one - '0');
        }

        return made;
    };

    const std::optional<double> over = whole(text.substr(0, slash));

    if (slash == std::string_view::npos || !over) return over;

    const std::optional<double> under = whole(text.substr(slash + 1));

    if (!under || *under == 0) return std::nullopt;

    return *over / *under;
}

// The reader of one packet: what it gathers, the photo, and what it noted of the place.
class Packet {
public:
    // A reader into `out` and `shot`.
    Packet(Gathered& out, Shot& shot) : out_(out), shot_(shot) {}

    void read(const xml::Document& document);

private:
    void found(const xml::Node& node, const Scope& scope, std::size_t depth);

    void described(const xml::Node& description, const Scope& scope, const std::string* path, std::size_t depth);

    void property(const xml::Node& node, const Name& name, const Scope& scope, const std::string* path, std::size_t depth);

    void valued(const xml::Node& node, const std::string& key, const Scope& scope, std::size_t depth);

    void simple(const Name& name, const std::string& key, bool top, std::string value);

    void placed();

    Gathered& out_;
    Shot& shot_;
    std::optional<double> latitude_;
    std::optional<double> longitude_;
    std::optional<double> altitude_;
    bool below_ = false;
    std::string created_;
    std::string original_;
};

// Whether the key of a property would be longer than `most_key`.
[[nodiscard]] bool overlong(const Name& name, const std::string* path) {
    return (path == nullptr ? 0 : path->size()) + name.prefix.size() + name.local.size() + 2 > most_key;
}

// The key of a property: at the top, "Xmp.", the prefix and the name; inside a structure, the path
// and the name with the file's prefix.
[[nodiscard]] std::string keyed(const Name& name, const std::string* path) {
    if (path != nullptr) return *path + "/" + name.prefix + ":" + name.local;

    const std::string_view known = space_prefix(name.space);

    return "Xmp." + std::string{known.empty() ? std::string_view{name.prefix} : known} + "." + name.local;
}

// Reads every rdf:Description under rdf:RDF, wherever rdf:RDF stands, then what was learned of
// the photo.
void Packet::read(const xml::Document& document) {
    for (const xml::Node& node : document.nodes) found(node, {}, 0);

    if (!created_.empty()) shot_.xmp.taken = created_;
    else if (!original_.empty()) shot_.xmp.taken = original_;

    placed();
}

// Looks for rdf:RDF in a node and under it, and reads its descriptions.
void Packet::found(const xml::Node& node, const Scope& scope, std::size_t depth) {
    if (node.kind != xml::Kind::element || depth > most_depth) return;

    const Scope inner = scoped(node, scope);
    const Name name = resolved(node.name, inner);

    if (!is_rdf(name, "RDF")) {
        for (const xml::Node& child : node.children) found(child, inner, depth + 1);

        return;
    }

    for (const xml::Node& child : node.children) {
        if (child.kind != xml::Kind::element) continue;

        const Scope at = scoped(child, inner);

        if (is_rdf(resolved(child.name, at), "Description")) described(child, at, nullptr, 0);
    }
}

// The properties of a description, or of a structure under `path`: its attributes that are not
// RDF's, XML's or declarations, and its child elements.
void Packet::described(const xml::Node& description, const Scope& scope, const std::string* path, std::size_t depth) {
    for (const xml::Attribute& one : description.attributes) {
        if (one.name == "xmlns" || one.name.starts_with("xmlns:")) continue;

        const Name name = resolved(one.name, scope, false);

        if (name.space == rdf || name.space == xml_space || name.space.empty() || overlong(name, path)) continue;

        simple(name, keyed(name, path), path == nullptr, one.value);
    }

    for (const xml::Node& child : description.children) {
        if (child.kind != xml::Kind::element) continue;

        const Scope inner = scoped(child, scope);

        property(child, resolved(child.name, inner), inner, path, depth);
    }
}

// One property element under its key.
void Packet::property(const xml::Node& node, const Name& name, const Scope& scope, const std::string* path, std::size_t depth) {
    if (name.space.empty() || name.space == rdf || overlong(name, path)) return;

    const std::string key = keyed(name, path);

    const std::string* resource = rdf_attribute(node, scope, "resource");

    if (resource != nullptr) {
        simple(name, key, path == nullptr, *resource);

        return;
    }

    bool elements = false;

    for (const xml::Node& child : node.children) elements = elements || child.kind == xml::Kind::element;

    if (!elements) {
        bool parts = false;

        for (const xml::Attribute& one : node.attributes) {
            const Name attribute = resolved(one.name, scope, false);

            parts = parts || (!attribute.space.empty() && attribute.space != rdf && attribute.space != xml_space && !one.name.starts_with("xmlns:"));
        }

        if (parts) described(node, scope, &key, depth + 1);
        else simple(name, key, path == nullptr, xml::text(node));

        return;
    }

    if (depth >= most_depth) return;

    const std::string* type = rdf_attribute(node, scope, "parseType");

    if (type != nullptr && *type == "Resource") {
        described(node, scope, &key, depth + 1);

        return;
    }

    for (const xml::Node& child : node.children) {
        if (child.kind != xml::Kind::element) continue;

        const Scope inner = scoped(child, scope);
        const Name inside = resolved(child.name, inner);

        if (is_rdf(inside, "Description")) {
            described(child, inner, &key, depth + 1);
        } else if (is_rdf(inside, "Bag") || is_rdf(inside, "Seq") || is_rdf(inside, "Alt")) {
            std::vector<const xml::Node*> items;
            const xml::Node* chosen = nullptr;

            for (const xml::Node& item : child.children)
                if (item.kind == xml::Kind::element && is_rdf(resolved(item.name, scoped(item, inner)), "li")) items.push_back(&item);

            if (is_rdf(inside, "Alt")) {
                for (const xml::Node* item : items) {
                    const std::string* language = xml::attribute(*item, "xml:lang");

                    if (chosen == nullptr && language != nullptr && *language == "x-default") chosen = item;
                }

                if (chosen == nullptr && !items.empty()) chosen = items.front();

                items = chosen == nullptr ? std::vector<const xml::Node*>{} : std::vector<const xml::Node*>{chosen};
            }

            for (std::size_t at = 0; at < items.size(); ++at) {
                const std::string indexed = key + "[" + std::to_string(at + 1) + "]";

                valued(*items[at], is_rdf(inside, "Alt") ? key : indexed, scoped(*items[at], inner), depth + 1);
            }
        }

        return;
    }
}

// An item of an array: a simple value under the array's own key, a structure (of elements, of
// parseType="Resource" or of attributes, as Adobe writes the events of xmpMM:History) under the
// item's indexed key.
void Packet::valued(const xml::Node& node, const std::string& key, const Scope& scope, std::size_t depth) {
    const std::string* type = rdf_attribute(node, scope, "parseType");
    bool elements = false;
    bool parts = false;

    for (const xml::Node& child : node.children) elements = elements || child.kind == xml::Kind::element;

    for (const xml::Attribute& one : node.attributes) {
        const Name attribute = resolved(one.name, scope, false);

        parts = parts || (!attribute.space.empty() && attribute.space != rdf && attribute.space != xml_space && !one.name.starts_with("xmlns:"));
    }

    if (parts && !elements) {
        if (depth < most_depth) described(node, scope, &key, depth);

        return;
    }

    if ((type != nullptr && *type == "Resource") || elements) {
        if (depth >= most_depth) return;

        if (type != nullptr && *type == "Resource") {
            described(node, scope, &key, depth);

            return;
        }

        for (const xml::Node& child : node.children) {
            if (child.kind != xml::Kind::element) continue;

            const Scope inner = scoped(child, scope);

            if (is_rdf(resolved(child.name, inner), "Description")) described(child, inner, &key, depth);
        }

        return;
    }

    const std::size_t bracket = key.rfind('[');
    const std::string own = bracket != std::string::npos && key.back() == ']' ? key.substr(0, bracket) : key;

    out_.field(Scheme::xmp, own, xml::text(node));
}

// A simple value: a field, and, at the top, what it says of the photo.
void Packet::simple(const Name& name, const std::string& key, bool top, std::string value) {
    if (top && name.space == tiff_space && name.local == "Orientation" && value.size() == 1 && value[0] >= '1' && value[0] <= '8')
        shot_.xmp.orientation = static_cast<std::uint8_t>(value[0] - '0');

    if (top && name.space == tiff_space && name.local == "Make") shot_.xmp.make = value;

    if (top && name.space == note_space && name.local == "HasExtendedXMP") shot_.extended = value;

    if (top && name.space == tiff_space && name.local == "Model") shot_.xmp.model = value;

    if (top && name.space == photoshop_space && name.local == "DateCreated") created_ = value;

    if (top && name.space == exif_space && name.local == "DateTimeOriginal") original_ = value;

    if (top && name.space == exif_space && name.local == "GPSLatitude") latitude_ = coordinate(value);

    if (top && name.space == exif_space && name.local == "GPSLongitude") longitude_ = coordinate(value);

    if (top && name.space == exif_space && name.local == "GPSAltitude") altitude_ = fraction(value);

    if (top && name.space == exif_space && name.local == "GPSAltitudeRef") below_ = value == "1";

    out_.field(Scheme::xmp, key, std::move(value));
}

// The place, when both coordinates were read.
void Packet::placed() {
    if (!latitude_ || !longitude_) return;

    Place place{*latitude_, *longitude_, std::nullopt};

    if (altitude_) place.altitude = below_ ? -*altitude_ : *altitude_;

    shot_.xmp.place = place;
}

}

// Reads a packet of XMP into fields and what it says of the photo; one that is not well-formed
// XML damages the file.
void xmp(std::string_view packet, Gathered& out, Shot& shot) {
    xml::Document document;

    try {
        document = xml::parsed(packet, xml::Limits{most_packet, 128, std::size_t{1} << 20});
    } catch (const Trouble&) {
        out.damaged();

        return;
    }

    Packet{out, shot}.read(document);
}

namespace {

// The packet of a file without XMP, to which changes are made.
constexpr std::string_view empty_packet =
    "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?><x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
    "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description rdf:about=\"\"/></rdf:RDF></x:xmpmeta>"
    "<?xpacket end=\"w\"?>";

// How a property is written: a simple value, an ordered array, an unordered array, or an
// alternative of languages.
enum class Form : std::uint8_t { simple, seq, bag, language };

// The form of the properties of Dublin Core and Photoshop whose form the schema fixes; any other
// property is simple with one value and an unordered array with several.
[[nodiscard]] Form form_of(std::string_view space, std::string_view local, std::size_t values) {
    constexpr std::string_view dc = "http://purl.org/dc/elements/1.1/";
    constexpr std::string_view rights = "http://ns.adobe.com/xap/1.0/rights/";

    if ((space == dc && (local == "title" || local == "description" || local == "rights")) || (space == rights && local == "UsageTerms"))
        return Form::language;

    if (space == dc && (local == "creator" || local == "date")) return Form::seq;

    if ((space == dc && (local == "subject" || local == "publisher" || local == "contributor" || local == "language" || local == "relation" || local == "type")) ||
        (space == photoshop_space && local == "SupplementalCategories"))
        return Form::bag;

    return values > 1 ? Form::bag : Form::simple;
}

// A value as text.
[[nodiscard]] std::string texted(const Value& one) { return std::holds_alternative<std::string>(one) ? std::get<std::string>(one) : std::to_string(std::get<std::int64_t>(one)); }

// Whether `name` is a name XML allows without a prefix: a letter, an underscore or a character
// beyond ASCII first, then those, digits, dots and hyphens; no colon, no space.
[[nodiscard]] bool plain_name(std::string_view name) {
    const auto letter = [](char one) { return (one >= 'A' && one <= 'Z') || (one >= 'a' && one <= 'z') || one == '_' || static_cast<unsigned char>(one) >= 0x80; };

    if (name.empty() || !letter(name.front())) return false;

    return std::ranges::all_of(name, [&letter](char one) { return letter(one) || (one >= '0' && one <= '9') || one == '.' || one == '-'; });
}

// Whether text of UTF-8 holds only characters XML 1.0 allows: no controls but tab, line feed and
// carriage return, and neither U+FFFE nor U+FFFF.
[[nodiscard]] bool xml_text(std::string_view text) {
    for (std::size_t at = 0; at < text.size(); ++at) {
        const auto one = static_cast<unsigned char>(text[at]);

        if (one < 0x20 && one != '\t' && one != '\n' && one != '\r') return false;

        if (one == 0xEF && text.substr(at + 1, 1) == "\xBF" && text.size() > at + 2 && (text[at + 2] == '\xBE' || text[at + 2] == '\xBF')) return false;
    }

    return true;
}

// An element with a name and its children.
[[nodiscard]] xml::Node element(std::string name, std::vector<xml::Attribute> attributes = {}, std::vector<xml::Node> children = {}) {
    return xml::Node{xml::Kind::element, std::move(name), {}, std::move(attributes), std::move(children)};
}

// A text node.
[[nodiscard]] xml::Node texting(std::string value) { return xml::Node{xml::Kind::text, {}, std::move(value), {}, {}}; }

// The names of the properties at the top of a packet: the namespace and the local name of each
// attribute and child element of every rdf:Description.
void named_in(const xml::Node& node, const Scope& scope, std::vector<std::pair<std::string, std::string>>& into, std::size_t depth) {
    if (node.kind != xml::Kind::element || depth > most_depth) return;

    const Scope inner = scoped(node, scope);

    if (!is_rdf(resolved(node.name, inner), "Description")) {
        for (const xml::Node& child : node.children) named_in(child, inner, into, depth + 1);

        return;
    }

    for (const xml::Attribute& one : node.attributes) {
        const Name name = resolved(one.name, inner, false);

        if (!name.space.empty() && name.space != rdf && name.space != xml_space) into.emplace_back(name.space, name.local);
    }

    for (const xml::Node& child : node.children)
        if (child.kind == xml::Kind::element) {
            const Name name = resolved(child.name, scoped(child, inner));

            into.emplace_back(name.space, name.local);
        }
}

// The rewriting of one packet: the document, where its descriptions are with their scopes.
class Rewriting {
public:
    // Over a parsed packet; one without rdf:RDF cannot be rewritten.
    explicit Rewriting(xml::Document& document);

    void change(const Change& one, const std::vector<std::pair<std::string, std::string>>& extended);

private:
    void found(xml::Node& node, const Scope& scope, std::size_t depth);

    [[nodiscard]] std::string prefix_of(std::string_view space, std::string_view wanted);

    struct Described {
        xml::Node* node = nullptr;
        Scope scope;
    };

    Scope outermost_;
    std::vector<Described> descriptions_;
    std::vector<std::unique_ptr<Scope>> scopes_;
    std::string rdf_prefix_ = "rdf";
};

// Finds every rdf:Description and keeps it with the scope around it.
Rewriting::Rewriting(xml::Document& document) {
    for (xml::Node& node : document.nodes) found(node, outermost_, 0);

    if (descriptions_.empty()) throw Trouble{Kind::malformed, "The XMP of the file has no rdf:Description."};
}

// Looks for descriptions in a node and under it; the scopes they stand in are kept alive here, the
// outermost one as a member, since each scope points to the one around it.
void Rewriting::found(xml::Node& node, const Scope& scope, std::size_t depth) {
    if (node.kind != xml::Kind::element || depth > most_depth) return;

    scopes_.push_back(std::make_unique<Scope>(scoped(node, scope)));

    const Scope& inner = *scopes_.back();
    const Name name = resolved(node.name, inner);

    if (is_rdf(name, "Description")) {
        descriptions_.push_back({&node, inner});

        if (!name.prefix.empty()) rdf_prefix_ = name.prefix;

        return;
    }

    for (xml::Node& child : node.children) found(child, inner, depth + 1);
}

// The prefix bound to the namespace in the scope of the first description, declared there under
// `wanted` (or `wanted` with a number, when `wanted` is bound elsewhere) when none is.
std::string Rewriting::prefix_of(std::string_view space, std::string_view wanted) {
    Described& first = descriptions_.front();

    for (const Scope* at = &first.scope; at != nullptr; at = at->outer)
        for (const auto& [prefix, uri] : at->own)
            if (uri == space && !prefix.empty() && resolved(prefix + ":x", first.scope).space == space) return prefix;

    std::string made{wanted};

    for (int number = 1; !resolved(made + ":x", first.scope).space.empty(); ++number) made = std::string{wanted} + std::to_string(number);

    first.node->attributes.push_back({"xmlns:" + made, std::string{space}});
    first.scope.own[made] = std::string{space};

    return made;
}

// Makes one change: the property goes from every description, and comes back in the first with
// the new values in its form, when there are any; of an alternative of languages only the default
// is new, and the other languages it had stay, after it. A key that names no property of XML and a
// value with a character XML does not allow are malformed.
void Rewriting::change(const Change& one, const std::vector<std::pair<std::string, std::string>>& extended) {
    if (!one.key.starts_with("Xmp.") || one.key.find_first_of("/[") != std::string::npos) throw Trouble{Kind::unsupported, "Only simple properties and arrays of XMP are written."};

    const std::size_t dot = one.key.find('.', 4);

    if (dot == std::string::npos) throw Trouble{Kind::unsupported, "A key of XMP has a prefix and a name."};

    const std::string prefix = one.key.substr(4, dot - 4);
    const std::string local = one.key.substr(dot + 1);

    if (!plain_name(prefix) || !plain_name(local)) throw Trouble{Kind::malformed, "The key " + one.key + " does not name a property of XML."};

    for (const Value& value : one.values)
        if (!xml_text(texted(value))) throw Trouble{Kind::malformed, "A value of XMP holds a character XML does not allow."};
    std::string space{space_uri(prefix)};

    for (const Described& each : descriptions_)
        if (space.empty()) space = resolved(prefix + ":x", each.scope).space;

    if (space.empty()) throw Trouble{Kind::unsupported, "The namespace of the prefix " + prefix + " is not known."};

    if (std::ranges::find(extended, std::pair{space, local}) != extended.end()) throw Trouble{Kind::unsupported, "The property lies in the extended XMP, which is not written."};

    const Form form = form_of(space, local, one.values.size());
    const std::string li = rdf_prefix_ + ":li";
    std::vector<xml::Node> languages;

    for (const Described& each : descriptions_)
        for (const xml::Node& child : each.node->children) {
            if (form != Form::language || child.kind != xml::Kind::element) continue;

            const Scope inner = scoped(child, each.scope);
            const Name name = resolved(child.name, inner);

            if (name.space != space || name.local != local) continue;

            for (const xml::Node& alternative : child.children) {
                if (alternative.kind != xml::Kind::element) continue;

                const Scope around = scoped(alternative, inner);

                if (!is_rdf(resolved(alternative.name, around), "Alt")) continue;

                for (const xml::Node& item : alternative.children) {
                    if (item.kind != xml::Kind::element) continue;

                    const Scope within = scoped(item, around);

                    if (!is_rdf(resolved(item.name, within), "li")) continue;

                    std::string language;
                    std::string text;

                    for (const xml::Attribute& attribute : item.attributes) {
                        const Name named = resolved(attribute.name, within, false);

                        if (named.space == xml_space && named.local == "lang") language = attribute.value;
                    }

                    for (const xml::Node& part : item.children)
                        if (part.kind == xml::Kind::text) text += part.value;

                    if (!language.empty() && language != "x-default") languages.push_back(element(li, {{"xml:lang", language}}, {texting(text)}));
                }
            }
        }

    for (Described& each : descriptions_) {
        std::erase_if(each.node->attributes, [&](const xml::Attribute& attribute) {
            const Name name = resolved(attribute.name, each.scope, false);

            return name.space == space && name.local == local;
        });
        std::erase_if(each.node->children, [&](const xml::Node& child) {
            if (child.kind != xml::Kind::element) return false;

            const Name name = resolved(child.name, scoped(child, each.scope));

            return name.space == space && name.local == local;
        });
    }

    if (one.values.empty()) return;

    const std::string name = prefix_of(space, prefix) + ":" + local;

    if (form == Form::simple) {
        descriptions_.front().node->children.push_back(element(name, {}, {texting(texted(one.values.front()))}));

        return;
    }

    if (form == Form::language) {
        if (one.values.size() != 1) throw Trouble{Kind::malformed, "An alternative of languages takes one value, the default."};

        languages.insert(languages.begin(), element(li, {{"xml:lang", "x-default"}}, {texting(texted(one.values.front()))}));
        descriptions_.front().node->children.push_back(element(name, {}, {element(rdf_prefix_ + ":Alt", {}, std::move(languages))}));

        return;
    }

    std::vector<xml::Node> items;

    for (const Value& value : one.values) items.push_back(element(li, {}, {texting(texted(value))}));

    descriptions_.front().node->children.push_back(element(name, {}, {element(rdf_prefix_ + (form == Form::seq ? ":Seq" : ":Bag"), {}, std::move(items))}));
}

}

// The packet `given` (empty: a new one) with the changes made, written again without an XML
// declaration, which a packet does not carry, and with two kilobytes of padding before its end, as
// the standard recommends, so that the next change can be made in place; less padding, or none,
// when the packet would grow past `most` bytes. A change of a property that lies in the extended
// packet `extension` is not made, since that packet is not written. Throws `Trouble`: unsupported
// for a structure, an unknown prefix or a property of the extension, malformed for a packet that
// is not XML, an alternative of more than one value, a bad name or a bad character.
std::string xmp_rewritten(std::string_view given, const std::vector<Change>& changes, std::string_view extension, std::size_t most) {
    xml::Document document = xml::parsed(given.empty() ? empty_packet : given, xml::Limits{most_packet, 128, std::size_t{1} << 20});
    std::vector<std::pair<std::string, std::string>> extended;

    if (!extension.empty()) {
        const xml::Document other = xml::parsed(extension, xml::Limits{most_packet, 128, std::size_t{1} << 20});

        for (const xml::Node& node : other.nodes) named_in(node, {}, extended, 0);
    }

    Rewriting rewriting{document};

    for (const Change& one : changes) rewriting.change(one, extended);

    std::string made = xml::written(document);

    if (made.starts_with("<?xml ")) {
        made.erase(0, made.find("?>") + 2);

        if (made.starts_with("\n")) made.erase(0, 1);
    }

    const std::size_t end = made.rfind("<?xpacket end");

    if (end != std::string::npos && made.size() < most) {
        const std::size_t lines = std::min<std::size_t>(20, (most - made.size() - 1) / 101);
        std::string padding;

        for (std::size_t line = 0; line < lines; ++line) padding += "\n" + std::string(100, ' ');

        made.insert(end, padding + "\n");
    }

    return made;
}

}
