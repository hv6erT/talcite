#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/crypto.hpp"
#include "tal/trouble.hpp"

#include "tagging.hpp"

// IPTC-IIM 4.2: datasets of a tag mark (0x1C), a record, a number, a size and the data; a size
// with its top bit set says how many bytes the real size takes, which the datasets of text never
// need, so such a dataset is skipped. The records read are those exiv2 reads, the envelope (1)
// and the application record (2), each dataset a field keyed as exiv2 keys it
// ("Iptc.Application2.Keywords"), a repeated dataset a field each time, in file order. The few
// datasets of IIM that are binary numbers (the versions, the file format, the preview's format)
// are numbers; the rest is text: valid UTF-8 as it is; otherwise, when the envelope's
// CodedCharacterSet says UTF-8 (ESC % G), UTF-8 with its broken sequences replaced, and else
// Latin-1, the usual coding of old cameras and programs.
// The preview (2:202) is no field.
//
// The time a photo was taken: DateCreated (2:55, CCYYMMDD) with TimeCreated (2:60, HHMMSS and an
// offset ±HHMM), in ISO 8601.
//
// Photoshop's image resources hold IPTC as resource 0x0404 ("8BIM", the number, a Pascal name
// padded to an even length, a size, the data padded to an even length).
namespace tal::meta::tagging {

namespace {

// A byte as an unsigned number.
[[nodiscard]] std::uint32_t sample(std::byte one) { return std::to_integer<std::uint32_t>(one); }

// The datasets that are binary numbers, as record times 256 plus number.
constexpr std::array<std::uint16_t, 8> numeric_sets{0x0100, 0x0114, 0x0116, 0x0178, 0x017a, 0x0200, 0x02c8, 0x02c9};

// The time from DateCreated and TimeCreated in ISO 8601, or nothing when the date is not eight
// digits; a time that is not six digits and an offset is left out.
[[nodiscard]] std::string when(std::string_view date, std::string_view time) {
    const auto digits = [](std::string_view text) {
        for (const char one : text)
            if (one < '0' || one > '9') return false;

        return true;
    };

    if (date.size() != 8 || !digits(date) || date == "00000000") return {};

    std::string made = std::string{date.substr(0, 4)} + "-" + std::string{date.substr(4, 2)} + "-" + std::string{date.substr(6, 2)};

    if (time.size() == 11 && digits(time.substr(0, 6)) && (time[6] == '+' || time[6] == '-') && digits(time.substr(7))) {
        made += "T" + std::string{time.substr(0, 2)} + ":" + std::string{time.substr(2, 2)} + ":" + std::string{time.substr(4, 2)};
        made += std::string{time.substr(6, 3)} + ":" + std::string{time.substr(9, 2)};
    }

    return made;
}

}

// Reads the datasets of records 1 and 2 into fields, and the time they say the photo was taken.
// Bytes that are no dataset end the reading and damage the file.
void iim(std::span<const std::byte> data, Gathered& out, Shot& shot) {
    bool utf = false;

    for (std::size_t pass = 0; pass < 2; ++pass) {
        std::size_t at = 0;
        std::string date;
        std::string time;

        while (at < data.size()) {
            if (sample(data[at]) != 0x1C || at + 5 > data.size()) {
                if (pass == 1 && sample(data[at]) != 0) out.damaged();

                break;
            }

            const std::uint32_t record = sample(data[at + 1]);
            const std::uint32_t number = sample(data[at + 2]);
            const std::uint32_t size = (sample(data[at + 3]) << 8) | sample(data[at + 4]);

            if ((size & 0x8000) != 0) {
                const std::size_t wide = size & 0x7FFF;
                std::uint64_t real = 0;

                if (wide > 8 || at + 5 + wide > data.size()) {
                    if (pass == 1) out.damaged();

                    break;
                }

                for (std::size_t one = 0; one < wide; ++one) real = (real << 8) | sample(data[at + 5 + one]);

                if (real > data.size() - at - 5 - wide) {
                    if (pass == 1) out.damaged();

                    break;
                }

                at += 5 + wide + static_cast<std::size_t>(real);

                continue;
            }

            if (size > data.size() - at - 5) {
                if (pass == 1) out.damaged();

                break;
            }

            const std::span<const std::byte> value = data.subspan(at + 5, size);

            at += 5 + size;

            if (pass == 0) {
                if (record == 1 && number == 90) utf = value.size() == 3 && sample(value[0]) == 0x1B && sample(value[1]) == '%' && sample(value[2]) == 'G';

                continue;
            }

            if ((record != 1 && record != 2) || (record == 2 && number == 202)) continue;

            const std::string_view name = dataset_name(static_cast<std::uint8_t>(record), static_cast<std::uint8_t>(number));
            std::array<char, 8> hex{};

            if (name.empty()) (void)std::snprintf(hex.data(), hex.size(), "0x%04x", static_cast<unsigned>(number));

            const std::string key = std::string{record == 1 ? "Iptc.Envelope." : "Iptc.Application2."} + (name.empty() ? std::string{hex.data()} : std::string{name});

            if (std::ranges::binary_search(numeric_sets, static_cast<std::uint16_t>(record * 256 + number))) {
                std::int64_t made = 0;

                for (const std::byte one : value) made = (made << 8) | static_cast<std::int64_t>(sample(one));

                if (value.size() <= 4) out.field(Scheme::iptc, key, made);

                continue;
            }

            std::string text = valid_utf8(value) ? std::string(reinterpret_cast<const char*>(value.data()), value.size()) : utf ? utf8(value) : latin1(value);

            if (record == 2 && number == 55) date = text;

            if (record == 2 && number == 60) time = text;

            out.field(Scheme::iptc, key, std::move(text));
        }

        if (pass == 1 && !date.empty()) shot.iptc.taken = when(date, time);
    }
}

// Reads Photoshop's image resources for IPTC (resource 0x0404). Bytes that are no resource end the
// reading and damage the file.
void resources(std::span<const std::byte> data, Gathered& out, Shot& shot) {
    std::size_t at = 0;

    while (at + 7 <= data.size()) {
        const std::string_view sign{reinterpret_cast<const char*>(data.data() + at), 4};

        if (sign != "8BIM") {
            out.damaged();

            return;
        }

        const std::uint32_t id = (sample(data[at + 4]) << 8) | sample(data[at + 5]);
        const std::size_t named = sample(data[at + 6]);
        const std::size_t padded = (named + 2) & ~std::size_t{1};

        if (at + 6 + padded + 4 > data.size()) {
            out.damaged();

            return;
        }

        const std::size_t head = at + 6 + padded;
        const std::uint64_t size = (std::uint64_t{sample(data[head])} << 24) | (sample(data[head + 1]) << 16) | (sample(data[head + 2]) << 8) | sample(data[head + 3]);

        if (size > data.size() - head - 4) {
            out.damaged();

            return;
        }

        if (id == 0x0404) iim(data.subspan(head + 4, static_cast<std::size_t>(size)), out, shot);

        at = head + 4 + static_cast<std::size_t>(size) + (size % 2);
    }
}

namespace {

// A dataset read for rewriting: its record, its number and its bytes.
struct Dataset {
    std::uint8_t record = 0;
    std::uint8_t number = 0;
    Bytes value;
};

// The datasets of IIM, in file order; a dataset of extended size, which no text needs, is kept
// as it is under record 0, written back unchanged. Bytes that are no dataset cannot be rewritten.
[[nodiscard]] std::vector<Dataset> datasets(std::span<const std::byte> data) {
    std::vector<Dataset> made;
    std::size_t at = 0;

    while (at < data.size() && sample(data[at]) == 0x1C) {
        if (at + 5 > data.size()) throw Trouble{Kind::malformed, "The IPTC of the file is cut short."};

        const std::uint32_t size = (sample(data[at + 3]) << 8) | sample(data[at + 4]);
        std::size_t end = at + 5 + size;

        if ((size & 0x8000) != 0) {
            const std::size_t wide = size & 0x7FFF;
            std::uint64_t real = 0;

            if (wide > 8 || at + 5 + wide > data.size()) throw Trouble{Kind::malformed, "The IPTC of the file is cut short."};

            for (std::size_t one = 0; one < wide; ++one) real = (real << 8) | sample(data[at + 5 + one]);

            if (real > data.size() - at - 5 - wide) throw Trouble{Kind::malformed, "The IPTC of the file is cut short."};

            end = at + 5 + wide + static_cast<std::size_t>(real);
            made.push_back(Dataset{0, 0, Bytes(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(end))});
            at = end;

            continue;
        }

        if (end > data.size()) throw Trouble{Kind::malformed, "The IPTC of the file is cut short."};

        made.push_back(Dataset{static_cast<std::uint8_t>(sample(data[at + 1])), static_cast<std::uint8_t>(sample(data[at + 2])),
                               Bytes(data.begin() + static_cast<std::ptrdiff_t>(at + 5), data.begin() + static_cast<std::ptrdiff_t>(end))});
        at = end;
    }

    return made;
}

// A dataset as bytes: the tag mark, the record, the number, the size and the value; one over
// 32767 bytes, which IIM's plain size cannot hold, is damage to the value.
void put(Bytes& into, const Dataset& one) {
    if (one.record == 0) {
        into.insert(into.end(), one.value.begin(), one.value.end());

        return;
    }

    if (one.value.size() > 0x7FFF) throw Trouble{Kind::malformed, "A value of IPTC is longer than 32767 bytes."};

    into.push_back(std::byte{0x1C});
    into.push_back(std::byte{one.record});
    into.push_back(std::byte{one.number});
    into.push_back(static_cast<std::byte>(one.value.size() >> 8));
    into.push_back(static_cast<std::byte>(one.value.size() & 0xFF));
    into.insert(into.end(), one.value.begin(), one.value.end());
}

// Bytes of a text.
[[nodiscard]] Bytes bytes_of(std::string_view text) { return Bytes(reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data()) + text.size()); }

}

