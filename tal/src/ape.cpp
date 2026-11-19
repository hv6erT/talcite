#include "tagging.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>

// APEv2 and APEv1, ported from talcite-adapters' tags: the tag of Monkey's Audio, WavPack and
// Musepack, also found in MP3 (foobar2000, MP3Gain). It usually sits at the end of the file, before
// ID3v1 and Lyrics3v2, with an "APETAGEX" footer; less often at the start, with a header of the
// same layout. APE keys are case-insensitive, so they are stored in upper case: "Album Artist" from
// foobar2000 and "ALBUM ARTIST" from Mp3tag are one field. A text value with several strings
// separated by zeros gives several fields.
//
// A picture is a binary value with the key "Cover Art (…)": a file name, a zero, the bytes. The
// picture type is recognized from the bytes, because APE does not record it; the role comes from
// the parentheses.
namespace tal::meta::tagging {

namespace {

constexpr std::uint32_t headed_flag = 0x80000000u;
constexpr std::uint32_t header_flag = 0x20000000u;

// Whether 32 bytes form an APE header or footer: they start with "APETAGEX".
[[nodiscard]] bool ape_at(std::span<const std::byte> head) {
    return head.size() >= 32 && std::memcmp(head.data(), "APETAGEX", 8) == 0;
}

// A little-endian 32-bit number at offset `at`; the caller makes sure that four bytes are there.
[[nodiscard]] std::uint32_t le32_of(std::span<const std::byte> given, std::size_t at) {
    return std::to_integer<std::uint32_t>(given[at]) | (std::to_integer<std::uint32_t>(given[at + 1]) << 8) |
           (std::to_integer<std::uint32_t>(given[at + 2]) << 16) | (std::to_integer<std::uint32_t>(given[at + 3]) << 24);
}

// The picture role from the parentheses of a "Cover Art (…)" key, which is already in upper case by
// then.
[[nodiscard]] Role covered(const std::string& key) {
    if (key.find("(FRONT)") != std::string::npos) return Role::front;

    if (key.find("(BACK)") != std::string::npos) return Role::back;

    if (key.find("(ARTIST)") != std::string::npos) return Role::artist;

    return Role::other;
}

// A picture in a binary value: the file name up to a zero (the description), then the bytes.
Task<void> cover(Walk& value, std::string key, Gathered& out) {
    const std::uint64_t size = value.left();
    const Bytes head = co_await value.take(static_cast<std::size_t>(std::min<std::uint64_t>(size, 4096)));
    const std::span<const std::byte> all{head};

    std::size_t named = 0;

    while (named < all.size() && std::to_integer<int>(all[named]) != 0) ++named;

    if (named == all.size()) co_return out.damaged();

    const std::size_t data_at = named + 1;
    const std::uint64_t data = size - data_at;

    Picture one{covered(key), image_type(all.subspan(data_at)), utf8(all.first(named)), static_cast<std::size_t>(data)};

    if (!out.pictured(std::move(one))) co_return;

    value.seek(value.end() - data);

    Bytes taken = co_await value.take(static_cast<std::size_t>(data));

    out.took(std::move(taken));
}

// The items of a tag: value length, flags, key up to a zero, value. The head of an item (two
// numbers and a key of at most 256 bytes) is loaded at once.
Task<void> items(Walk& walk, std::uint32_t count, Gathered& out) {
    for (std::uint32_t which = 0; which < count && walk.left() >= 8 && !out.done(); ++which) {
        co_await walk.need(8 + 256);

        const std::uint32_t size = walk.le32();
        const std::uint32_t flags = walk.le32();

        std::string key;

        while (walk.left() > 0 && key.size() < 256) {
            const std::uint8_t one = walk.u8();

            if (one == 0) break;

            key += static_cast<char>(one);
        }

        if (key.size() < 2 || key.size() >= 256) co_return out.damaged();

        if (size > walk.left()) out.damaged();

        Walk value = walk.part(size);

        key = upper(std::move(key));

        const std::uint32_t kind = (flags >> 1) & 3u;

        if (kind == 1) {
            if (key.starts_with("COVER ART")) co_await cover(value, std::move(key), out);

            continue;
        }

        if (out.wanted || size > most_text) continue;

        const Bytes raw = co_await value.take(size);
        std::span<const std::byte> rest{raw};

        while (!rest.empty()) {
            std::size_t cut = 0;

            while (cut < rest.size() && std::to_integer<int>(rest[cut]) != 0) ++cut;

            if (std::string text = utf8(rest.first(cut)); !text.empty()) out.field(Scheme::ape, key, std::move(text));

            rest = rest.subspan(cut < rest.size() ? cut + 1 : cut);
        }
    }
}

}

// APEv2 (and APEv1) ending at `end` of a file of `size` bytes, skipping a Lyrics3v2 block before
// ID3v1; returns the new end.
Task<std::uint64_t> ape(Reader& from, std::uint64_t size, std::uint64_t end, Gathered& out) {
    // Lyrics3v2 between APE and ID3v1: "LYRICSBEGIN", the content, six digits of length,
    // "LYRICS200".
    if (end >= 15 + 11) {
        const Bytes tail = co_await from.read(end - 15, 15);

        if (tail.size() == 15 && std::memcmp(tail.data() + 6, "LYRICS200", 9) == 0) {
            std::uint64_t much = 0;
            bool digits = true;

            for (std::size_t at = 0; at < 6; ++at) {
                const int one = std::to_integer<int>(tail[at]);

                digits = digits && one >= '0' && one <= '9';
                much = much * 10 + static_cast<std::uint64_t>(one - '0');
            }

            if (digits && much + 15 <= end) {
                const Bytes begun = co_await from.read(end - 15 - much, 11);

                if (begun.size() == 11 && std::memcmp(begun.data(), "LYRICSBEGIN", 11) == 0) end -= 15 + much;
            }
        }
    }

    if (end < 32) co_return end;

    const Bytes footer = co_await from.read(end - 32, 32);

    if (!ape_at(footer)) co_return end;

    const std::uint32_t tagged = le32_of(footer, 12);
    const std::uint32_t count = le32_of(footer, 16);
    const std::uint32_t flags = le32_of(footer, 20);

    if (tagged < 32 || tagged > end) {
        out.damaged();

        co_return end;
    }

    const std::uint64_t began = end - tagged;

    Walk walk{from, size, began, end - 32};

    co_await items(walk, count, out);

    const std::uint64_t header = (flags & headed_flag) != 0 && began >= 32 ? 32 : 0;

    co_return began - header;
}

// APEv2 with a header at the start of the file (Monkey's Audio and WavPack files do not put it
// there, but it happens).
Task<std::uint64_t> ape_ahead(Walk& walk, Gathered& out) {
    if (walk.left() < 32) co_return walk.at();

    const Bytes head = co_await walk.reader().read(walk.at(), 32);

    if (!ape_at(head) || (le32_of(head, 20) & header_flag) == 0) co_return walk.at();

    walk.skip(32);

    const std::uint32_t size = le32_of(head, 12);
    const std::uint32_t count = le32_of(head, 16);

    if (size < 32) {
        out.damaged();

        co_return walk.at();
    }

    Walk body = walk.part(size - 32);

    co_await items(body, count, out);

    walk.skip(32);

    co_return walk.at();
}

}
