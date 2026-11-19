#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/trouble.hpp"

#include "inflating.hpp"
#include "tagging.hpp"

// The containers of photos and where their metadata stands. JPEG: the segments up to the start of
// the scan: APP1 with "Exif\0\0" and a TIFF structure, APP1 with XMP, APP1 with pieces of extended
// XMP (of the GUID the main packet, which comes before them, names; joined when every byte came),
// APP13 with Photoshop's resources (joined across adjacent segments, as Photoshop splits them, and
// read where the run of them ends, so that fields come in file order), and the dimensions from the
// first frame header (SOF). PNG: IHDR, eXIf, the text chunks with XMP
// ("XML:com.adobe.xmp") and with ImageMagick's "Raw profile type" (EXIF, IPTC, XMP, Photoshop's
// resources written in hexadecimal), compressed ones inflated within the budget of the file. WebP:
// the canvas of VP8X, or the frame of VP8 or VP8L, the EXIF and XMP chunks. TIFF and the raw files
// built on it: the structure is the file. HEIF and AVIF: the items of the meta box: the primary
// item's dimensions (ispe), the Exif item and the XMP item (mime, application/rdf+xml), read
// from where iloc places them, in the file or in idat.
//
// Each container makes the photo of its file from what EXIF, XMP and IPTC said, in that order of
// trust (`photo_of`).
namespace tal::meta::tagging {

namespace {

constexpr std::string_view exif_mark{"Exif\0\0", 6};
constexpr std::string_view xmp_mark{"http://ns.adobe.com/xap/1.0/\0", 29};
constexpr std::string_view extension_mark{"http://ns.adobe.com/xmp/extension/\0", 35};
constexpr std::string_view photoshop_mark{"Photoshop 3.0\0", 14};

// How many items of HEIF are looked at, and how many extents one may have.
constexpr std::size_t most_items = std::size_t{1} << 12;
constexpr std::size_t most_extents = 64;

// A byte as an unsigned number.
[[nodiscard]] std::uint32_t sample(std::byte one) { return std::to_integer<std::uint32_t>(one); }

// Whether the bytes start with the mark.
[[nodiscard]] bool marked(std::span<const std::byte> given, std::string_view mark) {
    return given.size() >= mark.size() && std::equal(mark.begin(), mark.end(), given.begin(), [](char one, std::byte other) { return static_cast<std::byte>(one) == other; });
}

// The bytes as text.
[[nodiscard]] std::string_view text_of(std::span<const std::byte> given) { return {reinterpret_cast<const char*>(given.data()), given.size()}; }

// Reads a TIFF structure held in memory.
[[nodiscard]] Task<void> held_exif(Bytes data, Gathered& out, Shot& shot) {
    Held held{std::move(data)};
    const std::uint64_t size = co_await held.size();

    const bool read = co_await exif(held, size, 0, size, false, out, shot);

    if (!read) out.damaged();
}

// The pieces of an extended XMP packet: their full length and the bytes that came, by offset.
struct Extension {
    std::uint64_t length = 0;
    std::map<std::uint64_t, Bytes> pieces;
};

// Keeps a piece of the extension that the main packet named: one that lies within the packet's
// length, which is at most `most_packet`, and does not overlap a piece kept before, so that what
// is kept is never more than the packet. A piece that breaks this is left, and the packet will
// not be whole.
void placed(Extension& extension, std::uint64_t length, std::uint64_t at, std::span<const std::byte> piece) {
    if (length > most_packet || (extension.length != 0 && extension.length != length) || at > length || piece.size() > length - at) return;

    extension.length = length;

    const auto after = extension.pieces.lower_bound(at);

    if (after != extension.pieces.end() && after->first < at + piece.size()) return;

    if (after != extension.pieces.begin() && std::prev(after)->first + std::prev(after)->second.size() > at) return;

    extension.pieces.emplace(at, Bytes(piece.begin(), piece.end()));
}

// The packet of an extension, when its pieces cover every byte once; none otherwise.
[[nodiscard]] std::optional<std::string> joined(const Extension& extension) {
    if (extension.length == 0 || extension.length > most_packet) return std::nullopt;

    std::string made;
    std::uint64_t next = 0;

    for (const auto& [at, piece] : extension.pieces) {
        if (at != next || piece.size() > extension.length - at) return std::nullopt;

        made.append(text_of(piece));
        next = at + piece.size();
    }

    if (next != extension.length) return std::nullopt;

    return made;
}

// The content of ImageMagick's "Raw profile type": a line with the name, a line with the length in
// bytes, then the bytes in hexadecimal over many lines; none when the digits do not make the
// length.
[[nodiscard]] std::optional<Bytes> raw_profile(std::string_view text) {
    std::size_t at = text.find_first_not_of("\n\r");

    if (at == std::string_view::npos) return std::nullopt;

    at = text.find('\n', at);

    if (at == std::string_view::npos) return std::nullopt;

    std::size_t length = 0;
    bool digits = false;

    for (++at; at < text.size() && (text[at] == ' ' || (text[at] >= '0' && text[at] <= '9')); ++at) {
        if (text[at] == ' ') {
            if (digits) break;

            continue;
        }

        digits = true;
        length = length * 10 + static_cast<std::size_t>(text[at] - '0');

        if (length > most_block) return std::nullopt;
    }

    if (!digits) return std::nullopt;

    Bytes made;
    int high = -1;

    made.reserve(length);

    for (; at < text.size() && made.size() < length; ++at) {
        const char one = text[at];
        int value = -1;

        if (one >= '0' && one <= '9') value = one - '0';
        else if (one >= 'a' && one <= 'f') value = one - 'a' + 10;
        else if (one >= 'A' && one <= 'F') value = one - 'A' + 10;
        else if (one == ' ' || one == '\n' || one == '\r' || one == '\t') continue;
        else return std::nullopt;

        if (high < 0) {
            high = value;
        } else {
            made.push_back(static_cast<std::byte>(high * 16 + value));
            high = -1;
        }
    }

    if (made.size() != length) return std::nullopt;

    return made;
}

// The photo of a file whose dimensions the container knew.
[[nodiscard]] Photo shaped(Shot& shot, std::uint32_t width, std::uint32_t height) {
    shot.width = width;
    shot.height = height;

    return photo_of(shot);
}

// A big-endian unsigned integer of `width` bytes (0, 4 or 8, as HEIF's sizes are) from the walk.
[[nodiscard]] std::uint64_t sized(Walk& walk, std::size_t width) {
    if (width == 4) return walk.be32();

    if (width == 8) return walk.be64();

    return 0;
}

// Where an item of HEIF lies: how it is built (0 in the file, 1 in idat), from where, its pieces.
struct Located {
    std::uint32_t method = 0;
    std::uint64_t base = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> extents;
};

// An item of HEIF: its type and, for "mime", its content type.
struct Item {
    std::string type;
    std::string content;
};

// The meta box of HEIF, read: the primary item, the items, where they lie, the properties that
// are dimensions (by their number in ipco, from 1), the properties of each item, where idat lies.
struct Meta {
    std::uint32_t primary = 0;
    std::map<std::uint32_t, Item> items;
    std::map<std::uint32_t, Located> places;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> extents;
    std::map<std::uint32_t, std::vector<std::uint32_t>> properties;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> idat;
};

// A string up to a zero from the walk, at most what is left.
[[nodiscard]] std::string zeroed(Walk& walk) {
    std::string made;

    while (walk.left() > 0) {
        const std::uint8_t one = walk.u8();

        if (one == 0) break;

        made.push_back(static_cast<char>(one));
    }

    return made;
}

// The item information (iinf): each infe of version 2 or 3, its number, type and content type.
[[nodiscard]] Task<void> informed(Walk& body, Meta& meta, Gathered& out) {
    co_await body.need(4 + 4);

    const std::uint8_t version = body.u8();

    body.skip(3);
    body.skip(version == 0 ? 2 : 4);

    while (meta.items.size() < most_items) {
        std::optional<Box> entry = co_await boxed(body, out);

        if (!entry) break;

        if (entry->type != "infe") continue;

        Walk& infe = entry->body;

        if (infe.left() > most_text) continue;

        co_await infe.need(static_cast<std::size_t>(infe.left()));

        const std::uint8_t kind = infe.u8();

        infe.skip(3);

        if (kind < 2) continue;

        const std::uint32_t number = kind == 2 ? infe.be16() : infe.be32();

        infe.skip(2);

        Item item;

        item.type = infe.chars(4);
        (void)zeroed(infe);

        if (item.type == "mime") item.content = zeroed(infe);

        if (!infe.spoiled()) meta.items[number] = std::move(item);
    }
}

// The item locations (iloc), versions 0 to 2.
[[nodiscard]] Task<void> located(Walk& body, Meta& meta, Gathered& out) {
    if (body.left() > most_block) co_return;

    co_await body.need(static_cast<std::size_t>(body.left()));

    const std::uint8_t version = body.u8();

    body.skip(3);

    const std::uint8_t sizes = body.u8();
    const std::uint8_t more = body.u8();
    const std::size_t offset_size = sizes >> 4;
    const std::size_t length_size = sizes & 0xF;
    const std::size_t base_size = more >> 4;
    const std::size_t index_size = version >= 1 ? more & 0xF : 0;

    if (version > 2) co_return;

    const std::uint32_t many = version < 2 ? body.be16() : body.be32();

    for (std::uint32_t one = 0; one < many && one < most_items && !body.spoiled(); ++one) {
        Located place;
        const std::uint32_t number = version < 2 ? body.be16() : body.be32();

        if (version >= 1) place.method = body.be16() & 0xF;

        body.skip(2);
        place.base = sized(body, base_size);

        const std::uint16_t extents = body.be16();

        for (std::uint16_t piece = 0; piece < extents && !body.spoiled(); ++piece) {
            (void)sized(body, index_size);

            const std::uint64_t at = sized(body, offset_size);
            const std::uint64_t much = sized(body, length_size);

            if (place.extents.size() < most_extents) place.extents.emplace_back(at, much);
        }

        if (!body.spoiled()) meta.places[number] = std::move(place);
    }

    if (body.spoiled()) out.damaged();
}

// The item properties (iprp): the dimensions in ipco (ispe), by number, and which properties
// each item has (ipma).
[[nodiscard]] Task<void> propertied(Walk& body, Meta& meta, Gathered& out) {
    while (true) {
        std::optional<Box> box = co_await boxed(body, out);

        if (!box) break;

        if (box->type == "ipco") {
            std::uint32_t number = 0;

            while (number < most_items) {
                std::optional<Box> property = co_await boxed(box->body, out);

                if (!property) break;

                ++number;

                if (property->type != "ispe") continue;

                co_await property->body.need(4 + 8);
                property->body.skip(4);

                const std::uint32_t width = property->body.be32();
                const std::uint32_t height = property->body.be32();

                if (!property->body.spoiled()) meta.extents[number] = {width, height};
            }
        } else if (box->type == "ipma") {
            Walk& map = box->body;

            if (map.left() > most_block) continue;

            co_await map.need(static_cast<std::size_t>(map.left()));

            const std::uint8_t version = map.u8();
            const std::uint32_t flags = map.be24();
            const std::uint32_t many = map.be32();

            for (std::uint32_t one = 0; one < many && one < most_items && !map.spoiled(); ++one) {
                const std::uint32_t number = version < 1 ? map.be16() : map.be32();
                const std::uint8_t associations = map.u8();
                std::vector<std::uint32_t>& those = meta.properties[number];

                for (std::uint8_t at = 0; at < associations; ++at) those.push_back((flags & 1) != 0 ? map.be16() & 0x7FFF : map.u8() & 0x7F);
            }
        }
    }
}

// The bytes of an item, at most `most_block`; none when it lies nowhere we read, outside the file
// (in the file) or outside idat (in idat), or past what is left of `budget`, which it spends: the
// items read from one file add up to at most its size, since items that keep the rules do not share
// their bytes. An extent of length zero reaches to the end of where it lies, as the standard says.
[[nodiscard]] Task<std::optional<Bytes>> item_data(Reader& from, std::uint64_t size, const Meta& meta, std::uint32_t number, std::uint64_t& budget) {
    const auto place = meta.places.find(number);

    if (place == meta.places.end() || place->second.method > 1) co_return std::nullopt;

    std::uint64_t origin = place->second.base;
    std::uint64_t room = size;

    if (place->second.method == 1) {
        if (!meta.idat || origin > meta.idat->second) co_return std::nullopt;

        room = meta.idat->second - origin;
        origin += meta.idat->first;
    } else {
        if (origin > size) co_return std::nullopt;

        room = size - origin;
    }

    Bytes made;

    for (const auto& [at, length] : place->second.extents) {
        if (at > room) co_return std::nullopt;

        const std::uint64_t much = length == 0 ? room - at : length;

        if (much > room - at || much > most_block - made.size() || much > budget) co_return std::nullopt;

        budget -= much;

        const Bytes piece = co_await from.read(origin + at, static_cast<std::size_t>(much));

        if (piece.size() < much) co_return std::nullopt;

        made.insert(made.end(), piece.begin(), piece.end());
    }

    co_return made;
}

}

// The photo from what EXIF, XMP and IPTC said, each trusted before the next: the orientation (1
// when none says), the camera without the spaces that pad it, the time, the place; the dimensions
// are the container's.
Photo photo_of(const Shot& shot) {
    const auto trimmed = [](std::string text) {
        while (!text.empty() && (text.back() == ' ' || text.back() == '\0')) text.pop_back();

        return text;
    };

    Photo made;

    made.width = shot.width;
    made.height = shot.height;
    made.orientation = shot.exif.orientation.value_or(shot.xmp.orientation.value_or(1));
    made.make = trimmed(!shot.exif.make.empty() ? shot.exif.make : shot.xmp.make);
    made.model = trimmed(!shot.exif.model.empty() ? shot.exif.model : shot.xmp.model);
    made.taken = !shot.exif.taken.empty() ? shot.exif.taken : !shot.xmp.taken.empty() ? shot.xmp.taken : shot.iptc.taken;
    made.place = shot.exif.place ? shot.exif.place : shot.xmp.place;

    return made;
}

// A JPEG file: its segments up to the scan.
Task<void> jpeg(Reader& from, std::uint64_t size, Gathered& out) {
    Walk walk{from, size, 0, size};
    Shot shot;
    Bytes photoshop;
    Extension extension;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    out.into.type = "image/jpeg";

    co_await walk.need(2);
    walk.skip(2);

    while (walk.left() >= 2 && !out.done()) {
        co_await walk.need(4);

        if (walk.u8() != 0xFF) {
            out.damaged();

            break;
        }

        std::uint8_t marker = walk.u8();

        while (marker == 0xFF && walk.left() > 0) {
            co_await walk.need(1);
            marker = walk.u8();
        }

        if (marker != 0xED && !photoshop.empty()) {
            resources(photoshop, out, shot);
            photoshop.clear();
        }

        if (marker == 0xD9 || marker == 0xDA) break;

        if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) continue;

        co_await walk.need(2);

        const std::uint16_t length = walk.be16();

        if (length < 2) {
            out.damaged();

            break;
        }

        const std::size_t much = length - 2u;
        const bool frame = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;

        if (marker != 0xE1 && marker != 0xED && !frame) {
            walk.skip(much);

            continue;
        }

        co_await walk.need(much);

        const Bytes payload = walk.bytes(much);

        if (payload.size() < much) {
            out.damaged();

            break;
        }

        if (frame) {
            if (much >= 5) {
                height = (sample(payload[1]) << 8) | sample(payload[2]);
                width = (sample(payload[3]) << 8) | sample(payload[4]);
            }
        } else if (marker == 0xE1 && marked(payload, exif_mark)) {
            co_await held_exif(Bytes(payload.begin() + exif_mark.size(), payload.end()), out, shot);
        } else if (marker == 0xE1 && marked(payload, xmp_mark)) {
            xmp(text_of(payload).substr(xmp_mark.size()), out, shot);
        } else if (marker == 0xE1 && marked(payload, extension_mark) && much >= extension_mark.size() + 40) {
            const std::span<const std::byte> rest = std::span<const std::byte>{payload}.subspan(extension_mark.size());
            const std::uint64_t whole = (std::uint64_t{sample(rest[32])} << 24) | (sample(rest[33]) << 16) | (sample(rest[34]) << 8) | sample(rest[35]);
            const std::uint64_t at = (std::uint64_t{sample(rest[36])} << 24) | (sample(rest[37]) << 16) | (sample(rest[38]) << 8) | sample(rest[39]);

            if (!shot.extended.empty() && text_of(rest.first(32)) == shot.extended) placed(extension, whole, at, rest.subspan(40));
        } else if (marker == 0xED && marked(payload, photoshop_mark) && photoshop.size() + much <= most_block) {
            photoshop.insert(photoshop.end(), payload.begin() + static_cast<std::ptrdiff_t>(photoshop_mark.size()), payload.end());
        }
    }

