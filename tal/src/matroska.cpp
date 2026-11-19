#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "tagging.hpp"

// Matroska and WebM (mkv, mka, webm), ported from talcite-adapters' tags. The file is a tree of
// EBML elements: an id of one to four bytes that keeps its marker bits (0x1A45DFA3 for the EBML
// header), a size of one to eight bytes without them, and the content. A size of all ones means
// "unknown": the element lasts to the end of its parent, which live recordings write for the
// Segment and the Clusters.
//
// Read here: the document type from the EBML header (matroska or webm), and from the Segment the
// Info (the duration in units of the timestamp scale, by default a millisecond, and the title of
// the whole file, which MakeMKV and "mkvmerge --title" write, as the field TITLE) and the Tracks:
// every track entry becomes a stream with its codec, language and name, a picture's size, bit
// depth and transfer (PQ and HLG mean HDR), a sound's channels, sample rate and depth. Both stand
// before the first Cluster in what mkvmerge and ffmpeg write, and the reading stops at the first
// Cluster: past it come the frames, possibly gigabytes of them. The tags of the Tags element are
// not read: writers put them after the Clusters, at the end of the file. An element of known size
// that fits in the window is loaded together with its header; a larger one is read piece by piece.
namespace tal::meta::tagging {

namespace {

constexpr std::uint32_t ebml_head = 0x1A45DFA3;
constexpr std::uint32_t doc_type = 0x4282;
constexpr std::uint32_t segment = 0x18538067;
constexpr std::uint32_t info = 0x1549A966;
constexpr std::uint32_t timestamp_scale = 0x2AD7B1;
constexpr std::uint32_t duration = 0x4489;
constexpr std::uint32_t title = 0x7BA9;
constexpr std::uint32_t tracks = 0x1654AE6B;
constexpr std::uint32_t track_entry = 0xAE;
constexpr std::uint32_t track_type = 0x83;
constexpr std::uint32_t codec_id = 0x86;
constexpr std::uint32_t codec_private = 0x63A2;
constexpr std::uint32_t language = 0x22B59C;
constexpr std::uint32_t language_bcp47 = 0x22B59D;
constexpr std::uint32_t track_name = 0x536E;
constexpr std::uint32_t video = 0xE0;
constexpr std::uint32_t pixel_width = 0xB0;
constexpr std::uint32_t pixel_height = 0xBA;
constexpr std::uint32_t colour = 0x55B0;
constexpr std::uint32_t bits_per_channel = 0x55B2;
constexpr std::uint32_t transfer_characteristics = 0x55BA;
constexpr std::uint32_t audio = 0xE1;
constexpr std::uint32_t sampling_frequency = 0xB5;
constexpr std::uint32_t channels = 0x9F;
constexpr std::uint32_t bit_depth = 0x6264;
constexpr std::uint32_t cluster = 0x1F43B675;

// How many bytes of a string element are read; a longer one is cut at a character's boundary.
constexpr std::size_t most_said = 256;

// The default timestamp scale: nanoseconds per unit of the duration, so a unit is a millisecond.
constexpr std::uint64_t millisecond = 1'000'000;

// Longer than this is not a running time but garbage: a hundred thousand hours, in milliseconds.
constexpr double longest = 3.6e11;

struct Element {
    std::uint32_t id = 0;
    Walk body;
};

// The id of the next element, with its marker bits, as the specification writes ids. None for a
// first byte that marks no length of one to four bytes, or past the parent.
[[nodiscard]] std::optional<std::uint32_t> identified(Walk& parent) {
    const std::uint8_t first = parent.u8();
    const int length = first >= 0x80 ? 1 : first >= 0x40 ? 2 : first >= 0x20 ? 3 : first >= 0x10 ? 4 : 0;

    std::uint32_t made = first;

    for (int at = 1; at < length; ++at) made = (made << 8) | parent.u8();

    if (length == 0 || parent.spoiled()) return std::nullopt;

    return made;
}

struct Size {
    std::uint64_t value = 0;
    bool unknown = false;
};

// The size of the next element: the leading zeros of the first byte give its length, the marker
// bit is dropped, and all ones that remain mean an unknown size. None for a first byte of zero
// (a length over eight bytes) or past the parent.
[[nodiscard]] std::optional<Size> sized(Walk& parent) {
    const std::uint8_t first = parent.u8();

    if (first == 0) return std::nullopt;

    const int length = std::countl_zero(first) + 1;
    const std::uint8_t marker = static_cast<std::uint8_t>(0x80u >> (length - 1));

    std::uint64_t value = first & (marker - 1u);
    std::uint64_t ones = marker - 1u;

    for (int at = 1; at < length; ++at) {
        value = (value << 8) | parent.u8();
        ones = (ones << 8) | 0xFFu;
    }

    if (parent.spoiled()) return std::nullopt;

    return Size{value, value == ones};
}

// The next element from the parent's cursor, or none when the parent has ended. An element of
// unknown size takes the rest of the parent; one that promises more than the parent holds is
// clipped, and a broken id or size ends the parent: the file is damaged. A body of known size that
// fits in the window is loaded right after the header, so that the part holds it in memory.
[[nodiscard]] Task<std::optional<Element>> next(Walk& parent, Gathered& out) {
    if (parent.left() < 2) co_return std::nullopt;

    co_await parent.need(4 + 8);

    const std::optional<std::uint32_t> id = identified(parent);
    const std::optional<Size> size = id ? sized(parent) : std::nullopt;

    if (!size) {
        out.damaged();
        parent.seek(parent.end());

        co_return std::nullopt;
    }

    if (size->unknown) co_return Element{*id, parent.part(parent.left())};

    if (size->value > parent.left()) out.damaged();

    if (size->value <= windowed()) co_await parent.need(static_cast<std::size_t>(size->value));

    co_return Element{*id, parent.part(size->value)};
}

// An unsigned integer element: up to eight bytes, big-endian. None when it is longer or truncated.
[[nodiscard]] std::optional<std::uint64_t> unsigned_of(Walk& body) {
    if (body.left() > 8 || body.spoiled()) return std::nullopt;

    std::uint64_t made = 0;

    while (body.left() > 0) made = (made << 8) | body.u8();

    if (body.spoiled()) return std::nullopt;

    return made;
}

// A float element: four or eight bytes, IEEE 754 big-endian; an empty one is zero. None for any
// other length.
[[nodiscard]] std::optional<double> float_of(Walk& body) {
    if (body.spoiled()) return std::nullopt;

    double made = 0;

    if (body.left() == 4) made = std::bit_cast<float>(body.be32());
    else if (body.left() == 8) made = std::bit_cast<double>(body.be64());
    else if (body.left() != 0) return std::nullopt;

    if (body.spoiled()) return std::nullopt;

    return made;
}

struct Seen {
    std::optional<double> duration;
    std::uint64_t scale = millisecond;
    bool info = false;
    bool tracks = false;
    bool video = false;
    bool audio = false;
};

// A string element (ASCII or UTF-8) as UTF-8, without the NULs that may pad it, at most `most_said`
// bytes of it: a longer one is cut before the character the limit falls in, and a broken sequence
// becomes the replacement character (`utf8`).
[[nodiscard]] std::string text_of(Walk& body) {
    const bool cut = body.left() > most_said;
    std::string made = body.chars(static_cast<std::size_t>(std::min<std::uint64_t>(body.left(), most_said)));

    if (cut) {
        std::size_t lead = made.size();

        while (lead > 0 && (static_cast<unsigned char>(made[lead - 1]) & 0xC0) == 0x80) --lead;

        if (lead > 0) {
            const auto first = static_cast<unsigned char>(made[lead - 1]);
            const std::size_t length = (first & 0xE0) == 0xC0 ? 2 : (first & 0xF0) == 0xE0 ? 3 : (first & 0xF8) == 0xF0 ? 4 : 1;

            if (made.size() - (lead - 1) < length) made.resize(lead - 1);
        }
    }

    return utf8(std::as_bytes(std::span{made.data(), made.size()}));
}

// The Info element: the timestamp scale, the duration in its units and the title. Each child is
// loaded up to `most_said` bytes, the most any of them reads.
[[nodiscard]] Task<void> info_read(Walk& body, Seen& seen, Gathered& out) {
    seen.info = true;

    while (true) {
        std::optional<Element> one = co_await next(body, out);

        if (!one) break;

        co_await one->body.need(most_said);

        if (one->id == timestamp_scale) {
            if (const std::optional<std::uint64_t> scale = unsigned_of(one->body); scale && *scale > 0) seen.scale = *scale;
        } else if (one->id == duration) {
            seen.duration = float_of(one->body);
        } else if (one->id == title) {
            if (std::string said = text_of(one->body); !said.empty()) out.field(Scheme::matroska, "TITLE", std::move(said));
        }
    }
}

// The codec of a CodecID, in the short names of the meta streams; an id not listed stays as the
// file says it.
[[nodiscard]] std::string codec_named(const std::string& id) {
    static constexpr std::pair<std::string_view, std::string_view> exact[] = {
        {"V_MPEG4/ISO/AVC", "h264"}, {"V_MPEGH/ISO/HEVC", "hevc"}, {"V_AV1", "av1"}, {"V_VP9", "vp9"}, {"V_VP8", "vp8"},
        {"V_MPEG2", "mpeg2"}, {"V_MPEG1", "mpeg1"}, {"V_FFV1", "ffv1"}, {"V_THEORA", "theora"}, {"V_MJPEG", "mjpeg"},
        {"V_PRORES", "prores"}, {"A_MPEG/L3", "mp3"}, {"A_MPEG/L2", "mp2"}, {"A_MPEG/L1", "mp1"}, {"A_AC3", "ac3"},
        {"A_EAC3", "eac3"}, {"A_TRUEHD", "truehd"}, {"A_FLAC", "flac"}, {"A_ALAC", "alac"}, {"A_OPUS", "opus"},
        {"A_VORBIS", "vorbis"}, {"A_WAVPACK4", "wavpack"}, {"S_TEXT/UTF8", "srt"}, {"S_TEXT/ASCII", "srt"},
        {"S_TEXT/ASS", "ass"}, {"S_TEXT/SSA", "ssa"}, {"S_TEXT/WEBVTT", "webvtt"}, {"S_HDMV/PGS", "pgs"},
        {"S_VOBSUB", "vobsub"}, {"S_DVBSUB", "dvb"}};
    static constexpr std::pair<std::string_view, std::string_view> leading[] = {
        {"V_MPEG4/ISO/", "mpeg4"}, {"A_AAC", "aac"}, {"A_DTS", "dts"}, {"A_PCM/", "pcm"}};

    for (const auto& [given, made] : exact)
        if (id == given) return std::string{made};

    for (const auto& [given, made] : leading)
        if (id.starts_with(given)) return std::string{made};

    return id;
}

// Whether the codec keeps every sample as it was, so that its bit depth means something.
[[nodiscard]] bool lossless(const std::string& codec) {
    return codec == "pcm" || codec == "flac" || codec == "alac" || codec == "wavpack" || codec == "truehd";
}

// The Video element of a track entry: the size, and from Colour the bit depth and the transfer.
// Each number is loaded with its eight bytes at most before it is read.
[[nodiscard]] Task<void> video_read(Walk& body, Stream& made, Gathered& out) {
    while (true) {
        std::optional<Element> one = co_await next(body, out);

        if (!one) break;

        co_await one->body.need(8);

        if (one->id == pixel_width) {
            if (const std::optional<std::uint64_t> value = unsigned_of(one->body); value && *value > 0 && *value < 0x10000)
                made.width = static_cast<std::uint32_t>(*value);
        } else if (one->id == pixel_height) {
            if (const std::optional<std::uint64_t> value = unsigned_of(one->body); value && *value > 0 && *value < 0x10000)
                made.height = static_cast<std::uint32_t>(*value);
        } else if (one->id == colour) {
            while (true) {
                std::optional<Element> inner = co_await next(one->body, out);

                if (!inner) break;

                co_await inner->body.need(8);

                const std::optional<std::uint64_t> value =
                    inner->id == bits_per_channel || inner->id == transfer_characteristics ? unsigned_of(inner->body) : std::nullopt;

                if (!value) continue;

                if (inner->id == bits_per_channel && *value > 0 && *value <= 32) made.depth = static_cast<std::uint16_t>(*value);

                if (inner->id == transfer_characteristics) made.transfer = transfer_of(*value);
            }
        }
    }
}

// The Audio element of a track entry: the sample rate, the channels (one when not given) and the
// bit depth. Each number is loaded with its eight bytes at most before it is read.
[[nodiscard]] Task<void> audio_read(Walk& body, Stream& made, Gathered& out) {
    made.channels = std::uint16_t{1};

    while (true) {
        std::optional<Element> one = co_await next(body, out);

        if (!one) break;

        co_await one->body.need(8);

        if (one->id == sampling_frequency) {
            if (const std::optional<double> value = float_of(one->body); value && *value >= 1 && *value < 1e7)
                made.rate = static_cast<std::uint32_t>(std::llround(*value));
        } else if (one->id == channels) {
            if (const std::optional<std::uint64_t> value = unsigned_of(one->body); value && *value > 0 && *value < 0x10000)
                made.channels = static_cast<std::uint16_t>(*value);
        } else if (one->id == bit_depth) {
            if (const std::optional<std::uint64_t> value = unsigned_of(one->body); value && *value > 0 && *value <= 64)
                made.depth = static_cast<std::uint16_t>(*value);
        }
    }
}

// One track entry as a stream: video (type 1), audio (2) or subtitles (17); other types (buttons,
// logos, complex tracks) are none. The language is English when the entry does not say it, as the
// specification sets; a BCP 47 language takes the place of the older code. Each child is loaded up
// to `most_said` bytes, the most any of the simple ones reads.
[[nodiscard]] Task<std::optional<Stream>> track_read(Walk& body, Gathered& out) {
    Stream made;
    std::optional<std::uint64_t> kind;
    std::string older = "eng";
    std::string newer;
    Bytes data;

    while (true) {
        std::optional<Element> one = co_await next(body, out);

        if (!one) break;

        co_await one->body.need(most_said);

        switch (one->id) {
            case track_type: kind = unsigned_of(one->body); break;
            case codec_id: made.codec = codec_named(text_of(one->body)); break;
            case codec_private: data = one->body.bytes(static_cast<std::size_t>(std::min<std::uint64_t>(one->body.left(), 32))); break;
            case language: older = text_of(one->body); break;
            case language_bcp47: newer = text_of(one->body); break;
            case track_name: made.name = text_of(one->body); break;
            case video: co_await video_read(one->body, made, out); break;
            case audio: co_await audio_read(one->body, made, out); break;
            default: break;
        }
    }

    if (kind == 1u) made.kind = Stream::Kind::video;
    else if (kind == 2u) made.kind = Stream::Kind::audio;
    else if (kind == 17u) made.kind = Stream::Kind::subtitle;
    else co_return std::nullopt;

    if (made.kind != Stream::Kind::audio) made.channels.reset();

    made.language = newer.empty() ? older : newer;

    // Windows codecs carry their header in CodecPrivate: BITMAPINFOHEADER with the four characters
    // at byte 16, WAVEFORMATEX with the format tag at byte 0.
    if (made.codec == "V_MS/VFW/FOURCC" && data.size() >= 20)
        made.codec = vfw_codec(std::string(reinterpret_cast<const char*>(data.data()) + 16, 4));
    else if (made.codec == "A_MS/ACM" && data.size() >= 2)
        made.codec = acm_codec(static_cast<std::uint16_t>(std::to_integer<unsigned>(data[0]) | (std::to_integer<unsigned>(data[1]) << 8)));

    if (made.kind == Stream::Kind::video && !made.depth) made.depth = depth_in(made.codec, data);

    // A lossy sound has no depth of its own; the one a muxer writes is the decoder's sample format.
    if (made.kind == Stream::Kind::audio && !lossless(made.codec)) made.depth.reset();

    co_return made;
}

// The Tracks element: every track entry as a stream, and whether there is a video and a sound
// track.
[[nodiscard]] Task<void> tracks_read(Walk& body, Seen& seen, Gathered& out) {
    seen.tracks = true;

    while (true) {
        std::optional<Element> entry = co_await next(body, out);

        if (!entry) break;

        if (entry->id != track_entry) continue;

        std::optional<Stream> one = co_await track_read(entry->body, out);

        if (!one) continue;

        if (one->kind == Stream::Kind::video) seen.video = true;

        if (one->kind == Stream::Kind::audio) seen.audio = true;

        out.stream(std::move(*one));
    }
}

}

// Parses a Matroska or WebM file of `size` bytes: the document type from the EBML header, then the
// Info and the Tracks of the first Segment, up to its first Cluster. Sets the type (video with a
// video track or without tracks, audio with sound tracks only) and the duration. A file without a
// Segment, or whose header is not EBML, is marked damaged.
Task<void> matroska(Reader& from, std::uint64_t size, Gathered& out) {
    Walk file{from, size, 0, size};

    const std::optional<Element> head = co_await next(file, out);

    if (!head || head->id != ebml_head) {
        out.damaged();

        co_return;
    }

    std::string document = "matroska";
    Walk header = head->body;

    while (true) {
        std::optional<Element> one = co_await next(header, out);

        if (!one) break;

        if (one->id != doc_type) continue;

        co_await one->body.need(16);
        document = one->body.chars(static_cast<std::size_t>(std::min<std::uint64_t>(one->body.left(), 16)));
    }

    const bool web = document == "webm";

    Seen seen;
    bool found = false;

    while (true) {
        std::optional<Element> top = co_await next(file, out);

        if (!top) break;

        if (top->id != segment) continue;

        found = true;

        while (true) {
            std::optional<Element> child = co_await next(top->body, out);

            if (!child) break;

            if (child->id == cluster) break;

            if (child->id == info) co_await info_read(child->body, seen, out);
            else if (child->id == tracks) co_await tracks_read(child->body, seen, out);

            if (seen.info && seen.tracks) break;
        }

        break;
    }

    const bool sound = seen.audio && !seen.video;

    out.into.type = web ? (sound ? "audio/webm" : "video/webm") : (sound ? "audio/x-matroska" : "video/x-matroska");

    if (!found) {
        out.damaged();

        co_return;
    }

    if (!seen.duration) co_return;

    const double lasting = *seen.duration * static_cast<double>(seen.scale) / 1e6;

    if (std::isfinite(lasting) && lasting > 0 && lasting < longest)
        out.into.properties.duration = std::chrono::milliseconds{static_cast<std::int64_t>(std::llround(lasting))};
}

}
