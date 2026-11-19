#include "tagging.hpp"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// Ogg: Vorbis, Opus and FLAC in Ogg, ported from talcite-adapters' tags. Ogg consists of pages,
// and the pages carry the packets of a stream; a packet may span several pages (comments with a
// picture in base64 always do). Packets of the first logical stream, the one from the opening page,
// are assembled, and pages of other streams are skipped (Ogg with Theora video next to the audio).
// The first packet says which codec it is and carries the properties; the second carries the
// Vorbis comments; FLAC in Ogg has ordinary FLAC metadata blocks after its header, each in a
// separate packet.
//
// The duration comes from the granule position of the last page of the stream: in samples for
// Vorbis and FLAC, in 48 kHz samples for Opus, minus the pre-skip. The last page starts at most
// 65,307 bytes before the end of the file, which is the size of the largest possible page, so only
// the tail is read.
namespace tal::meta::tagging {

namespace {

constexpr std::size_t tail = 65307 + 27;

// The packets of one stream, page by page, lazily. A broken or truncated page ends the packets and
// sets `spoiled`; so does a packet over the limit.
class Packets {
public:
    // The packets of `from`, a file of `size` bytes, from its first page.
    explicit Packets(Reader& from, std::uint64_t size) : walk_(from, size, 0, size) {}

    // Whether a broken or truncated page, or a packet over `most_packet`, has ended the packets.
    [[nodiscard]] bool spoiled() const { return spoiled_; }

    // The serial number of the stream being assembled, taken from the opening page.
    [[nodiscard]] std::uint32_t serial() const { return serial_; }

    // The position past the last page read; after the header packets it is the start of the audio.
    [[nodiscard]] std::uint64_t at() const { return walk_.at(); }

    // The next complete packet of the stream, reading further pages as needed; none at the end of
    // the file or after damage.
    [[nodiscard]] Task<std::optional<Bytes>> next() {
        while (true) {
            if (!ready_.empty()) {
                Bytes made = std::move(ready_.front());

                ready_.erase(ready_.begin());

                co_return made;
            }

            const bool read = co_await page();

            if (!read) co_return std::nullopt;
        }
    }

private:
    // One page: the header, the table of segment lengths, the content. A segment shorter than 255
    // ends a packet. The header with the longest table is loaded at once, and so is the content of
    // a page of our stream, which is never over 64 KiB.
    Task<bool> page() {
        if (walk_.left() < 27) co_return false;

        co_await walk_.need(27 + 255);

        if (!walk_.ahead("OggS")) {
            spoiled_ = true;

            co_return false;
        }

        walk_.skip(4);

        const std::uint8_t version = walk_.u8();
        const std::uint8_t kind = walk_.u8();

        walk_.skip(8);

        const std::uint32_t serial = walk_.le32();

        walk_.skip(8);

        const std::uint8_t count = walk_.u8();
        const Bytes lacing = walk_.bytes(count);

        if (walk_.spoiled() || version != 0) {
            spoiled_ = true;

            co_return false;
        }

        std::uint64_t body = 0;

        for (const std::byte one : lacing) body += std::to_integer<std::uint32_t>(one);

        if (!begun_) {
            begun_ = true;
            serial_ = serial;
        }

        Walk content = walk_.part(body);

        if (serial != serial_) co_return true;

        // A page without the continuation flag while a packet is unfinished: that packet was cut
        // off and is dropped.
        if ((kind & 1u) == 0) pending_.clear();

        co_await content.need(static_cast<std::size_t>(body));

        for (const std::byte one : lacing) {
            const std::uint32_t much = std::to_integer<std::uint32_t>(one);

            if (pending_.size() + much > most_packet) {
                spoiled_ = true;

                co_return false;
            }

            const Bytes piece = content.bytes(much);

            pending_.insert(pending_.end(), piece.begin(), piece.end());

            if (much < 255) {
                ready_.push_back(std::move(pending_));
                pending_.clear();
            }
        }

        if (content.spoiled()) {
            spoiled_ = true;

            co_return false;
        }

        co_return true;
    }