    if (!photoshop.empty()) resources(photoshop, out, shot);

    if (!shot.extended.empty()) {
        const std::optional<std::string> packet = joined(extension);

        if (packet) xmp(*packet, out, shot);
        else out.damaged();
    }

    out.into.photo = shaped(shot, width, height);
}

// A PNG file: its chunks.
Task<void> png(Reader& from, std::uint64_t size, Gathered& out) {
    Walk walk{from, size, 8, size};
    Shot shot;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    out.into.type = "image/png";

    while (walk.left() >= 12 && !out.done()) {
        co_await walk.need(8);

        const std::uint32_t length = walk.be32();
        const std::string type = walk.chars(4);

        if (length > walk.left()) {
            out.damaged();

            break;
        }

        const bool text = type == "tEXt" || type == "zTXt" || type == "iTXt";

        if (type == "IHDR" && length >= 8) {
            co_await walk.need(8);
            width = walk.be32();
            height = walk.be32();
            walk.skip(length - 8 + 4);

            continue;
        }

        if ((type != "eXIf" && !text) || length > most_block) {
            walk.skip(std::uint64_t{length} + 4);

            if (type == "IEND") break;

            continue;
        }

        const Bytes data = co_await walk.take(length);

        walk.skip(4);

        if (data.size() < length) {
            out.damaged();

            break;
        }

        if (type == "eXIf") {
            co_await held_exif(data, out, shot);

            continue;
        }

        const std::span<const std::byte> all{data};
        const std::string_view whole = text_of(all);
        const std::size_t zero = whole.find('\0');

        if (zero == std::string_view::npos) continue;

        const std::string_view keyword = whole.substr(0, zero);
        std::optional<Bytes> content;

        if (type == "tEXt") {
            content = Bytes(all.begin() + static_cast<std::ptrdiff_t>(zero + 1), all.end());
        } else if (type == "zTXt") {
            if (zero + 2 <= all.size()) content = inflated(all.subspan(zero + 2), most_block, out);
        } else if (zero + 3 <= all.size()) {
            const bool compressed = sample(all[zero + 1]) == 1;
            const std::size_t language = whole.find('\0', zero + 3);
            const std::size_t translated = language == std::string_view::npos ? language : whole.find('\0', language + 1);

            if (translated != std::string_view::npos) {
                const std::span<const std::byte> body = all.subspan(translated + 1);

                content = compressed ? inflated(body, most_block, out) : Bytes(body.begin(), body.end());
            }
        }

        if (!content) {
            if (keyword == "XML:com.adobe.xmp" || keyword.starts_with("Raw profile type ")) out.damaged();

            continue;
        }

        if (keyword == "XML:com.adobe.xmp") {
            xmp(text_of(*content), out, shot);

            continue;
        }

        if (!keyword.starts_with("Raw profile type ")) continue;

        const std::string_view kind = keyword.substr(17);
        const std::optional<Bytes> profile = raw_profile(text_of(*content));

        if (!profile) {
            out.damaged();

            continue;
        }

        if ((kind == "exif" || kind == "APP1") && marked(*profile, exif_mark)) {
            co_await held_exif(Bytes(profile->begin() + exif_mark.size(), profile->end()), out, shot);
        } else if (kind == "exif") {
            co_await held_exif(*profile, out, shot);
        } else if (kind == "xmp" || (kind == "APP1" && marked(*profile, xmp_mark))) {
            xmp(text_of(*profile).substr(kind == "xmp" ? 0 : xmp_mark.size()), out, shot);
        } else if (kind == "iptc" || kind == "8bim") {
            if (marked(*profile, "8BIM")) resources(*profile, out, shot);
            else iim(*profile, out, shot);
        }
    }

    out.into.photo = shaped(shot, width, height);
}

