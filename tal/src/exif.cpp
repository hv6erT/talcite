#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tagging.hpp"

// The structure of TIFF, which carries EXIF (TIFF 6.0, EXIF 3.0, CIPA DC-008): a header with the
// byte order and the offset of the first directory, directories (IFD) of twelve-byte entries, each
// a tag, a type, a count and either the value itself (four bytes or fewer) or its offset. Offsets
// count from the header, wherever the header stands: at the start of a TIFF file, after "Exif\0\0"
// in a JPEG, inside a chunk of PNG or WebP or an item of HEIF.
//
// The directories read are those exiv2 reads, named as it names them: IFD0 ("Image"), the EXIF
// directory it points to ("Photo"), the GPS directory ("GPSInfo"), the interoperability directory
// of EXIF ("Iop") and IFD1, the thumbnail ("Thumbnail"). Every tag becomes a field with the key
// exiv2 gives it and the value as `exiv2 -Pv` prints it: text up to its first zero; one integer
// as a number and several as text joined by spaces; fractions as "1/125"; bytes and undefined
// data as decimal numbers, but only up to `most_undefined` of them, past which the value is a
// blob (a profile, a print setting) and no field. A user comment is text in the coding its prefix
// names. Structure is not a field: the pointers to directories and to data, the maker's note, and
// the blocks of XMP, IPTC and Photoshop, which are read as what they are.
//
// What a photo is: the orientation, the camera, the time it was taken and the place, with the
// dimensions of the image itself when the TIFF is the image (a TIFF file, a raw file built on TIFF;
// for DNG the full-size image of a sub-directory when IFD0 holds a reduced one). The thumbnail of
// IFD1 goes to the picture list.
//
// Limits: a directory of more than `most_entries` entries is damage and is not read; a directory
// met again (a loop of offsets) is not read again; a value longer than `most_text` is skipped; the
// count of a value is checked against its size without overflow; at most `most_subs` sub-images.
// The values read from outside the entries add up to at most the size of the structure and
// `most_spare` more, since in a structure that keeps the rules no two values share their bytes: a
// thousand entries pointing at one megabyte would otherwise be read a thousand times.
namespace tal::meta::tagging {

namespace {

// The size of one value of each TIFF type, 1 to 13; zero for a type there is not.
constexpr std::array<std::uint8_t, 14> sizes{0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4};

// The tags that are structure, not metadata, sorted: data offsets and counts, sub-directories,
// thumbnail offsets, XMP, IPTC, Photoshop, the EXIF and GPS pointers, the maker's note, the
// interoperability pointer.
constexpr std::array<std::uint16_t, 14> structural{0x0111, 0x0117, 0x0144, 0x0145, 0x014a, 0x0201, 0x0202,
                                                   0x02bc, 0x83bb, 0x8649, 0x8769, 0x8825, 0x927c, 0xa005};

// How many entries a directory may have, how many bytes or undefined values a field shows, how
// many sub-images of a TIFF are looked at.
constexpr std::size_t most_entries = 1000;
constexpr std::size_t most_undefined = 64;
constexpr std::size_t most_subs = 16;
constexpr std::uint64_t most_spare = std::uint64_t{64} << 10;

// One entry of a directory: the tag, the type, the count and the four bytes of value or offset.
struct Entry {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::array<std::byte, 4> raw{};
};

// A byte as an unsigned number.
[[nodiscard]] std::uint32_t sample(std::byte one) { return std::to_integer<std::uint32_t>(one); }

// An unsigned integer of `width` bytes at `at` in the byte order.
[[nodiscard]] std::uint64_t unsigned_at(std::span<const std::byte> given, std::size_t at, std::size_t width, bool big) {
    std::uint64_t made = 0;

    for (std::size_t one = 0; one < width; ++one) {
        const std::size_t index = big ? at + one : at + width - 1 - one;

        made = (made << 8) | sample(given[index]);
    }

    return made;
}

// A signed integer of `width` bytes at `at` in the byte order.
[[nodiscard]] std::int64_t signed_at(std::span<const std::byte> given, std::size_t at, std::size_t width, bool big) {
    const std::uint64_t raw = unsigned_at(given, at, width, big);
    const std::uint64_t sign = std::uint64_t{1} << (8 * width - 1);

    return static_cast<std::int64_t>(raw ^ sign) - static_cast<std::int64_t>(sign);
}

// A floating point number as the C library prints it with "%g", as exiv2 does.
[[nodiscard]] std::string printed(double value) {
    std::array<char, 32> made{};
    const int many = std::snprintf(made.data(), made.size(), "%g", value);

    return std::string(made.data(), static_cast<std::size_t>(std::clamp(many, 0, static_cast<int>(made.size()) - 1)));
}

// The value of an entry as `exiv2 -Pv` prints it, or none for a blob, a pointer or a type there is
// not. `data` holds the `count` values. One integer is a number, undefined data never is.
[[nodiscard]] std::optional<Value> valued(const Entry& entry, std::span<const std::byte> data, bool big) {
    const std::size_t each = sizes[entry.type];
    const std::size_t many = entry.count;

    if (entry.type == 2) return Value{utf8(data)};

    if ((entry.type == 1 || entry.type == 6 || entry.type == 7) && many > most_undefined) return std::nullopt;

    if (entry.type == 13) return std::nullopt;

    const bool whole = entry.type == 1 || entry.type == 3 || entry.type == 4 || entry.type == 6 || entry.type == 7 || entry.type == 8 || entry.type == 9;
    const bool signed_type = entry.type == 6 || entry.type == 8 || entry.type == 9 || entry.type == 10;

    if (whole && many == 1 && entry.type != 7)
        return Value{signed_type ? signed_at(data, 0, each, big) : static_cast<std::int64_t>(unsigned_at(data, 0, each, big))};

    std::string made;

    for (std::size_t one = 0; one < many; ++one) {
        const std::size_t at = one * each;

        if (one > 0) made += ' ';

        if (whole) {
            made += signed_type ? std::to_string(signed_at(data, at, each, big)) : std::to_string(unsigned_at(data, at, each, big));
        } else if (entry.type == 5 || entry.type == 10) {
            const std::size_t half = each / 2;

            made += signed_type ? std::to_string(signed_at(data, at, half, big)) : std::to_string(unsigned_at(data, at, half, big));
            made += '/';
            made += signed_type ? std::to_string(signed_at(data, at + half, half, big)) : std::to_string(unsigned_at(data, at + half, half, big));
        } else {
            const std::uint64_t bits = unsigned_at(data, at, each, big);

            if (entry.type == 11) {
                const auto narrow = static_cast<std::uint32_t>(bits);

                made += printed(static_cast<double>(std::bit_cast<float>(narrow)));
            } else {
                made += printed(std::bit_cast<double>(bits));
            }
        }
    }

    return Value{std::move(made)};
}

// A user comment (EXIF 0x9286): eight bytes that name the coding, then the text in it, without
// the zeros and spaces that pad it. Unicode is UTF-16 in the byte order of the structure, unless a
// byte order mark says otherwise; JIS is not read.
[[nodiscard]] std::optional<std::string> commented(std::span<const std::byte> data, bool big) {
    if (data.size() < 8) return std::nullopt;

    const std::string_view code{reinterpret_cast<const char*>(data.data()), 8};
    const std::span<const std::byte> text = data.subspan(8);
    std::string made;

    if (code.starts_with("UNICODE")) {
        const bool marked = text.size() >= 2 && ((sample(text[0]) == 0xFE && sample(text[1]) == 0xFF) || (sample(text[0]) == 0xFF && sample(text[1]) == 0xFE));

        made = marked ? utf16(text) : utf16(text, big);
    } else if (code.starts_with("JIS")) {
        return std::nullopt;
    } else {
        made = utf8(text);
    }

    while (!made.empty() && (made.back() == ' ' || made.back() == '\0')) made.pop_back();

    return made;
}

// A time as EXIF writes it ("2026:11:17 21:30:00") in ISO 8601 ("2026-11-17T21:30:00"), with the
// offset of EXIF 2.31 ("+01:00") after it when there is one; none for a time left blank or zero.
[[nodiscard]] std::string iso(std::string_view stamp, std::string_view offset) {
    constexpr std::string_view pattern = "dddd:dd:dd dd:dd:dd";

    if (stamp.size() < pattern.size()) return {};

    for (std::size_t at = 0; at < pattern.size(); ++at) {
        const char one = stamp[at];

        if (pattern[at] == 'd' ? (one < '0' || one > '9') : one != pattern[at]) return {};
    }

    if (stamp.starts_with("0000")) return {};

    std::string made{stamp.substr(0, pattern.size())};

    made[4] = '-';
    made[7] = '-';
    made[10] = 'T';

    const bool signed_offset = offset.size() == 6 && (offset[0] == '+' || offset[0] == '-') && offset[3] == ':';

    if (signed_offset) made += offset;

    return made;
}

// Degrees from the three fractions of a GPS coordinate (degrees, minutes, seconds), or none when a
// denominator is zero.
[[nodiscard]] std::optional<double> degrees(std::span<const std::byte> data, bool big) {
    double made = 0;
    double scale = 1;

    for (std::size_t one = 0; one < 3; ++one) {
        const std::uint64_t over = unsigned_at(data, one * 8, 4, big);
        const std::uint64_t under = unsigned_at(data, one * 8 + 4, 4, big);

        if (under == 0) return std::nullopt;

        made += static_cast<double>(over) / static_cast<double>(under) / scale;
        scale *= 60;
    }

    return made;
}

// The reader of one TIFF structure: the file, where its header stands and where it ends, its
// byte order, and what it gathers.
class Tiff {
public:
    // A reader of the structure in [base, end) of `from`.
    Tiff(Reader& from, std::uint64_t size, std::uint64_t base, std::uint64_t end, Gathered& out, Shot& shot)
        : walk_(from, size, base, end), base_(base), out_(out), shot_(shot) {}

