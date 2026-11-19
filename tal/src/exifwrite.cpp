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

#include "tal/text.hpp"
#include "tal/trouble.hpp"

#include "tagging.hpp"

// Writing EXIF: a TIFF structure changed in place where it can be and by adding to it otherwise.
// What the structure holds stays at its offset, so the maker's note, whose own offsets count from
// the TIFF header and which no program but its maker understands, and the thumbnail never move. A
// value that does not fit in its entry takes the place of the old value of its tag when it fits
// there, and goes after the end of the structure otherwise; a directory that changes is written
// where it stood when it has no more entries than before, and whole after the end otherwise, and
// then what pointed to it (the header for IFD0, IFD0 for the EXIF and GPS directories) points to
// the new one, the old one staying as bytes nobody points to. A place is taken again only when
// nothing else we can see in the structure (the values and directories of IFD0, IFD1, EXIF, GPS
// and interoperability, the strips, tiles and thumbnail) lies in it; a structure we cannot see
// whole (a type of unknown width, a directory that does not fit) is only added to. So editing the
// same file again and again does not make it grow. The byte order is the structure's own, and a
// new structure is little-endian.
//
// Only the tags whose type we know are written (`writable`): the orientation, the times, the
// description, the artist, the copyright, the rating, the user comment, the fraction of a second
// of the time taken and the place of GPS. A
// value comes in the form `exiv2 -Pv` prints and our reader gives: text for ASCII, numbers for
// SHORT and BYTE, "n/d" for each RATIONAL; a user comment is written as ASCII when it is ASCII,
// otherwise as Unicode (UTF-16 in the structure's byte order). A new GPS directory gets version
// 2.3.0.0.
namespace tal::meta::tagging {

namespace {

// A tag we write: its directory, its number, its type and how many values it takes (zero for
// text of any length).
struct Writable {
    std::string_view group;
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint32_t count = 0;
};

constexpr std::array<Writable, 17> writable{{
    {"Image", 0x010e, 2, 0},  {"Image", 0x0112, 3, 1},   {"Image", 0x013b, 2, 0},   {"Image", 0x8298, 2, 0},   {"Image", 0x4746, 3, 1},
    {"Photo", 0x9003, 2, 0},  {"Photo", 0x9004, 2, 0},   {"Photo", 0x9011, 2, 0},   {"Photo", 0x9286, 7, 0},   {"GPSInfo", 0x0000, 1, 4},
    {"GPSInfo", 0x0001, 2, 0}, {"GPSInfo", 0x0002, 5, 3}, {"GPSInfo", 0x0003, 2, 0}, {"GPSInfo", 0x0004, 5, 3}, {"GPSInfo", 0x0005, 1, 1},
    {"GPSInfo", 0x0006, 5, 1}, {"Photo", 0x9291, 2, 0},
}};

// An entry as it stands in a directory: the tag, the type, the count and the four bytes of value
// or offset, in the structure's byte order.
struct Raw {
    std::uint16_t tag = 0;
    std::uint16_t type = 0;
    std::uint32_t count = 0;
    std::array<std::byte, 4> four{};
};

// A directory read: where it stood (0: new), how many entries it had room for, its entries, the
// offset of the one after it.
struct Directory {
    std::uint32_t offset = 0;
    std::size_t room = 0;
    std::vector<Raw> entries;
    std::uint32_t next = 0;
    bool changed = false;
};

// Whose a place in the structure is: a directory (0 IFD0, 1 EXIF, 2 GPS, 3 IFD1, 4
// interoperability) and the tag whose value it holds, or none for the directory itself or for
// data other tags point to (strips, tiles, the thumbnail).
struct Owner {
    int directory = 0;
    std::optional<std::uint16_t> tag;

    bool operator==(const Owner&) const = default;
};

// A place in the structure and whose it is.
struct Span {
    std::uint64_t from = 0;
    std::uint64_t much = 0;
    Owner owner;
};

// How many entries a directory may have before the structure is too strange to rewrite.
constexpr std::size_t most_entries = 1000;

// A byte as an unsigned number.
[[nodiscard]] std::uint32_t sample(std::byte one) { return std::to_integer<std::uint32_t>(one); }

// The structure being written: its bytes so far and its byte order.
class Structure {
public:
    // Over the bytes of a structure, or a new one when they are empty.
    explicit Structure(std::span<const std::byte> given);