// A WebP file: its chunks inside RIFF.
Task<void> webp(Reader& from, std::uint64_t size, Gathered& out) {
    Walk file{from, size, 0, size};
    Shot shot;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool canvas = false;

    out.into.type = "image/webp";

    co_await file.need(12);
    file.skip(4);

    const std::uint32_t riff = file.le32();

    file.skip(4);

    Walk walk = file.part(riff >= 4 ? riff - 4u : 0u);

    while (walk.left() >= 8 && !out.done()) {
        co_await walk.need(8);

        const std::string type = walk.chars(4);
        const std::uint32_t length = walk.le32();
        const std::uint64_t padded = std::uint64_t{length} + (length & 1u);

        if (length > walk.left()) out.damaged();

        if (type == "VP8X" || type == "VP8 " || type == "VP8L") {
            co_await walk.need(10);

            Walk chunk = walk.part(padded);

            if (type == "VP8X" && length >= 10) {
                chunk.skip(4);

                const std::uint32_t wide = chunk.le16();
                const std::uint32_t wider = chunk.u8();
                const std::uint32_t high = chunk.le16();
                const std::uint32_t higher = chunk.u8();

                width = (wide | (wider << 16)) + 1;
                height = (high | (higher << 16)) + 1;
                canvas = true;
            } else if (type == "VP8 " && length >= 10 && !canvas) {
                chunk.skip(3);

                if (chunk.u8() == 0x9D && chunk.u8() == 0x01 && chunk.u8() == 0x2A) {
                    width = chunk.le16() & 0x3FFFu;
                    height = chunk.le16() & 0x3FFFu;
                }
            } else if (type == "VP8L" && length >= 5 && !canvas && chunk.u8() == 0x2F) {
                const std::uint32_t bits = chunk.le32();

                width = (bits & 0x3FFFu) + 1;
                height = ((bits >> 14) & 0x3FFFu) + 1;
            }

            continue;
        }

        if ((type != "EXIF" && type != "XMP ") || length > most_block) {
            walk.skip(padded);

            continue;
        }

        const Bytes data = co_await walk.take(length);

        walk.skip(length & 1u);

        if (data.size() < length) {
            out.damaged();

            break;
        }

        if (type == "XMP ") xmp(text_of(data), out, shot);
        else if (marked(data, exif_mark)) co_await held_exif(Bytes(data.begin() + exif_mark.size(), data.end()), out, shot);
        else co_await held_exif(data, out, shot);
    }

    out.into.photo = shaped(shot, width, height);
}