    [[nodiscard]] Task<bool> headed();

    [[nodiscard]] Task<void> read(bool whole);

private:
    [[nodiscard]] Task<std::optional<std::vector<Entry>>> directory(std::uint64_t offset, std::uint64_t* following);

    [[nodiscard]] Task<std::optional<Bytes>> data(const Entry& entry);

    [[nodiscard]] Task<void> fields(std::string_view group, const std::vector<Entry>& entries);

    [[nodiscard]] Task<void> dimensions(const std::vector<Entry>& entries);

    [[nodiscard]] Task<void> thumbnail(const std::vector<Entry>& entries);

    [[nodiscard]] std::uint32_t number(const Entry& entry) const;

    [[nodiscard]] static const Entry* found(const std::vector<Entry>& entries, std::uint16_t tag);

    Walk walk_;
    std::uint64_t base_;
    Gathered& out_;
    Shot& shot_;
    bool big_ = false;
    std::uint64_t first_ = 0;
    std::set<std::uint64_t> seen_;
    std::vector<std::pair<std::uint16_t, Bytes>> blocks_;
    std::uint64_t read_ = 0;
};

// The header: "II" or "MM", 42 (or the marks of raw files built on TIFF: "RO" of Olympus, "SR" of
// Olympus too, 0x55 of Panasonic), and the offset of IFD0. False when there is no TIFF here.
Task<bool> Tiff::headed() {
    co_await walk_.need(8);

    const Bytes head = walk_.bytes(8);

    if (walk_.spoiled() || head.size() < 8) co_return false;

    const std::uint32_t first = sample(head[0]);

    if (first != sample(head[1]) || (first != 'I' && first != 'M')) co_return false;

    big_ = first == 'M';

    const std::uint64_t mark = unsigned_at(head, 2, 2, big_);

    if (mark != 42 && mark != 0x4f52 && mark != 0x5352 && mark != 0x55) co_return false;

    first_ = unsigned_at(head, 4, 4, big_);

    co_return true;
}

// The value of a SHORT or LONG entry with one value, zero for any other.
std::uint32_t Tiff::number(const Entry& entry) const {
    if (entry.count != 1) return 0;

    if (entry.type == 3) return static_cast<std::uint32_t>(unsigned_at(entry.raw, 0, 2, big_));

    if (entry.type == 4 || entry.type == 13) return static_cast<std::uint32_t>(unsigned_at(entry.raw, 0, 4, big_));

    return 0;
}

// The entry of a tag in a directory, or null.
const Entry* Tiff::found(const std::vector<Entry>& entries, std::uint16_t tag) {
    for (const Entry& one : entries)
        if (one.tag == tag) return &one;

    return nullptr;
}

// The entries of the directory at `offset` (from the header), and the offset of the next one in
// `following` when asked; none when it is outside the structure, read already, or too long.
Task<std::optional<std::vector<Entry>>> Tiff::directory(std::uint64_t offset, std::uint64_t* following) {
    if (offset == 0 || offset >= walk_.end() - base_ || !seen_.insert(offset).second) co_return std::nullopt;

    walk_.seek(base_ + offset);
    co_await walk_.need(2);

    const Bytes head = walk_.bytes(2);

    if (head.size() < 2) {
        out_.damaged();

        co_return std::nullopt;
    }

    const auto many = static_cast<std::size_t>(unsigned_at(head, 0, 2, big_));

    if (many > most_entries) {
        out_.damaged();

        co_return std::nullopt;
    }

    co_await walk_.need(12 * many + 4);

    const Bytes body = walk_.bytes(12 * many);
    std::vector<Entry> made;

    for (std::size_t one = 0; one + 12 <= body.size(); one += 12) {
        Entry entry;

        entry.tag = static_cast<std::uint16_t>(unsigned_at(body, one, 2, big_));
        entry.type = static_cast<std::uint16_t>(unsigned_at(body, one + 2, 2, big_));
        entry.count = static_cast<std::uint32_t>(unsigned_at(body, one + 4, 4, big_));
        std::copy_n(body.begin() + static_cast<std::ptrdiff_t>(one + 8), 4, entry.raw.begin());
        made.push_back(entry);
    }

    if (following != nullptr) {
        const Bytes next = walk_.bytes(4);

        *following = next.size() == 4 ? unsigned_at(next, 0, 4, big_) : 0;
    }

    if (body.size() < 12 * many) out_.damaged();

    co_return made;
}

// The bytes of an entry's values: the four of the entry when they fit there, otherwise read at its
// offset; none for a type there is not, a value longer than `most_text`, or one outside the
// structure or past the budget of reading, which is damage.
Task<std::optional<Bytes>> Tiff::data(const Entry& entry) {
    if (entry.type == 0 || entry.type >= sizes.size()) co_return std::nullopt;

    const std::uint64_t each = sizes[entry.type];

    if (entry.count > most_text / each) co_return std::nullopt;

    const auto much = static_cast<std::size_t>(entry.count * each);

    if (much <= 4) co_return Bytes(entry.raw.begin(), entry.raw.begin() + static_cast<std::ptrdiff_t>(much));

    const std::uint64_t offset = unsigned_at(entry.raw, 0, 4, big_);

    if (offset > walk_.end() - base_ || much > walk_.end() - base_ - offset || much > walk_.end() - base_ + most_spare - read_) {
        out_.damaged();

        co_return std::nullopt;
    }

    read_ += much;
    walk_.seek(base_ + offset);

    Bytes made = co_await walk_.take(much);

    if (made.size() < much) {
        out_.damaged();

        co_return std::nullopt;
    }

    co_return made;
}

// The fields of a directory of the group, and what they say of the photo: the camera and the
// orientation in IFD0, the time in the EXIF directory, the place in the GPS directory. A pointer
// to the EXIF or the GPS directory, or to the interoperability directory in EXIF, reads that
// directory where the pointer stands, so the fields come in the order exiv2 gives them. XMP, IPTC
// and Photoshop's resources standing in IFD0 are kept to be read after the structure.
Task<void> Tiff::fields(std::string_view group, const std::vector<Entry>& entries) {
    std::optional<double> latitude;
    std::optional<double> longitude;
    std::optional<double> altitude;
    bool south = false;
    bool west = false;
    bool below = false;
    std::string stamp;
    std::string offset;

    for (const Entry& entry : entries) {
        const bool exif_pointer = group == "Image" && entry.tag == 0x8769;
        const bool gps_pointer = group == "Image" && entry.tag == 0x8825;
        const bool iop_pointer = group == "Photo" && entry.tag == 0xa005;

        if (exif_pointer || gps_pointer || iop_pointer) {
            const std::optional<std::vector<Entry>> inner = co_await directory(number(entry), nullptr);

            if (inner) co_await fields(exif_pointer ? "Photo" : gps_pointer ? "GPSInfo" : "Iop", *inner);

            continue;
        }

        if (entry.tag == 0x02bc || entry.tag == 0x83bb || entry.tag == 0x8649) {
            if (group != "Image") continue;

            std::optional<Bytes> given = co_await data(entry);

            if (given) blocks_.emplace_back(entry.tag, std::move(*given));

            continue;
        }

        if (std::ranges::binary_search(structural, entry.tag)) continue;

        const std::optional<Bytes> given = co_await data(entry);

        if (!given) continue;

        std::optional<Value> value;

        if (group == "Photo" && entry.tag == 0x9286) {
            const std::optional<std::string> comment = commented(*given, big_);

            if (comment) value = Value{*comment};
        } else {
            value = valued(entry, *given, big_);
        }

        if (!value) continue;

        const std::string_view name = tag_name(group, entry.tag);
        std::array<char, 8> hex{};

        if (name.empty()) (void)std::snprintf(hex.data(), hex.size(), "0x%04x", static_cast<unsigned>(entry.tag));

        const std::string key = "Exif." + std::string{group} + "." + (name.empty() ? std::string{hex.data()} : std::string{name});
        const std::string* text = std::get_if<std::string>(&*value);

        if (group == "Image" && text != nullptr && entry.tag == 0x010f) shot_.exif.make = *text;

        if (group == "Image" && text != nullptr && entry.tag == 0x0110) shot_.exif.model = *text;

        if (group == "Image" && entry.tag == 0x0112) {
            const std::uint32_t turned = number(entry);

            if (turned >= 1 && turned <= 8) shot_.exif.orientation = static_cast<std::uint8_t>(turned);
        }

        if (group == "Photo" && text != nullptr && entry.tag == 0x9003) stamp = *text;

        if (group == "Photo" && text != nullptr && entry.tag == 0x9011) offset = *text;

        if (group == "GPSInfo" && text != nullptr && entry.tag == 1) south = *text == "S";

        if (group == "GPSInfo" && text != nullptr && entry.tag == 3) west = *text == "W";

        if (group == "GPSInfo" && entry.tag == 5 && entry.type == 1 && entry.count == 1) below = sample((*given)[0]) == 1;

        if (group == "GPSInfo" && (entry.tag == 2 || entry.tag == 4) && entry.type == 5 && entry.count == 3) {
            const std::optional<double> angle = degrees(*given, big_);

            (entry.tag == 2 ? latitude : longitude) = angle;
        }

        if (group == "GPSInfo" && entry.tag == 6 && entry.type == 5 && entry.count == 1) {
            const std::uint64_t under = unsigned_at(*given, 4, 4, big_);

            if (under != 0) altitude = static_cast<double>(unsigned_at(*given, 0, 4, big_)) / static_cast<double>(under);
        }

        out_.field(Scheme::exif, key, std::move(*value));
    }

    if (group == "Photo" && !stamp.empty()) shot_.exif.taken = iso(stamp, offset);

    if (group == "GPSInfo" && latitude && longitude) {
        Place place{south ? -*latitude : *latitude, west ? -*longitude : *longitude, std::nullopt};

        if (altitude) place.altitude = below ? -*altitude : *altitude;

        shot_.exif.place = place;
    }
}

// The dimensions of the image a TIFF file holds: IFD0's, unless IFD0 says it is a reduced image
// (bit 0 of NewSubfileType), as DNG's preview is; then the first sub-image that is not reduced.
Task<void> Tiff::dimensions(const std::vector<Entry>& entries) {
    const auto sized = [this](const std::vector<Entry>& those) {
        const Entry* wide = found(those, 0x0100);
        const Entry* high = found(those, 0x0101);

        if (wide != nullptr && high != nullptr) {
            shot_.width = number(*wide);
            shot_.height = number(*high);
        }
    };

    const Entry* kind = found(entries, 0x00fe);
    const Entry* subs = found(entries, 0x014a);

    if (kind == nullptr || (number(*kind) & 1) == 0 || subs == nullptr || (subs->type != 4 && subs->type != 13)) {
        sized(entries);

        co_return;
    }

    const std::optional<Bytes> offsets = co_await data(*subs);

    if (!offsets) co_return;

    for (std::size_t one = 0; one < std::min<std::size_t>(subs->count, most_subs); ++one) {
        const std::optional<std::vector<Entry>> sub = co_await directory(unsigned_at(*offsets, one * 4, 4, big_), nullptr);

        if (!sub) continue;

        const Entry* sub_kind = found(*sub, 0x00fe);

        if (sub_kind == nullptr || (number(*sub_kind) & 1) == 0) {
            sized(*sub);

            co_return;
        }
    }
}

// The thumbnail of IFD1: a JPEG at JPEGInterchangeFormat, as long as JPEGInterchangeFormatLength,
// entered as a picture and handed over when its bytes are wanted.
Task<void> Tiff::thumbnail(const std::vector<Entry>& entries) {
    const Entry* at = found(entries, 0x0201);
    const Entry* long_ = found(entries, 0x0202);

    if (at == nullptr || long_ == nullptr) co_return;

    const std::uint64_t offset = number(*at);
    const std::uint64_t length = number(*long_);

    if (length == 0 || offset > walk_.end() - base_ || length > walk_.end() - base_ - offset) {
        out_.damaged();

        co_return;
    }

    if (!out_.pictured(Picture{Role::other, "image/jpeg", "thumbnail", static_cast<std::size_t>(length)})) co_return;

    walk_.seek(base_ + offset);

    Bytes taken = co_await walk_.take(static_cast<std::size_t>(length));

    if (taken.size() < length) {
        out_.damaged();

        co_return;
    }

    out_.took(std::move(taken));
}

// Reads IFD0 and IFD1 and the directories IFD0 and EXIF point to; with `whole` the TIFF is the
// image, and IFD0 gives its dimensions. Then the blocks of XMP, IPTC and Photoshop that IFD0 held,
// so that all of EXIF comes first, as exiv2 gives it.
Task<void> Tiff::read(bool whole) {
    std::uint64_t second = 0;
    const std::optional<std::vector<Entry>> zero = co_await directory(first_, &second);

    if (!zero) {
        out_.damaged();

        co_return;
    }

    co_await fields("Image", *zero);

    if (whole) co_await dimensions(*zero);

    const std::optional<std::vector<Entry>> one = co_await directory(second, nullptr);

    if (one) {
        co_await fields("Thumbnail", *one);
        co_await thumbnail(*one);
    }

    for (const auto& [tag, given] : blocks_) {
        if (tag == 0x02bc) xmp(std::string_view{reinterpret_cast<const char*>(given.data()), given.size()}, out_, shot_);
        else if (tag == 0x83bb) iim(given, out_, shot_);
        else resources(given, out_, shot_);
    }
}

}

// Reads the TIFF structure in [base, end) of `from`: its fields, what it says of the photo and its
// thumbnail; with `whole` it is the image itself and gives its dimensions. False when no TIFF
// header stands at `base`.
Task<bool> exif(Reader& from, std::uint64_t size, std::uint64_t base, std::uint64_t end, bool whole, Gathered& out, Shot& shot) {
    Tiff tiff{from, size, base, end, out, shot};

    const bool headed = co_await tiff.headed();

    if (!headed) co_return false;

    co_await tiff.read(whole);

    co_return true;
}

}