    void change(const ExifChange& one);

    [[nodiscard]] Bytes done();

private:
    [[nodiscard]] std::uint32_t read(std::size_t at, std::size_t width) const;

    void put(Bytes& into, std::uint64_t value, std::size_t width) const;

    [[nodiscard]] std::optional<Directory> directory(std::uint32_t offset) const;

    [[nodiscard]] Directory& of(std::string_view group);

    [[nodiscard]] std::array<std::byte, 4> stored(const Bytes& value);

    [[nodiscard]] std::uint32_t appended(const Directory& one);

    [[nodiscard]] std::uint32_t placed(const Directory& one, int which);

    [[nodiscard]] std::uint32_t pointed(const Raw& one) const;

    [[nodiscard]] std::optional<std::vector<std::uint32_t>> numbers(const Raw& one) const;

    void seen(const Directory& one, int which);

    [[nodiscard]] bool free_for(std::uint64_t from, std::uint64_t much, const Owner& owner) const;

    [[nodiscard]] std::uint64_t room_at(std::uint64_t from, const Owner& owner) const;

    [[nodiscard]] Bytes encoded(const Writable& kind, const std::vector<Value>& values) const;

    Bytes bytes_;
    std::size_t first_size_ = 0;
    bool big_ = false;
    bool whole_ = true;
    std::vector<Span> spans_;
    Directory zero_;
    std::optional<Directory> photo_;
    std::optional<Directory> gps_;
};

// How many bytes one value of a TIFF type takes, or 0 for a type we do not know.
[[nodiscard]] std::uint64_t width_of(std::uint16_t type) {
    switch (type) {
    case 1: case 2: case 6: case 7: return 1;
    case 3: case 8: return 2;
    case 4: case 9: case 11: case 13: return 4;
    case 5: case 10: case 12: case 16: case 17: case 18: return 8;
    default: return 0;
    }
}

// The tokens of values given as numbers or as text of numbers separated by spaces.
[[nodiscard]] std::vector<std::string> tokens(const std::vector<Value>& values) {
    std::vector<std::string> made;

    for (const Value& one : values) {
        if (const auto* number = std::get_if<std::int64_t>(&one)) {
            made.push_back(std::to_string(*number));

            continue;
        }

        const std::string& text = std::get<std::string>(one);
        std::size_t at = 0;

        while (at < text.size()) {
            const std::size_t end = std::min(text.find(' ', at), text.size());

            if (end > at) made.push_back(text.substr(at, end - at));

            at = end + 1;
        }
    }

    return made;
}

// A whole number from a token, within [0, most]; damage to the value otherwise.
[[nodiscard]] std::uint64_t whole(std::string_view token, std::uint64_t most) {
    std::uint64_t made = 0;

    if (token.empty() || token.size() > 10) throw Trouble{Kind::malformed, "A value of EXIF is not a number: " + std::string{token}};

    for (const char one : token) {
        if (one < '0' || one > '9') throw Trouble{Kind::malformed, "A value of EXIF is not a number: " + std::string{token}};

        made = made * 10 + static_cast<std::uint64_t>(one - '0');
    }

    if (made > most) throw Trouble{Kind::malformed, "A value of EXIF is out of range: " + std::string{token}};

    return made;
}

// Reads the structure: the header, IFD0 and the EXIF and GPS directories it points to, and then
// where everything we can see lies. A structure that is not TIFF of 42, or whose IFD0, EXIF or
// GPS directory does not fit in it, cannot be rewritten.
Structure::Structure(std::span<const std::byte> given) : bytes_(given.begin(), given.end()), first_size_(given.size()) {
    if (bytes_.empty()) {
        bytes_ = {std::byte{'I'}, std::byte{'I'}, std::byte{42}, std::byte{0}, std::byte{8}, std::byte{0}, std::byte{0}, std::byte{0}};
        zero_.changed = true;

        return;
    }

    if (bytes_.size() < 8 || sample(bytes_[0]) != sample(bytes_[1]) || (sample(bytes_[0]) != 'I' && sample(bytes_[0]) != 'M'))
        throw Trouble{Kind::malformed, "The EXIF of the file is not TIFF."};

    big_ = sample(bytes_[0]) == 'M';

    if (read(2, 2) != 42) throw Trouble{Kind::malformed, "The EXIF of the file is not TIFF."};

    std::optional<Directory> zero = directory(read(4, 4));

    if (!zero) throw Trouble{Kind::malformed, "The first directory of the EXIF does not fit in it."};

    zero_ = std::move(*zero);

    for (const Raw& one : zero_.entries) {
        if (one.tag == 0x8769 && one.count == 1) {
            photo_ = directory(pointed(one));

            if (!photo_) throw Trouble{Kind::malformed, "The EXIF directory does not fit in the structure."};
        }

        if (one.tag == 0x8825 && one.count == 1) {
            gps_ = directory(pointed(one));

            if (!gps_) throw Trouble{Kind::malformed, "The GPS directory does not fit in the structure."};
        }
    }

    seen(zero_, 0);

    if (photo_) {
        seen(*photo_, 1);

        for (const Raw& one : photo_->entries) {
            if (one.tag != 0xA005 || one.count != 1) continue;

            const std::optional<Directory> interoperability = directory(pointed(one));

            if (interoperability) seen(*interoperability, 4);
            else whole_ = false;
        }
    }

    if (gps_) seen(*gps_, 2);

    if (zero_.next != 0) {
        const std::optional<Directory> one = directory(zero_.next);

        if (one) seen(*one, 3);
        else whole_ = false;
    }
}

// How many bytes from `from` to the end of the place of `owner` that holds it, as the structure
// was; 0 when no place of the owner holds it. A value written in place keeps the whole place of
// the old one, so a later value may be longer than the one before it.
std::uint64_t Structure::room_at(std::uint64_t from, const Owner& owner) const {
    std::uint64_t made = 0;

    for (const Span& one : spans_)
        if (one.owner == owner && one.from <= from && from < one.from + one.much) made = std::max(made, one.from + one.much - from);

    return made;
}

// The offset an entry holds in its four bytes.
std::uint32_t Structure::pointed(const Raw& one) const {
    std::uint32_t made = 0;

    for (std::size_t at = 0; at < 4; ++at) made = (made << 8) | sample(one.four[big_ ? at : 3 - at]);

    return made;
}

// The numbers of an entry of SHORT or LONG, where they stand in it or after it; none when it is
// of another type, has more than `most_entries` or lies outside the structure.
std::optional<std::vector<std::uint32_t>> Structure::numbers(const Raw& one) const {
    const std::uint64_t width = one.type == 3 ? 2 : one.type == 4 ? 4 : 0;

    if (width == 0 || one.count > most_entries) return std::nullopt;

    const std::uint64_t much = width * one.count;
    const std::uint64_t from = much <= 4 ? 0 : pointed(one);

    if (much > 4 && from + much > bytes_.size()) return std::nullopt;

    std::vector<std::uint32_t> made;

    for (std::uint64_t index = 0; index < one.count; ++index) {
        std::uint32_t number = 0;

        for (std::uint64_t at = 0; at < width; ++at) {
            const std::uint64_t which = big_ ? at : width - 1 - at;
            const std::byte part = much <= 4 ? one.four[index * width + which] : bytes_[from + index * width + which];

            number = (number << 8) | sample(part);
        }

        made.push_back(number);
    }

    return made;
}

// Notes where a directory and what it points to lie: itself, each value that does not fit in its
// entry, and the strips, tiles or thumbnail its pairs of offsets and lengths name. A type of
// unknown width or a pair that cannot be read leaves the structure not seen whole.
void Structure::seen(const Directory& one, int which) {
    spans_.push_back({one.offset, 2 + 12 * one.room + 4, {which, std::nullopt}});

    for (const Raw& entry : one.entries) {
        const std::uint64_t width = width_of(entry.type);

        if (width == 0) {
            whole_ = false;

            continue;
        }

        if (width * entry.count > 4) spans_.push_back({pointed(entry), width * entry.count, {which, entry.tag}});
    }

    constexpr std::array<std::pair<std::uint16_t, std::uint16_t>, 3> pairs{{{0x0111, 0x0117}, {0x0144, 0x0145}, {0x0201, 0x0202}}};

    for (const auto& [where, size] : pairs) {
        const auto offsets = std::ranges::find(one.entries, where, &Raw::tag);
        const auto lengths = std::ranges::find(one.entries, size, &Raw::tag);

        if (offsets == one.entries.end() && lengths == one.entries.end()) continue;

        const std::optional<std::vector<std::uint32_t>> starts = offsets == one.entries.end() ? std::nullopt : numbers(*offsets);
        const std::optional<std::vector<std::uint32_t>> counts = lengths == one.entries.end() ? std::nullopt : numbers(*lengths);

        if (!starts || !counts || starts->size() != counts->size()) {
            whole_ = false;

            continue;
        }

        for (std::size_t index = 0; index < starts->size(); ++index) spans_.push_back({(*starts)[index], (*counts)[index], {which, std::nullopt}});
    }
}

// Whether the place from `from` of `much` bytes may be written again for `owner`: the structure
// was seen whole, the place lies in what it was, every place we saw that meets it is the owner's,
// and one of them holds it all.
bool Structure::free_for(std::uint64_t from, std::uint64_t much, const Owner& owner) const {
    if (!whole_ || from < 8 || from + much > first_size_) return false;

    bool held = false;

    for (const Span& one : spans_) {
        if (one.from >= from + much || from >= one.from + one.much) continue;

        if (one.owner != owner) return false;

        held = held || (one.from <= from && from + much <= one.from + one.much);
    }

    return held;
}

// An unsigned integer of `width` bytes at `at`, in the structure's byte order; the caller made
// sure that it is there.
std::uint32_t Structure::read(std::size_t at, std::size_t width) const {
    std::uint32_t made = 0;

    for (std::size_t one = 0; one < width; ++one) made = (made << 8) | sample(bytes_[big_ ? at + one : at + width - 1 - one]);

    return made;
}

// Appends an unsigned integer of `width` bytes in the structure's byte order.
void Structure::put(Bytes& into, std::uint64_t value, std::size_t width) const {
    for (std::size_t one = 0; one < width; ++one) {
        const std::size_t shift = 8 * (big_ ? width - 1 - one : one);

        into.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
    }
}

// The directory at the offset, or none when it does not fit in the structure or has more than
// `most_entries` entries.
std::optional<Directory> Structure::directory(std::uint32_t offset) const {
    if (offset < 8 || offset > bytes_.size() - 2) return std::nullopt;

    const std::size_t many = read(offset, 2);

    if (many > most_entries || offset + 2 + 12 * many + 4 > bytes_.size()) return std::nullopt;

    Directory made;

    made.offset = offset;
    made.room = many;

    for (std::size_t one = 0; one < many; ++one) {
        const std::size_t at = offset + 2 + 12 * one;
        Raw entry;

        entry.tag = static_cast<std::uint16_t>(read(at, 2));
        entry.type = static_cast<std::uint16_t>(read(at + 2, 2));
        entry.count = read(at + 4, 4);
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(at + 8), 4, entry.four.begin());
        made.entries.push_back(entry);
    }