    Walk walk_;
    bool begun_ = false;
    bool spoiled_ = false;
    std::uint32_t serial_ = 0;
    Bytes pending_;
    std::vector<Bytes> ready_;
};

// Whether the packet starts with the given magic bytes.
[[nodiscard]] bool starts(const Bytes& packet, std::string_view magic) {
    return packet.size() >= magic.size() && std::memcmp(packet.data(), magic.data(), magic.size()) == 0;
}

// A little-endian 32-bit number at offset `at`; zero when fewer than four bytes are left.
[[nodiscard]] std::uint32_t le32_of(const Bytes& given, std::size_t at) {
    if (at + 4 > given.size()) return 0;

    return std::to_integer<std::uint32_t>(given[at]) | (std::to_integer<std::uint32_t>(given[at + 1]) << 8) |
           (std::to_integer<std::uint32_t>(given[at + 2]) << 16) | (std::to_integer<std::uint32_t>(given[at + 3]) << 24);
}

// The granule position of the last page of the stream with the given serial number in a file of
// `size` bytes; none when that page is not visible.
Task<std::optional<std::uint64_t>> last_granule(Reader& from, std::uint64_t size, std::uint32_t serial) {
    const std::uint64_t began = size > tail ? size - tail : 0;
    const Bytes end = co_await from.read(began, static_cast<std::size_t>(size - began));

    std::optional<std::uint64_t> made;

    for (std::size_t at = 0; at + 27 <= end.size(); ++at) {
        if (std::memcmp(end.data() + at, "OggS", 4) != 0 || std::to_integer<int>(end[at + 4]) != 0) continue;

        if (le32_of(end, at + 14) != serial) continue;

        const std::uint64_t granule = le32_of(end, at + 6) | (static_cast<std::uint64_t>(le32_of(end, at + 10)) << 32);

        if (granule != ~std::uint64_t{0}) made = granule;
    }

    co_return made;
}

}

// Parses an Ogg file of `size` bytes: the codec and its properties from the first packet (Vorbis,
// Opus or FLAC; Theora makes the file video/ogg, anything else application/ogg), the comments and
// FLAC pictures from the following packets, and the duration from the granule position of the last
// page. The bitrate is the Vorbis nominal one when given, otherwise the average over the file
// without the header pages.
Task<void> ogg(Reader& from, std::uint64_t size, Gathered& out) {
    Packets packets{from, size};

    const std::optional<Bytes> first = co_await packets.next();

    if (!first) {
        out.into.type = "application/ogg";

        co_return out.damaged();
    }

    enum class Codec { vorbis, opus, flac, other } codec = Codec::other;

    std::uint32_t rate = 0;
    std::uint32_t skipped = 0;
    std::uint32_t nominal = 0;

    if (starts(*first, "\x01vorbis") && first->size() >= 30) {
        codec = Codec::vorbis;
        out.into.type = "audio/ogg";
        out.codec = "vorbis";
        rate = le32_of(*first, 12);
        nominal = le32_of(*first, 20);
        out.into.properties.channels = std::to_integer<std::uint16_t>((*first)[11]);
    } else if (starts(*first, "OpusHead") && first->size() >= 19) {
        codec = Codec::opus;
        out.into.type = "audio/ogg";
        out.codec = "opus";
        rate = 48000;
        skipped = std::to_integer<std::uint32_t>((*first)[10]) | (std::to_integer<std::uint32_t>((*first)[11]) << 8);
        out.into.properties.channels = std::to_integer<std::uint16_t>((*first)[9]);
    } else if (starts(*first, "\x7F" "FLAC") && first->size() >= 13 + 38) {
        codec = Codec::flac;
        out.into.type = "audio/ogg";
        out.codec = "flac";

        Held held{*first};
        Walk info{held, first->size(), 13 + 4, 13 + 4 + 34};

        co_await streaminfo(info, out);

        rate = out.into.properties.rate.value_or(0);
    } else {
        out.into.type = starts(*first, "\x80theora") ? "video/ogg" : "application/ogg";

        co_return;
    }

    if (rate == 0) co_return out.damaged();

    out.into.properties.rate = rate;

    // Comments: in Vorbis and Opus the second packet; in FLAC metadata blocks in the following
    // packets.
    while (!out.done()) {
        std::optional<Bytes> packet = co_await packets.next();

        if (!packet) break;

        // The packet goes to the reader whole, moved, not copied: comments with a picture weigh up
        // to `most_packet`.
        const std::size_t length = packet->size();

        if (codec == Codec::vorbis || codec == Codec::opus) {
            const std::string_view magic = codec == Codec::vorbis ? std::string_view{"\x03vorbis"} : std::string_view{"OpusTags"};

            if (!starts(*packet, magic)) break;

            Held held{std::move(*packet)};
            Walk comments{held, length, magic.size(), length};

            co_await vorbis(comments, out);

            break;
        }

        if (length < 4) break;

        const std::uint8_t head = std::to_integer<std::uint8_t>((*packet)[0]);
        Held held{std::move(*packet)};
        Walk block{held, length, 4, length};

        if ((head & 0x7F) == 4) co_await vorbis(block, out);
        else if ((head & 0x7F) == 6) co_await picture_block(block, out);

        if ((head & 0x80) != 0) break;
    }

    if (packets.spoiled()) out.damaged();

    if (out.wanted) co_return;

    const std::optional<std::uint64_t> granule = co_await last_granule(from, size, packets.serial());

    if (granule && *granule > skipped) {
        const std::uint64_t ms = (*granule - skipped) * 1000 / rate;

        out.into.properties.duration = std::chrono::milliseconds{static_cast<std::int64_t>(ms)};

        if (codec == Codec::vorbis && nominal > 0 && nominal < 0x80000000u) {
            out.into.properties.bitrate = nominal;
        } else if (ms > 0 && size > packets.at()) {
            // Without the header pages: comments with a picture sometimes weigh more than the audio
            // itself.
            out.into.properties.bitrate = static_cast<std::uint32_t>((size - packets.at()) * 8 * 1000 / ms);
        }
    }
}

}