// A TIFF file, or a raw file built on TIFF: the structure is the file, and the image.
Task<void> tiff(Reader& from, std::uint64_t size, Gathered& out) {
    Shot shot;

    out.into.type = "image/tiff";

    const bool read = co_await exif(from, size, 0, size, true, out, shot);

    if (!read) out.damaged();

    out.into.photo = photo_of(shot);
}

// A HEIF or AVIF file: the items of its meta box.
Task<void> heif(Reader& from, std::uint64_t size, bool avif, Gathered& out) {
    Walk file{from, size, 0, size};
    Shot shot;
    Meta meta;

    out.into.type = avif ? "image/avif" : "image/heic";

    while (true) {
        std::optional<Box> top = co_await boxed(file, out);

        if (!top) break;

        if (top->type != "meta") continue;

        co_await top->body.need(4);
        top->body.skip(4);

        while (true) {
            std::optional<Box> box = co_await boxed(top->body, out);

            if (!box) break;

            if (box->type == "pitm") {
                co_await box->body.need(4 + 4);

                const std::uint8_t version = box->body.u8();

                box->body.skip(3);
                meta.primary = version == 0 ? box->body.be16() : box->body.be32();
            } else if (box->type == "iinf") {
                co_await informed(box->body, meta, out);
            } else if (box->type == "iloc") {
                co_await located(box->body, meta, out);
            } else if (box->type == "iprp") {
                co_await propertied(box->body, meta, out);
            } else if (box->type == "idat") {
                meta.idat = std::pair{box->body.at(), box->body.left()};
            }
        }

        break;
    }

    for (const std::uint32_t property : meta.properties[meta.primary]) {
        const auto extent = meta.extents.find(property);

        if (extent != meta.extents.end()) {
            shot.width = extent->second.first;
            shot.height = extent->second.second;

            break;
        }
    }

    std::uint64_t budget = size;

    for (const auto& [number, item] : meta.items) {
        if (out.done()) break;

        const bool exif_item = item.type == "Exif";
        const bool xmp_item = item.type == "mime" && item.content == "application/rdf+xml";

        if (!exif_item && !xmp_item) continue;

        const std::optional<Bytes> data = co_await item_data(from, size, meta, number, budget);

        if (!data) {
            out.damaged();

            continue;
        }

        if (xmp_item) {
            xmp(text_of(*data), out, shot);

            continue;
        }

        if (data->size() < 4) {
            out.damaged();

            continue;
        }

        const std::uint64_t skip = (std::uint64_t{sample((*data)[0])} << 24) | (sample((*data)[1]) << 16) | (sample((*data)[2]) << 8) | sample((*data)[3]);

        if (skip > data->size() - 4) {
            out.damaged();

            continue;
        }

        co_await held_exif(Bytes(data->begin() + static_cast<std::ptrdiff_t>(4 + skip), data->end()), out, shot);
    }

    out.into.photo = photo_of(shot);
}

// --- Writing -------------------------------------------------------------------------------------