    made.next = read(offset + 2 + 12 * many, 4);

    return made;
}

// The directory of the group, made when there is none: a new GPS directory gets its version.
Directory& Structure::of(std::string_view group) {
    if (group == "Image") return zero_;

    std::optional<Directory>& made = group == "Photo" ? photo_ : gps_;

    if (!made) {
        made = Directory{};
        made->changed = true;

        if (group == "GPSInfo") made->entries.push_back(Raw{0x0000, 1, 4, {std::byte{2}, std::byte{3}, std::byte{0}, std::byte{0}}});
    }

    return *made;
}

// The four bytes of an entry for a value: the value itself when it fits, otherwise the offset of
// the value appended after the end, which is kept at an even offset as TIFF asks.
std::array<std::byte, 4> Structure::stored(const Bytes& value) {
    std::array<std::byte, 4> made{};

    if (value.size() <= 4) {
        std::copy(value.begin(), value.end(), made.begin());

        return made;
    }

    if (bytes_.size() % 2 != 0) bytes_.push_back(std::byte{0});

    Bytes offset;

    put(offset, bytes_.size(), 4);
    std::copy(offset.begin(), offset.end(), made.begin());
    bytes_.insert(bytes_.end(), value.begin(), value.end());

    return made;
}

// The bytes of a value of the kind, in the structure's byte order; damage to the value when it is
// not of its form.
Bytes Structure::encoded(const Writable& kind, const std::vector<Value>& values) const {
    Bytes made;

    if (kind.type == 2) {
        if (values.size() != 1) throw Trouble{Kind::malformed, "A text of EXIF takes one value."};

        const std::string text = std::holds_alternative<std::string>(values[0]) ? std::get<std::string>(values[0]) : std::to_string(std::get<std::int64_t>(values[0]));

        if (text.find('\0') != std::string::npos) throw Trouble{Kind::malformed, "A text of EXIF ends at its first zero byte and cannot hold one."};

        for (const char one : text) made.push_back(static_cast<std::byte>(one));

        made.push_back(std::byte{0});

        return made;
    }

    if (kind.type == 7) {
        if (values.size() != 1 || !std::holds_alternative<std::string>(values[0])) throw Trouble{Kind::malformed, "A user comment of EXIF takes one text."};

        const std::string& text = std::get<std::string>(values[0]);
        const bool ascii = std::ranges::all_of(text, [](char one) { return static_cast<unsigned char>(one) < 0x80; });
        const std::string_view code = ascii ? std::string_view{"ASCII\0\0\0", 8} : std::string_view{"UNICODE\0", 8};

        for (const char one : code) made.push_back(static_cast<std::byte>(one));

        if (ascii) {
            for (const char one : text) made.push_back(static_cast<std::byte>(one));

            return made;
        }

        std::size_t at = 0;

        while (at < text.size()) {
            const char32_t point = text::decoded(text, at).value_or(char32_t{0xFFFD});

            if (point >= 0x10000) {
                put(made, 0xD800 + ((point - 0x10000) >> 10), 2);
                put(made, 0xDC00 + ((point - 0x10000) & 0x3FF), 2);
            } else {
                put(made, point, 2);
            }
        }

        return made;
    }

    const std::vector<std::string> parts = tokens(values);

    if (parts.size() != kind.count) throw Trouble{Kind::malformed, "A value of EXIF has " + std::to_string(parts.size()) + " parts, not " + std::to_string(kind.count) + "."};

    for (const std::string& part : parts) {
        if (kind.type == 1) {
            put(made, whole(part, 0xFF), 1);
        } else if (kind.type == 3) {
            put(made, whole(part, 0xFFFF), 2);
        } else {
            const std::size_t slash = part.find('/');

            const std::uint64_t below = slash == std::string::npos ? 1 : whole(std::string_view{part}.substr(slash + 1), 0xFFFFFFFF);

            if (below == 0) throw Trouble{Kind::malformed, "A fraction of EXIF has nothing below the line: " + part};

            put(made, whole(std::string_view{part}.substr(0, slash), 0xFFFFFFFF), 4);
            put(made, below, 4);
        }
    }

    return made;
}

// Changes one tag: its entry goes, and a new one comes with the values when there are any, its
// value in the place the old one had (`room_at`) when it fits there and the place is free, a text
// then counting the whole place, its zeros after it, so that the place stays its own for the next
// edit; taking away a tag that is not there changes nothing and makes no directory.
void Structure::change(const ExifChange& one) {
    const auto kind = std::ranges::find_if(writable, [&one](const Writable& each) { return each.group == one.group && each.tag == one.tag; });

    if (kind == writable.end()) throw Trouble{Kind::unsupported, "This field of EXIF is not written."};

    if (one.values.empty() && ((one.group == "Photo" && !photo_) || (one.group == "GPSInfo" && !gps_))) return;

    Directory& into = of(one.group);
    const int which = one.group == "Image" ? 0 : one.group == "Photo" ? 1 : 2;
    const auto old = std::ranges::find(into.entries, one.tag, &Raw::tag);
    const std::uint64_t old_much = old == into.entries.end() ? 0 : width_of(old->type) * old->count;
    const std::array<std::byte, 4> old_four = old == into.entries.end() ? std::array<std::byte, 4>{} : old->four;
    const std::uint32_t old_from = old_much > 4 ? pointed(*old) : 0;
    const std::uint64_t room = old_much > 4 ? room_at(old_from, Owner{which, one.tag}) : 0;

    if (std::erase_if(into.entries, [&one](const Raw& entry) { return entry.tag == one.tag; }) > 0) into.changed = true;

    if (one.values.empty()) return;

    into.changed = true;

    const Bytes value = encoded(*kind, one.values);
    const std::uint32_t count = kind->type == 5 ? static_cast<std::uint32_t>(value.size() / 8)
                                : kind->type == 3 ? static_cast<std::uint32_t>(value.size() / 2)
                                                  : static_cast<std::uint32_t>(value.size());

    if (value.size() > 4 && value.size() <= room && free_for(old_from, room, Owner{which, one.tag})) {
        const auto at = bytes_.begin() + static_cast<std::ptrdiff_t>(old_from);

        std::fill_n(std::copy(value.begin(), value.end(), at), room - value.size(), std::byte{0});

        const auto kept = kind->type == 2 && room <= 0xFFFFFFFFu ? static_cast<std::uint32_t>(room) : count;

        into.entries.push_back(Raw{one.tag, kind->type, kept, old_four});

        return;
    }

    into.entries.push_back(Raw{one.tag, kind->type, count, stored(value)});
}

// A directory written after the end, its entries in the order of their tags; its offset.
std::uint32_t Structure::appended(const Directory& one) {
    if (bytes_.size() % 2 != 0) bytes_.push_back(std::byte{0});

    std::vector<Raw> sorted = one.entries;

    std::ranges::sort(sorted, {}, &Raw::tag);

    const auto at = static_cast<std::uint32_t>(bytes_.size());

    put(bytes_, sorted.size(), 2);

    for (const Raw& entry : sorted) {
        put(bytes_, entry.tag, 2);
        put(bytes_, entry.type, 2);
        put(bytes_, entry.count, 4);
        bytes_.insert(bytes_.end(), entry.four.begin(), entry.four.end());
    }

    put(bytes_, one.next, 4);

    return at;
}

// A changed directory (`which` as in `Owner`) written where it stood, when it has no more
// entries than it had room for and the place is free, its unused room cleared; otherwise after the
// end. Its offset.
std::uint32_t Structure::placed(const Directory& one, int which) {
    if (one.offset == 0 || one.entries.size() > one.room || !free_for(one.offset, 2 + 12 * one.room + 4, Owner{which, std::nullopt})) return appended(one);

    std::vector<Raw> sorted = one.entries;

    std::ranges::sort(sorted, {}, &Raw::tag);

    Bytes made;

    put(made, sorted.size(), 2);

    for (const Raw& entry : sorted) {
        put(made, entry.tag, 2);
        put(made, entry.type, 2);
        put(made, entry.count, 4);
        made.insert(made.end(), entry.four.begin(), entry.four.end());
    }

    put(made, one.next, 4);
    made.resize(2 + 12 * one.room + 4, std::byte{0});
    std::ranges::copy(made, bytes_.begin() + static_cast<std::ptrdiff_t>(one.offset));

    return one.offset;
}

// The structure with the changed directories written where they stood or after its end, and
// pointed to when they moved.
Bytes Structure::done() {
    const auto pointing = [this](std::uint16_t tag, std::uint32_t offset) {
        std::erase_if(zero_.entries, [tag](const Raw& entry) { return entry.tag == tag; });

        Bytes four;

        put(four, offset, 4);

        Raw entry{tag, 4, 1, {}};

        std::copy(four.begin(), four.end(), entry.four.begin());
        zero_.entries.push_back(entry);
        zero_.changed = true;
    };

    if (photo_ && photo_->changed) {
        const std::uint32_t at = placed(*photo_, 1);

        if (at != photo_->offset) pointing(0x8769, at);
    }

    if (gps_ && gps_->changed) {
        const std::uint32_t at = placed(*gps_, 2);

        if (at != gps_->offset) pointing(0x8825, at);
    }

    if (zero_.changed) {
        const std::uint32_t at = placed(zero_, 0);

        if (at != zero_.offset) {
            Bytes four;

            put(four, at, 4);
            std::copy(four.begin(), four.end(), bytes_.begin() + 4);
        }
    }

    if (bytes_.size() > 0xFFFFFFFFu) throw Trouble{Kind::unsupported, "The EXIF would grow past four gigabytes."};

    return std::move(bytes_);
}

}

// The TIFF structure `given` (none: a new one) with the changes of EXIF made in place where they
// fit and by adding to it otherwise.
// Throws `Trouble`: unsupported for a tag that is not written, malformed for a value not of its
// form or a structure that cannot be read.
Bytes exif_rewritten(std::span<const std::byte> given, const std::vector<ExifChange>& changes) {
    Structure structure{given};

    for (const ExifChange& one : changes) structure.change(one);

    return structure.done();
}

}
