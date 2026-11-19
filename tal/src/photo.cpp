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

}