namespace {

// The changes sorted by where they go.
struct Sorted {
    std::vector<ExifChange> exif;
    std::vector<Change> xmp;
    std::vector<IptcChange> iptc;
};

// Which blocks of metadata the file has.
struct Present {
    bool exif = false;
    bool xmp = false;
    bool iptc = false;
};

// Clears changes that only remove from a block the file does not have: they would make an empty
// block where there was none.
template <typename Changes> void pruned(Changes& changes, bool present) {
    if (!present && std::ranges::all_of(changes, [](const auto& one) { return one.values.empty(); })) changes.clear();
}

// A field of text with one value.
[[nodiscard]] Change one(Scheme scheme, std::string key, Value value) { return {scheme, std::move(key), {std::move(value)}}; }

// The changes an edit makes, as the Metadata Working Group pairs the fields of EXIF, XMP and IPTC:
// each goes to EXIF and XMP, and to IPTC when the file has IPTC. A time is ISO 8601 with seconds
// and an optional offset ("2024-07-08T09:10:11+02:00"); EXIF takes it as "2024:07:08 09:10:11"
// with the offset apart and the fraction of a second it may have had removed, IPTC as a date and a
// time with the offset. Throws `Trouble` malformed for a time of another form, an orientation
// outside 1 to 8 or a rating outside -1 to 5.
[[nodiscard]] std::vector<Change> expanded(const Edit& edit, bool iptc) {
    std::vector<Change> made;

    if (edit.orientation) {
        if (*edit.orientation < 1 || *edit.orientation > 8) throw Trouble{Kind::malformed, "An orientation is 1 to 8."};

        made.push_back(one(Scheme::exif, "Exif.Image.Orientation", std::int64_t{*edit.orientation}));
        made.push_back(one(Scheme::xmp, "Xmp.tiff.Orientation", std::to_string(*edit.orientation)));
    }

    if (edit.taken) {
        const std::string& at = *edit.taken;
        constexpr std::string_view pattern = "dddd-dd-ddTdd:dd:dd";
        bool good = at.size() == pattern.size() || (at.size() == pattern.size() + 6 && (at[19] == '+' || at[19] == '-') && at[22] == ':');

        for (std::size_t one_at = 0; good && one_at < pattern.size(); ++one_at)
            good = pattern[one_at] == 'd' ? at[one_at] >= '0' && at[one_at] <= '9' : at[one_at] == pattern[one_at];

        if (!good) throw Trouble{Kind::malformed, "A time is written as 2024-07-08T09:10:11, with an offset or without."};

        std::string exif_time = at.substr(0, 19);

        exif_time[4] = ':';
        exif_time[7] = ':';
        exif_time[10] = ' ';

        const std::string offset = at.size() > 19 ? at.substr(19) : std::string{};

        made.push_back(one(Scheme::exif, "Exif.Photo.DateTimeOriginal", exif_time));
        made.push_back(Change{Scheme::exif, "Exif.Photo.SubSecTimeOriginal", {}});
        made.push_back(offset.empty() ? Change{Scheme::exif, "Exif.Photo.OffsetTimeOriginal", {}} : one(Scheme::exif, "Exif.Photo.OffsetTimeOriginal", offset));
        made.push_back(one(Scheme::xmp, "Xmp.photoshop.DateCreated", at));

        if (iptc) {
            made.push_back(one(Scheme::iptc, "Iptc.Application2.DateCreated", at.substr(0, 4) + at.substr(5, 2) + at.substr(8, 2)));

            const std::string clock = at.substr(11, 2) + at.substr(14, 2) + at.substr(17, 2);

            made.push_back(offset.empty() ? Change{Scheme::iptc, "Iptc.Application2.TimeCreated", {}}
                                          : one(Scheme::iptc, "Iptc.Application2.TimeCreated", clock + offset.substr(0, 3) + offset.substr(4, 2)));
        }
    }

    if (edit.description) {
        made.push_back(one(Scheme::exif, "Exif.Image.ImageDescription", *edit.description));
        made.push_back(one(Scheme::xmp, "Xmp.dc.description", *edit.description));

        if (iptc) made.push_back(one(Scheme::iptc, "Iptc.Application2.Caption", *edit.description));
    }

    if (edit.keywords) {
        std::vector<Value> words(edit.keywords->begin(), edit.keywords->end());

        made.push_back({Scheme::xmp, "Xmp.dc.subject", words});

        if (iptc) made.push_back({Scheme::iptc, "Iptc.Application2.Keywords", words});
    }

    if (edit.rating) {
        if (*edit.rating < -1 || *edit.rating > 5) throw Trouble{Kind::malformed, "A rating is -1 (rejected) to 5."};

        made.push_back(one(Scheme::xmp, "Xmp.xmp.Rating", std::to_string(*edit.rating)));
    }

    if (edit.creator) {
        made.push_back(one(Scheme::exif, "Exif.Image.Artist", *edit.creator));
        made.push_back(one(Scheme::xmp, "Xmp.dc.creator", *edit.creator));

        if (iptc) made.push_back(one(Scheme::iptc, "Iptc.Application2.Byline", *edit.creator));
    }

    if (edit.copyright) {
        made.push_back(one(Scheme::exif, "Exif.Image.Copyright", *edit.copyright));
        made.push_back(one(Scheme::xmp, "Xmp.dc.rights", *edit.copyright));

        if (iptc) made.push_back(one(Scheme::iptc, "Iptc.Application2.Copyright", *edit.copyright));
    }

    return made;
}

// The changes sorted by where they go, the edit's first and the caller's after them, which win.
// Changes that only remove from a block the file does not have (`present`) are left out. Throws
// `Trouble`: unsupported for a key of another scheme or one we do not know, malformed for text that
// is not UTF-8.
[[nodiscard]] Sorted sorted(const Edit& edit, const std::vector<Change>& changes, Present present) {
    std::vector<Change> all = expanded(edit, present.iptc);

    all.insert(all.end(), changes.begin(), changes.end());

    Sorted made;

    for (const Change& change : all) {
        for (const Value& value : change.values)
            if (const auto* text = std::get_if<std::string>(&value); text != nullptr && !valid_utf8(std::as_bytes(std::span{*text})))
                throw Trouble{Kind::malformed, "A value is not text of UTF-8."};

        if (change.scheme == Scheme::xmp) {
            made.xmp.push_back(change);

            continue;
        }

        const std::size_t first = change.key.find('.');
        const std::size_t second = first == std::string::npos ? first : change.key.find('.', first + 1);

        if (second == std::string::npos) throw Trouble{Kind::unsupported, "The key " + change.key + " is not of a field we write."};

        const std::string_view family = std::string_view{change.key}.substr(0, first);
        const std::string_view group = std::string_view{change.key}.substr(first + 1, second - first - 1);
        const std::string_view name = std::string_view{change.key}.substr(second + 1);

        if (change.scheme == Scheme::exif && family == "Exif" && (group == "Image" || group == "Photo" || group == "GPSInfo")) {
            const std::optional<std::uint16_t> tag = tag_number(group, name);

            if (!tag) throw Trouble{Kind::unsupported, "The key " + change.key + " is not of a field we write."};

            made.exif.push_back({group == "Image" ? std::string_view{"Image"} : group == "Photo" ? std::string_view{"Photo"} : std::string_view{"GPSInfo"}, *tag, change.values});

            continue;
        }

        if (change.scheme == Scheme::iptc && family == "Iptc" && group == "Application2") {
            const std::optional<std::uint8_t> number = dataset_number(2, name);

            if (!number) throw Trouble{Kind::unsupported, "The key " + change.key + " is not of a field we write."};

            made.iptc.push_back({*number, change.values});

            continue;
        }

        throw Trouble{Kind::unsupported, "The key " + change.key + " is not of a field we write."};
    }

    pruned(made.exif, present.exif);
    pruned(made.xmp, present.xmp);
    pruned(made.iptc, present.iptc);

    return made;
}

// The IPTC that Photoshop's resources hold (0x0404), or none.
[[nodiscard]] std::optional<Bytes> iptc_in(std::span<const std::byte> data) {
    std::size_t at = 0;

    while (at + 7 <= data.size() && text_of(data.subspan(at, 4)) == "8BIM") {
        const std::uint32_t id = (sample(data[at + 4]) << 8) | sample(data[at + 5]);
        const std::size_t padded = (sample(data[at + 6]) + 2u) & ~std::size_t{1};

        if (at + 6 + padded + 4 > data.size()) break;

        const std::size_t head = at + 6 + padded;
        const std::uint64_t size = (std::uint64_t{sample(data[head])} << 24) | (sample(data[head + 1]) << 16) | (sample(data[head + 2]) << 8) | sample(data[head + 3]);

        if (size > data.size() - head - 4) break;

        if (id == 0x0404) return Bytes(data.begin() + static_cast<std::ptrdiff_t>(head + 4), data.begin() + static_cast<std::ptrdiff_t>(head + 4 + size));

        at = head + 4 + static_cast<std::size_t>(size) + (size % 2);
    }

    return std::nullopt;
}

// A segment of JPEG with its payload after the mark; one longer than a segment holds cannot be
// written.
[[nodiscard]] Bytes segment(std::uint8_t marker, std::string_view mark, std::span<const std::byte> payload) {
    if (mark.size() + payload.size() > 65533) throw Trouble{Kind::unsupported, "The metadata would not fit in a segment of JPEG."};

    const std::size_t length = 2 + mark.size() + payload.size();
    Bytes made{std::byte{0xFF}, std::byte{marker}, static_cast<std::byte>(length >> 8), static_cast<std::byte>(length & 0xFF)};

    made.insert(made.end(), reinterpret_cast<const std::byte*>(mark.data()), reinterpret_cast<const std::byte*>(mark.data()) + mark.size());
    made.insert(made.end(), payload.begin(), payload.end());

    return made;
}

// A chunk of PNG with its check.
[[nodiscard]] Bytes png_chunk(std::string_view type, std::span<const std::byte> data) {
    Bytes made;

    for (int shift = 24; shift >= 0; shift -= 8) made.push_back(static_cast<std::byte>((data.size() >> shift) & 0xFF));

    made.insert(made.end(), reinterpret_cast<const std::byte*>(type.data()), reinterpret_cast<const std::byte*>(type.data()) + 4);
    made.insert(made.end(), data.begin(), data.end());

    const std::uint32_t check = inflating::crc32(0, std::span<const std::byte>{made}.subspan(4));

    for (int shift = 24; shift >= 0; shift -= 8) made.push_back(static_cast<std::byte>((check >> shift) & 0xFF));

    return made;
}

// A chunk of RIFF, padded to an even length.
[[nodiscard]] Bytes riff_chunk(std::string_view type, std::span<const std::byte> data) {
    Bytes made(reinterpret_cast<const std::byte*>(type.data()), reinterpret_cast<const std::byte*>(type.data()) + 4);

    for (int shift = 0; shift < 32; shift += 8) made.push_back(static_cast<std::byte>((data.size() >> shift) & 0xFF));

    made.insert(made.end(), data.begin(), data.end());

    if (data.size() % 2 != 0) made.push_back(std::byte{0});

    return made;
}

// The bytes of a text.
[[nodiscard]] Bytes bytes_of(std::string_view text) { return Bytes(reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data()) + text.size()); }

// A piece of the file copied as it is.
[[nodiscard]] Piece kept(std::uint64_t from, std::uint64_t much) { return Piece{from, much, {}}; }

// A piece written anew.
[[nodiscard]] Piece written_piece(Bytes made) { return Piece{0, 0, std::move(made)}; }

// The bytes of `much` from `at`, all of them; a file that ends before is damage to it.
[[nodiscard]] Task<Bytes> exactly(Reader& from, std::uint64_t at, std::uint64_t much) {
    Bytes made = co_await from.read(at, static_cast<std::size_t>(much));

    if (made.size() < much) throw Trouble{Kind::malformed, "The file ends before its structure does."};

    co_return made;
}

