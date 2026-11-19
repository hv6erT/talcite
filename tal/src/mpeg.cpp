#include "tagging.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

// Properties of an MPEG audio stream (MP3, MP2, MP1), ported from talcite-adapters' tags. There are
// no tags here: they sit around the stream (ID3v2 before it, APE and ID3v1 after it), and
// `gathered` reads them. This file computes the duration and bitrate from the first frame: from a
// Xing or Info header (LAME, variable bitrate), from a VBRI header (Fraunhofer), and without them
// from the stream size at a constant bitrate, because that is all that is known without walking the
// whole file.
//
// A first frame is accepted only when a second one of the same version, layer and sample rate
// follows it: the bytes FF Ex occur in garbage before the stream, in pictures embedded without a
// tag and in leftover padding, but two matching frames in a row do not occur by chance.
namespace tal::meta::tagging {

namespace {

// How many bytes from the start of the stream are searched for the first frame: enough to cover the
// garbage that programs leave before the stream.
constexpr std::size_t searched = 64 * 1024;

constexpr std::array<std::array<std::uint16_t, 15>, 3> first{{
    {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
    {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
    {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},
}};

constexpr std::array<std::array<std::uint16_t, 15>, 2> later{{
    {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
    {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
}};

constexpr std::array<std::array<std::uint32_t, 3>, 3> rates{{
    {44100, 48000, 32000},
    {22050, 24000, 16000},
    {11025, 12000, 8000},
}};

struct Header {
    int version = 1;
    int layer = 3;
    std::uint32_t bitrate = 0;
    std::uint32_t rate = 0;
    bool mono = false;
    std::uint32_t samples = 0;
    std::uint32_t length = 0;
};

// Version: 0 is MPEG 1, 1 is MPEG 2, 2 is MPEG 2.5. The "free" bitrate (index 0) is rejected:
// without a bitrate there is no frame length, and music does not use it.
[[nodiscard]] std::optional<Header> header(std::uint32_t word) {
    if ((word >> 21) != 0x7FF) return std::nullopt;

    const std::uint32_t version_bits = (word >> 19) & 3u;
    const std::uint32_t layer_bits = (word >> 17) & 3u;
    const std::uint32_t bitrate_bits = (word >> 12) & 15u;
    const std::uint32_t rate_bits = (word >> 10) & 3u;

    if (version_bits == 1 || layer_bits == 0 || bitrate_bits == 0 || bitrate_bits == 15 || rate_bits == 3)
        return std::nullopt;

    Header made;

    const int version = version_bits == 3 ? 0 : version_bits == 2 ? 1 : 2;

    made.version = version;
    made.layer = 4 - static_cast<int>(layer_bits);
    made.bitrate = version == 0 ? first[static_cast<std::size_t>(made.layer - 1)][bitrate_bits]
                                : later[made.layer == 1 ? 0 : 1][bitrate_bits];
    made.rate = rates[static_cast<std::size_t>(version)][rate_bits];
    made.mono = ((word >> 6) & 3u) == 3;

    const std::uint32_t padding = (word >> 9) & 1u;

    if (made.layer == 1) {
        made.samples = 384;
        made.length = (12 * made.bitrate * 1000 / made.rate + padding) * 4;
    } else {
        made.samples = made.layer == 3 && version != 0 ? 576 : 1152;
        made.length = made.samples / 8 * made.bitrate * 1000 / made.rate + padding;
    }

    return made;
}

// Whether two frame headers belong to one stream: the same version, layer and sample rate.
[[nodiscard]] bool fellow(const Header& one, const Header& two) {
    return one.version == two.version && one.layer == two.layer && one.rate == two.rate;
}

}

// An MPEG audio stream from `walk.at()` to `walk.end()`: finds the first frame and computes the
// properties. False when it is not MPEG audio.
Task<bool> mpeg(Walk& walk, Gathered& out) {
    Reader& from = walk.reader();
    const std::uint64_t began = walk.at();
    const std::uint64_t end = walk.end();
    const Bytes head = co_await from.read(began, static_cast<std::size_t>(std::min<std::uint64_t>(searched + 4, end - began)));
    const std::span<const std::byte> all{head};

    std::optional<Header> found;
    std::uint64_t start = 0;

    for (std::size_t at = 0; at + 4 <= all.size() && !found; ++at) {
        if (std::to_integer<int>(all[at]) != 0xFF) continue;

        const std::optional<Header> one = header(be32_of(all, at));

        if (!one) continue;

        const std::uint64_t next = began + at + one->length;

        if (next == end) {
            found = one;
            start = began + at;

            break;
        }

        if (next + 4 > end) continue;

        // The second header is usually among the bytes searched; past them it is read alone.
        std::uint32_t word = 0;

        if (next - began + 4 <= all.size()) {
            word = be32_of(all, static_cast<std::size_t>(next - began));
        } else {
            const Bytes four = co_await from.read(next, 4);

            word = be32_of(four, 0);
        }

        const std::optional<Header> two = header(word);

        if (two && fellow(*one, *two)) {
            found = one;
            start = began + at;
        }
    }

    if (!found) co_return false;

    const Header& one = *found;

    out.into.type = "audio/mpeg";
    out.codec = one.layer == 3 ? "mp3" : one.layer == 2 ? "mp2" : "mp1";
    out.into.properties.rate = one.rate;
    out.into.properties.channels = static_cast<std::uint16_t>(one.mono ? 1 : 2);

    const Bytes frame = co_await from.read(start, one.length);
    const std::span<const std::byte> bytes{frame};

    std::optional<std::uint32_t> frames;
    std::optional<std::uint32_t> sized;

    // Xing/Info follows the side information, whose length depends on the version and the number of
    // channels.
    const std::size_t side = one.version == 0 ? (one.mono ? 17 : 32) : (one.mono ? 9 : 17);
    const std::size_t xing = 4 + side;

    if (bytes.size() >= xing + 16 &&
        (std::memcmp(bytes.data() + xing, "Xing", 4) == 0 || std::memcmp(bytes.data() + xing, "Info", 4) == 0)) {
        const std::uint32_t flags = be32_of(bytes, xing + 4);
        std::size_t at = xing + 8;

        if ((flags & 1u) != 0) {
            frames = be32_of(bytes, at);
            at += 4;
        }

        if ((flags & 2u) != 0 && at + 4 <= bytes.size()) sized = be32_of(bytes, at);
    } else if (bytes.size() >= 4 + 32 + 18 && std::memcmp(bytes.data() + 36, "VBRI", 4) == 0) {
        sized = be32_of(bytes, 36 + 10);
        frames = be32_of(bytes, 36 + 14);
    }

    // A frame with a Xing or VBRI header is a silent frame: it does not count among the frames and
    // is not audio, although the size in the header includes it.
    const std::uint64_t stream = end - start;
    const std::uint64_t told = sized && *sized > 0 && *sized <= stream ? *sized : stream;
    const std::uint64_t audio = frames && told > one.length ? told - one.length : told;

    if (frames && *frames > 0) {
        const std::uint64_t samples = static_cast<std::uint64_t>(*frames) * one.samples;
        const std::uint64_t ms = samples * 1000 / one.rate;

        out.into.properties.duration = std::chrono::milliseconds{static_cast<std::int64_t>(ms)};

        if (ms > 0) out.into.properties.bitrate = static_cast<std::uint32_t>(audio * 8 * 1000 / ms);
    } else {
        const std::uint64_t bps = static_cast<std::uint64_t>(one.bitrate) * 1000;

        out.into.properties.bitrate = static_cast<std::uint32_t>(bps);
        out.into.properties.duration = std::chrono::milliseconds{static_cast<std::int64_t>(audio * 8 * 1000 / bps)};
    }

    co_return true;
}

}
