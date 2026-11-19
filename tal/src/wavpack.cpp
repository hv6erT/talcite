#include "tagging.hpp"

#include <array>
#include <chrono>
#include <cstdint>

// WavPack properties, ported from talcite-adapters' tags. WavPack tags are APEv2 at the end of the
// file (`ape.cpp`); this file reads the header of the first block: the sample count, the sample
// rate from the table in the flags, the bit depth and the channels. When the sample rate is outside
// the table or there are more than two channels, the metadata sub-blocks of the first block say so
// (ID_SAMPLE_RATE, ID_CHANNEL_INFO); they are walked because they sit right after the header.
namespace tal::meta::tagging {

namespace {

constexpr std::array<std::uint32_t, 15> rates{6000,  8000,  9600,  11025, 12000, 16000, 22050, 24000,
                                              32000, 44100, 48000, 64000, 88200, 96000, 192000};

constexpr std::uint32_t mono = 0x4u;
constexpr std::uint32_t floating = 0x80u;
constexpr std::uint32_t initial = 0x800u;
constexpr std::uint32_t final_ = 0x1000u;

constexpr std::uint8_t channel_info = 0x0D;
constexpr std::uint8_t sample_rate = 0x27;

}

// WavPack from "wvpk": properties from the first block, the bitrate from the size to `walk.end()`.
// The block header (32 bytes) is loaded at once, and so is the head of each sub-block.
Task<void> wavpack(Walk& walk, Gathered& out) {
    const std::uint64_t began = walk.at();

    co_await walk.need(32);

    walk.skip(4);

    const std::uint32_t size = walk.le32();

    walk.skip(2 + 1);

    const std::uint8_t samples_high = walk.u8();
    const std::uint32_t samples_low = walk.le32();

    walk.skip(4 + 4);

    const std::uint32_t flags = walk.le32();

    walk.skip(4);

    out.into.type = "audio/x-wavpack";
    out.codec = "wavpack";

    if (walk.spoiled() || size < 24) co_return out.damaged();

    const std::uint32_t index = (flags >> 23) & 0xFu;
    std::uint32_t rate = index < rates.size() ? rates[index] : 0;
    std::uint16_t channels = (flags & mono) != 0 ? 1 : 2;
    const bool whole = (flags & initial) != 0 && (flags & final_) != 0;
    bool told = false;

    // Sub-blocks: an identifier (0x80 means a three-byte length, 0x40 means one byte shorter), the
    // length in two-byte words, the content.
    Walk blocks = walk.part(size - 24);

    while (blocks.left() >= 2) {
        co_await blocks.need(4);

        const std::uint8_t id = blocks.u8();
        std::uint32_t words = blocks.u8();

        // The two bytes in two statements: the halves of one expression have no order.
        if ((id & 0x80u) != 0) {
            const std::uint32_t middle = blocks.u8();

            words |= middle << 8 | static_cast<std::uint32_t>(blocks.u8()) << 16;
        }

        Walk body = blocks.part(static_cast<std::uint64_t>(words) * 2);
        const std::uint8_t kind = id & 0x3Fu;

        co_await body.need(3);

        if (kind == channel_info && body.left() >= 1) {
            channels = body.u8();
            told = true;
        }

        if (kind == sample_rate && body.left() >= 3) {
            const std::uint32_t low = body.u8();
            const std::uint32_t middle = body.u8();

            rate = low | (middle << 8) | (static_cast<std::uint32_t>(body.u8()) << 16);
        }

        if (blocks.spoiled()) break;
    }

    // A block that is not both the first and the last carries only some of the channels; without
    // the sub-block giving their number, the count is unknown.
    if (!whole && !told) channels = 0;

    if (rate > 0) out.into.properties.rate = rate;

    if (channels > 0) out.into.properties.channels = channels;

    out.into.properties.depth = (flags & floating) != 0 ? std::uint16_t{32} : static_cast<std::uint16_t>(((flags & 3u) + 1) * 8);

    const std::uint64_t samples = (static_cast<std::uint64_t>(samples_high) << 32) | samples_low;

    if (rate == 0 || samples_low == 0xFFFFFFFFu) co_return;

    const std::uint64_t ms = samples * 1000 / rate;

    out.into.properties.duration = std::chrono::milliseconds{static_cast<std::int64_t>(ms)};

    if (ms > 0) out.into.properties.bitrate = static_cast<std::uint32_t>((walk.end() - began) * 8 * 1000 / ms);
}

}