// The datasets `given` with the changes made: every dataset of a changed number goes, and the
// new values come, a dataset each, among record 2 in the order of numbers. The text is UTF-8:
// CodedCharacterSet says so (ESC % G), and text of records 1 and 2 that was in another coding
// (Latin-1, or broken UTF-8 under a declared UTF-8) is written again in UTF-8, while the preview
// (2:202) and the objects of records 3 to 9 stay bytes; RecordVersion is there, as IIM asks.
// Throws `Trouble`: malformed for datasets that cannot be read or a value too long.
Bytes iim_rewritten(std::span<const std::byte> given, const std::vector<IptcChange>& changes) {
    std::vector<Dataset> all = datasets(given);
    bool utf = false;

    for (const Dataset& one : all)
        if (one.record == 1 && one.number == 90) utf = one.value == bytes_of("\x1B%G");

    std::erase_if(all, [](const Dataset& one) { return one.record == 1 && one.number == 90; });

    for (Dataset& one : all) {
        const bool numeric = std::ranges::binary_search(numeric_sets, static_cast<std::uint16_t>(one.record * 256 + one.number));
        const bool text = (one.record == 1 || one.record == 2) && !numeric && !(one.record == 2 && one.number == 202);

        if (text && !valid_utf8(one.value)) one.value = bytes_of(utf ? utf8(one.value) : latin1(one.value));
    }

    for (const IptcChange& change : changes) {
        std::erase_if(all, [&change](const Dataset& one) { return one.record == 2 && one.number == change.number; });

        const bool numeric = std::ranges::binary_search(numeric_sets, static_cast<std::uint16_t>(0x200 + change.number));

        for (const Value& value : change.values) {
            Dataset made{2, change.number, {}};

            if (numeric) {
                const auto* number = std::get_if<std::int64_t>(&value);

                if (number == nullptr || *number < 0 || *number > 0xFFFF) throw Trouble{Kind::malformed, "A number of IPTC takes a value of 0 to 65535."};

                made.value = {static_cast<std::byte>(*number >> 8), static_cast<std::byte>(*number & 0xFF)};
            } else {
                made.value = bytes_of(std::holds_alternative<std::string>(value) ? std::get<std::string>(value) : std::to_string(std::get<std::int64_t>(value)));
            }

            all.push_back(std::move(made));
        }
    }

    if (std::ranges::none_of(all, [](const Dataset& one) { return one.record == 2 && one.number == 0; })) all.push_back(Dataset{2, 0, {std::byte{0}, std::byte{4}}});

    all.push_back(Dataset{1, 90, bytes_of("\x1B%G")});

    std::ranges::stable_sort(all, [](const Dataset& one, const Dataset& other) {
        const int left = one.record == 0 ? 0x10000 : one.record * 256 + one.number;
        const int right = other.record == 0 ? 0x10000 : other.record * 256 + other.number;

        return left < right;
    });

    Bytes made;

    for (const Dataset& one : all) put(made, one);

    return made;
}