// The extended XMP the main packet names, joined from the pieces of JPEG, or nothing when it
// names none; damage when it names one that is not whole.
[[nodiscard]] std::string extension_of(std::string_view packet, const std::vector<Bytes>& pieces) {
    Description seen;
    Gathered aside{seen};
    Shot shot;

    xmp(packet, aside, shot);

    if (shot.extended.empty()) return {};

    Extension extension;

    for (const Bytes& payload : pieces) {
        const std::span<const std::byte> rest = std::span<const std::byte>{payload}.subspan(extension_mark.size());

        if (rest.size() < 40 || text_of(rest.first(32)) != shot.extended) continue;

        const std::uint64_t whole = (std::uint64_t{sample(rest[32])} << 24) | (sample(rest[33]) << 16) | (sample(rest[34]) << 8) | sample(rest[35]);
        const std::uint64_t at = (std::uint64_t{sample(rest[36])} << 24) | (sample(rest[37]) << 16) | (sample(rest[38]) << 8) | sample(rest[39]);

        placed(extension, whole, at, rest.subspan(40));
    }

    std::optional<std::string> made = joined(extension);

    if (!made) throw Trouble{Kind::malformed, "The extended XMP of the file is not whole."};

    return std::move(*made);
}

// The plan of a JPEG: its segments as they are, those of EXIF, XMP and Photoshop's resources
// written anew where they stood, new ones after SOI and the JFIF segments.
[[nodiscard]] Task<std::vector<Piece>> jpeg_plan(Reader& from, std::uint64_t size, const Edit& edit, const std::vector<Change>& changes) {
    struct Segment {
        std::uint8_t marker = 0;
        std::uint64_t at = 0;
        std::uint64_t much = 0;
        Bytes payload;
    };

    std::vector<Segment> segments;
    std::uint64_t at = 2;
    std::uint64_t scan = size;

    while (at + 4 <= size) {
        const Bytes head = co_await exactly(from, at, 4);

        if (sample(head[0]) != 0xFF) throw Trouble{Kind::malformed, "The segments of the JPEG are broken."};

        const std::uint32_t marker = sample(head[1]);

        if (marker == 0xFF) {
            ++at;

            continue;
        }

        if (marker == 0xDA || marker == 0xD9) {
            scan = at;

            break;
        }

        if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            segments.push_back({static_cast<std::uint8_t>(marker), at, 2, {}});
            at += 2;

            continue;
        }

        const std::uint32_t length = (sample(head[2]) << 8) | sample(head[3]);

        if (length < 2 || at + 2 + length > size) throw Trouble{Kind::malformed, "The segments of the JPEG are broken."};

        Segment one{static_cast<std::uint8_t>(marker), at, 2u + length, {}};

        if (marker == 0xE1 || marker == 0xED) one.payload = co_await exactly(from, at + 4, length - 2u);

        segments.push_back(std::move(one));
        at += 2u + length;
    }

    if (scan == size) throw Trouble{Kind::malformed, "The JPEG has no scan."};

    std::optional<std::size_t> exif_at;
    std::optional<std::size_t> xmp_at;
    std::vector<std::size_t> photoshop_at;
    std::vector<Bytes> pieces;
    Bytes photoshop;

    for (std::size_t index = 0; index < segments.size(); ++index) {
        const Segment& one = segments[index];

        if (one.marker == 0xE1 && !exif_at && marked(one.payload, exif_mark)) exif_at = index;
        else if (one.marker == 0xE1 && !xmp_at && marked(one.payload, xmp_mark)) xmp_at = index;
        else if (one.marker == 0xE1 && marked(one.payload, extension_mark)) pieces.push_back(one.payload);
        else if (one.marker == 0xED && marked(one.payload, photoshop_mark)) {
            photoshop_at.push_back(index);
            photoshop.insert(photoshop.end(), one.payload.begin() + static_cast<std::ptrdiff_t>(photoshop_mark.size()), one.payload.end());
        }
    }

    const std::optional<Bytes> iim = photoshop.empty() ? std::nullopt : iptc_in(photoshop);
    const Sorted sorting = sorted(edit, changes, {exif_at.has_value(), xmp_at.has_value(), iim.has_value()});
    std::optional<Bytes> exif_new;
    std::optional<Bytes> xmp_new;
    std::vector<Bytes> photoshop_new;

    if (!sorting.exif.empty()) {
        const std::span<const std::byte> old = exif_at ? std::span<const std::byte>{segments[*exif_at].payload}.subspan(exif_mark.size()) : std::span<const std::byte>{};

        exif_new = segment(0xE1, exif_mark, exif_rewritten(old, sorting.exif));
    }

    if (!sorting.xmp.empty()) {
        const std::string_view old = xmp_at ? text_of(segments[*xmp_at].payload).substr(xmp_mark.size()) : std::string_view{};
        const std::string extension = old.empty() ? std::string{} : extension_of(old, pieces);

        xmp_new = segment(0xE1, xmp_mark, bytes_of(xmp_rewritten(old, sorting.xmp, extension, 65533 - xmp_mark.size())));
    }

    if (!sorting.iptc.empty()) {
        const Bytes resources = resources_rewritten(photoshop, iim_rewritten(iim.value_or(Bytes{}), sorting.iptc));

        for (std::size_t from_at = 0; from_at < resources.size(); from_at += 65519)
            photoshop_new.push_back(segment(0xED, photoshop_mark, std::span<const std::byte>{resources}.subspan(from_at, std::min<std::size_t>(65519, resources.size() - from_at))));
    }

    std::vector<Piece> made{kept(0, 2)};
    std::size_t lead = 0;

    while (lead < segments.size() && segments[lead].marker == 0xE0) ++lead;

    std::vector<std::optional<std::size_t>> order;
    std::vector<Bytes> fresh;

    for (std::size_t index = 0; index < segments.size(); ++index) order.emplace_back(index);

    const auto inserted = [&order, &fresh](std::size_t at_order, const Bytes& one) {
        fresh.push_back(one);
        order.insert(order.begin() + static_cast<std::ptrdiff_t>(at_order), std::nullopt);

        return at_order + 1;
    };
    const auto after = [&order](std::size_t segment_index) {
        return static_cast<std::size_t>(std::ranges::find(order, std::optional<std::size_t>{segment_index}) - order.begin()) + 1;
    };

    std::size_t next = lead;

    if (exif_new && !exif_at) next = inserted(next, *exif_new);
    else if (exif_at) next = after(*exif_at);

    if (xmp_new && !xmp_at) next = inserted(next, *xmp_new);
    else if (xmp_at) next = std::max(next, after(*xmp_at));

    if (!photoshop_new.empty() && photoshop_at.empty())
        for (const Bytes& one : photoshop_new) next = inserted(next, one);

    std::size_t fresh_at = 0;

    for (const std::optional<std::size_t>& item : order) {
        if (!item) {
            made.push_back(written_piece(fresh[fresh_at++]));

            continue;
        }

        const std::size_t index = *item;
        const Segment& one = segments[index];

        if (exif_at && index == *exif_at && exif_new) {
            made.push_back(written_piece(*exif_new));
        } else if (xmp_at && index == *xmp_at && xmp_new) {
            made.push_back(written_piece(*xmp_new));
        } else if (!photoshop_new.empty() && std::ranges::find(photoshop_at, index) != photoshop_at.end()) {
            if (index == photoshop_at.front())
                for (const Bytes& part : photoshop_new) made.push_back(written_piece(part));
        } else {
            made.push_back(kept(one.at, one.much));
        }
    }

    made.push_back(kept(scan, size - scan));

    co_return made;
}

