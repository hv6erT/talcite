#include <cstddef>
#include <cstdint>
#include <map>
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

}