// Photoshop's resources `given` (empty: none) with the IPTC (0x0404) replaced by `iim`, or added
// after them. The digest of IPTC (0x0425, MD5) is computed again when weak sums are built in
// (TAL_WEAK_SUMS); otherwise it goes, since a stale one would lie. Throws `Trouble`: malformed for
// resources that cannot be read.
Bytes resources_rewritten(std::span<const std::byte> given, const Bytes& iim) {
    Bytes made;
    bool placed = false;
    std::size_t at = 0;

    const auto resource = [&made](std::span<const std::byte> head, const Bytes& data) {
        made.insert(made.end(), head.begin(), head.end());

        for (int shift = 24; shift >= 0; shift -= 8) made.push_back(static_cast<std::byte>((data.size() >> shift) & 0xFF));

        made.insert(made.end(), data.begin(), data.end());

        if (data.size() % 2 != 0) made.push_back(std::byte{0});
    };

    while (at + 7 <= given.size()) {
        if (std::string_view{reinterpret_cast<const char*>(given.data() + at), 4} != "8BIM") throw Trouble{Kind::malformed, "Photoshop's resources of the file are not readable."};

        const std::uint32_t id = (sample(given[at + 4]) << 8) | sample(given[at + 5]);
        const std::size_t padded = (sample(given[at + 6]) + 2u) & ~std::size_t{1};

        if (at + 6 + padded + 4 > given.size()) throw Trouble{Kind::malformed, "Photoshop's resources of the file are not readable."};

        const std::size_t head = at + 6 + padded;
        const std::uint64_t size = (std::uint64_t{sample(given[head])} << 24) | (sample(given[head + 1]) << 16) | (sample(given[head + 2]) << 8) | sample(given[head + 3]);

        if (size > given.size() - head - 4) throw Trouble{Kind::malformed, "Photoshop's resources of the file are not readable."};

        const std::span<const std::byte> named = given.subspan(at, 6 + padded);

        if (id == 0x0404) {
            resource(named, iim);
            placed = true;
        } else if (id == 0x0425) {
#ifdef TAL_WEAK_SUMS
            crypto::Summing sum{crypto::Sum::md5};

            sum.feed(iim);
            resource(named, sum.done());
#endif
        } else {
            resource(named, Bytes(given.begin() + static_cast<std::ptrdiff_t>(head + 4), given.begin() + static_cast<std::ptrdiff_t>(head + 4 + size)));
        }

        at = head + 4 + static_cast<std::size_t>(size) + (size % 2);
    }

    if (!placed) resource(bytes_of(std::string_view{"8BIM\x04\x04\0\0", 8}), iim);

    return made;
}

}