// The plan of a PNG: its chunks as they are, eXIf and the XMP of iTXt written anew where they stood
// or after IHDR, and ImageMagick's raw profiles of what was written anew left out. A chunk of
// EXIF or XMP too large to read is not rewritten: the file is refused rather than given a second.
[[nodiscard]] Task<std::vector<Piece>> png_plan(Reader& from, std::uint64_t size, const Edit& edit, const std::vector<Change>& changes) {
    struct Chunk {
        std::string type;
        std::uint64_t at = 0;
        std::uint64_t much = 0;
        std::string keyword;
        Bytes data;
    };

    std::vector<Chunk> chunks;
    std::uint64_t at = 8;

    while (at + 12 <= size) {
        const Bytes head = co_await exactly(from, at, 8);
        const std::uint64_t length = (std::uint64_t{sample(head[0])} << 24) | (sample(head[1]) << 16) | (sample(head[2]) << 8) | sample(head[3]);

        if (length > size - at - 12) throw Trouble{Kind::malformed, "The chunks of the PNG are broken."};

        Chunk one{std::string{text_of(std::span<const std::byte>{head}.subspan(4, 4))}, at, length + 12, {}, {}};
        const bool text = one.type == "tEXt" || one.type == "zTXt" || one.type == "iTXt";

        if ((one.type == "eXIf" || text) && length <= most_block) {
            one.data = co_await exactly(from, at + 8, length);

            if (text) one.keyword = std::string{text_of(one.data).substr(0, text_of(one.data).find('\0'))};
        } else if (one.type == "eXIf" || text) {
            const Bytes start = one.type == "eXIf" ? Bytes{} : co_await exactly(from, at + 8, 80);
            const std::string_view keyword = text_of(start).substr(0, text_of(start).find('\0'));

            if (one.type == "eXIf" || keyword == "XML:com.adobe.xmp" || keyword == "Raw profile type exif" || keyword == "Raw profile type xmp")
                throw Trouble{Kind::unsupported, "A chunk of metadata of the PNG is too large to rewrite."};
        }

        chunks.push_back(std::move(one));
        at += length + 12;

        if (chunks.back().type == "IEND") break;
    }

    if (chunks.empty() || chunks.front().type != "IHDR") throw Trouble{Kind::malformed, "The PNG does not start with IHDR."};

    Description seen;
    Gathered aside{seen};

    aside.spent->size = size;

    const auto content = [&aside](const Chunk& one) -> std::optional<Bytes> {
        const std::span<const std::byte> all{one.data};
        const std::size_t zero = one.keyword.size();

        if (one.type == "tEXt") return Bytes(all.begin() + static_cast<std::ptrdiff_t>(zero + 1), all.end());

        if (one.type == "zTXt") return zero + 2 <= all.size() ? inflated(all.subspan(zero + 2), most_block, aside) : std::nullopt;

        const std::string_view whole = text_of(all);
        const std::size_t language = zero + 3 <= all.size() ? whole.find('\0', zero + 3) : std::string_view::npos;
        const std::size_t translated = language == std::string_view::npos ? language : whole.find('\0', language + 1);

        if (translated == std::string_view::npos) return std::nullopt;

        const std::span<const std::byte> body = all.subspan(translated + 1);

        return sample(all[zero + 1]) == 1 ? inflated(body, most_block, aside) : Bytes(body.begin(), body.end());
    };

    std::optional<std::size_t> exif_at;
    std::optional<std::size_t> xmp_at;
    std::optional<std::size_t> exif_profile;
    std::optional<std::size_t> xmp_profile;

    for (std::size_t index = 0; index < chunks.size(); ++index) {
        const Chunk& one = chunks[index];

        if (one.type == "eXIf" && !exif_at) exif_at = index;
        else if (one.keyword == "XML:com.adobe.xmp" && !xmp_at) xmp_at = index;
        else if (one.keyword == "Raw profile type exif" && !exif_profile) exif_profile = index;
        else if (one.keyword == "Raw profile type xmp" && !xmp_profile) xmp_profile = index;
    }

    const Sorted sorting = sorted(edit, changes, {exif_at || exif_profile, xmp_at || xmp_profile, false});

    if (!sorting.iptc.empty()) throw Trouble{Kind::unsupported, "IPTC is not written in PNG; its carrier there is XMP."};

    std::optional<Bytes> exif_new;
    std::optional<Bytes> xmp_new;

    if (!sorting.exif.empty()) {
        Bytes old;

        if (exif_at) {
            old = chunks[*exif_at].data;
        } else if (exif_profile) {
            const std::optional<Bytes> text = content(chunks[*exif_profile]);
            const std::optional<Bytes> profile = text ? raw_profile(text_of(*text)) : std::nullopt;

            if (!profile) throw Trouble{Kind::malformed, "The raw profile of EXIF in the PNG is not readable."};

            old = marked(*profile, exif_mark) ? Bytes(profile->begin() + static_cast<std::ptrdiff_t>(exif_mark.size()), profile->end()) : *profile;
        }

        exif_new = png_chunk("eXIf", exif_rewritten(old, sorting.exif));
    }

    if (!sorting.xmp.empty()) {
        std::string old;

        if (xmp_at || xmp_profile) {
            const std::optional<Bytes> text = content(chunks[xmp_at ? *xmp_at : *xmp_profile]);
            const std::optional<Bytes> packet = !text ? std::nullopt : xmp_at ? text : raw_profile(text_of(*text));

            if (!packet) throw Trouble{Kind::malformed, "The XMP of the PNG is not readable."};

            old = std::string{text_of(*packet)};
        }

        Bytes data = bytes_of(std::string_view{"XML:com.adobe.xmp\0\0\0\0\0", 22});
        const Bytes packet = bytes_of(xmp_rewritten(old, sorting.xmp, {}, most_packet));

        data.insert(data.end(), packet.begin(), packet.end());
        xmp_new = png_chunk("iTXt", data);
    }

    std::vector<Piece> made{kept(0, 8)};

    for (std::size_t index = 0; index < chunks.size(); ++index) {
        const Chunk& one = chunks[index];

        if (exif_at && index == *exif_at && exif_new) {
            made.push_back(written_piece(*exif_new));
        } else if (xmp_at && index == *xmp_at && xmp_new) {
            made.push_back(written_piece(*xmp_new));
        } else if ((exif_profile && index == *exif_profile && exif_new) || (xmp_profile && index == *xmp_profile && xmp_new)) {
            continue;
        } else {
            made.push_back(kept(one.at, one.much));
        }

        if (index == 0) {
            if (exif_new && !exif_at) made.push_back(written_piece(*exif_new));

            if (xmp_new && !xmp_at) made.push_back(written_piece(*xmp_new));
        }
    }

    if (at < size) made.push_back(kept(at, size - at));

    co_return made;
}

// The plan of a WebP: VP8X first, with the flags of EXIF and XMP, made from the frame's size when
// the file is simple; the chunks as they are (a last one cut before its pad byte gets it), EXIF
// and XMP written anew where they stood or at the end, EXIF before XMP, as the format orders them;
// the size of RIFF counted again. Of a frame only the first ten bytes, which hold its size, are
// read.
[[nodiscard]] Task<std::vector<Piece>> webp_plan(Reader& from, std::uint64_t size, const Edit& edit, const std::vector<Change>& changes) {
    struct Chunk {
        std::string type;
        std::uint64_t at = 0;
        std::uint64_t much = 0;
        Bytes data;
    };

    const Bytes riff = co_await exactly(from, 0, 12);
    const std::uint64_t declared = sample(riff[4]) | (sample(riff[5]) << 8) | (sample(riff[6]) << 16) | (std::uint64_t{sample(riff[7])} << 24);
    const std::uint64_t end = std::min(size, 8 + declared);
    std::vector<Chunk> chunks;
    std::uint64_t at = 12;

    while (at + 8 <= end) {
        const Bytes head = co_await exactly(from, at, 8);
        const std::uint64_t length = sample(head[4]) | (sample(head[5]) << 8) | (sample(head[6]) << 16) | (std::uint64_t{sample(head[7])} << 24);
        const std::uint64_t padded = length + (length & 1);

        if (length > end - at - 8) throw Trouble{Kind::malformed, "The chunks of the WebP are broken."};

        Chunk one{std::string{text_of(std::span<const std::byte>{head}.first(4))}, at, std::min(8 + padded, end - at), {}};

        if (one.type == "VP8 " || one.type == "VP8L") {
            one.data = co_await exactly(from, at + 8, std::min<std::uint64_t>(length, 10));
        } else if (one.type == "VP8X" || one.type == "EXIF" || one.type == "XMP ") {
            if (length > most_block) throw Trouble{Kind::unsupported, "A chunk of the WebP is too large to rewrite."};

            one.data = co_await exactly(from, at + 8, length);
        }

        chunks.push_back(std::move(one));
        at += 8 + padded;
    }

    const auto find = [&chunks](std::string_view type) -> std::optional<std::size_t> {
        for (std::size_t index = 0; index < chunks.size(); ++index)
            if (chunks[index].type == type) return index;

        return std::nullopt;
    };

    const std::optional<std::size_t> canvas = find("VP8X");
    const std::optional<std::size_t> exif_at = find("EXIF");
    const std::optional<std::size_t> xmp_at = find("XMP ");
    const Sorted sorting = sorted(edit, changes, {exif_at.has_value(), xmp_at.has_value(), false});

    if (!sorting.iptc.empty()) throw Trouble{Kind::unsupported, "IPTC is not written in WebP; its carrier there is XMP."};

    std::optional<Bytes> exif_new;
    std::optional<Bytes> xmp_new;

    if (!sorting.exif.empty()) {
        const Bytes old = exif_at ? chunks[*exif_at].data : Bytes{};
        const std::span<const std::byte> tiff = marked(old, exif_mark) ? std::span<const std::byte>{old}.subspan(exif_mark.size()) : std::span<const std::byte>{old};

        exif_new = riff_chunk("EXIF", exif_rewritten(tiff, sorting.exif));
    }

    if (!sorting.xmp.empty()) xmp_new = riff_chunk("XMP ", bytes_of(xmp_rewritten(xmp_at ? text_of(chunks[*xmp_at].data) : std::string_view{}, sorting.xmp, {}, most_packet)));

    Bytes flags_chunk;

    if (canvas) {
        Bytes data = chunks[*canvas].data;

        if (data.size() < 10) throw Trouble{Kind::malformed, "The VP8X of the WebP is cut short."};

        if (exif_new || exif_at) data[0] |= std::byte{0x08};

        if (xmp_new || xmp_at) data[0] |= std::byte{0x04};

        flags_chunk = riff_chunk("VP8X", data);
    } else {
        const std::optional<std::size_t> lossy = find("VP8 ");
        const std::optional<std::size_t> lossless = find("VP8L");
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint8_t flags = 0;

        if (lossy && chunks[*lossy].data.size() >= 10) {
            const Bytes& frame = chunks[*lossy].data;

            width = (sample(frame[6]) | (sample(frame[7]) << 8)) & 0x3FFFu;
            height = (sample(frame[8]) | (sample(frame[9]) << 8)) & 0x3FFFu;
        } else if (lossless && chunks[*lossless].data.size() >= 5 && sample(chunks[*lossless].data[0]) == 0x2F) {
            const Bytes& frame = chunks[*lossless].data;
            const std::uint32_t bits = sample(frame[1]) | (sample(frame[2]) << 8) | (sample(frame[3]) << 16) | (sample(frame[4]) << 24);

            width = (bits & 0x3FFFu) + 1;
            height = ((bits >> 14) & 0x3FFFu) + 1;

            if (((bits >> 28) & 1u) != 0) flags |= 0x10;
        } else {
            throw Trouble{Kind::malformed, "The WebP has no frame whose size we know."};
        }

        if (width == 0 || height == 0) throw Trouble{Kind::malformed, "The frame of the WebP has no size."};

        if (exif_new || exif_at) flags |= 0x08;

        if (xmp_new || xmp_at) flags |= 0x04;

        Bytes data{std::byte{flags}, std::byte{0}, std::byte{0}, std::byte{0}};

        for (const std::uint32_t one : {width - 1, height - 1})
            for (int shift = 0; shift < 24; shift += 8) data.push_back(static_cast<std::byte>((one >> shift) & 0xFF));

        flags_chunk = riff_chunk("VP8X", data);
    }

    std::vector<Piece> body{written_piece(flags_chunk)};

    for (std::size_t index = 0; index < chunks.size(); ++index) {
        const Chunk& one = chunks[index];

        if (canvas && index == *canvas) continue;

        if (exif_new && !exif_at && xmp_at && index == *xmp_at) body.push_back(written_piece(*exif_new));

        if (exif_at && index == *exif_at && exif_new) {
            body.push_back(written_piece(*exif_new));
        } else if (xmp_at && index == *xmp_at && xmp_new) {
            body.push_back(written_piece(*xmp_new));
        } else {
            body.push_back(kept(one.at, one.much));

            if (one.much % 2 != 0) body.push_back(written_piece(Bytes{std::byte{0}}));
        }
    }

    if (exif_new && !exif_at && !xmp_at) body.push_back(written_piece(*exif_new));

    if (xmp_new && !xmp_at) body.push_back(written_piece(*xmp_new));

    std::uint64_t total = 4;

    for (const Piece& one : body) total += one.made.empty() ? one.much : one.made.size();

    if (total > 0xFFFFFFFFu) throw Trouble{Kind::unsupported, "The WebP would grow past what RIFF counts."};

    Bytes header = bytes_of("RIFF");

    for (int shift = 0; shift < 32; shift += 8) header.push_back(static_cast<std::byte>((total >> shift) & 0xFF));

    const Bytes webp = bytes_of("WEBP");

    header.insert(header.end(), webp.begin(), webp.end());

    std::vector<Piece> made{written_piece(header)};

    made.insert(made.end(), body.begin(), body.end());

    co_return made;
}

}

// The plan of writing the changes into the file: the pieces of the new file in order, each copied
// from the file or written anew. JPEG, PNG and WebP are written; any other file is not. Throws
// `Trouble`: unsupported for a format, a field or a size we do not write, malformed for a value not
// of its form or a file whose structure cannot be rewritten safely.
Task<std::vector<Piece>> planned(Reader& from, const Edit& edit, const std::vector<Change>& changes) {
    const std::uint64_t size = co_await from.size();
    const Bytes head = co_await from.read(0, 12);

    if (marked(head, std::string_view{"\xFF\xD8\xFF", 3})) co_return co_await jpeg_plan(from, size, edit, changes);

    if (marked(head, std::string_view{"\x89PNG\r\n\x1A\n", 8})) co_return co_await png_plan(from, size, edit, changes);

    if (marked(head, "RIFF") && head.size() >= 12 && text_of(std::span<const std::byte>{head}.subspan(8, 4)) == "WEBP") co_return co_await webp_plan(from, size, edit, changes);

    throw Trouble{Kind::unsupported, "The metadata of this format is not written."};
}

}
