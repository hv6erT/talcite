#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "tagging.hpp"

// AVI, ported from talcite-adapters' tags: a RIFF file of the form "AVI ", a tree of chunks (four
// characters, a little-endian 32-bit size, the content padded to an even length). The headers
// stand at the start, in LIST hdrl: avih (microseconds per frame and the number of frames), a LIST
// strl per stream with its strh (the kind of stream, a time scale and rate, the length in those
// units), and for files over a gigabyte (OpenDML) LIST odml with dmlh, the number of frames in the
// whole file. The avih count covers only the first RIFF piece of such a file, so dmlh goes first,
// then the length of the first video stream, and avih last. Only the headers are read; the frames
// in LIST movi are not touched. A chunk that fits in the window is loaded together with its header.
//
// Streams: each strl with its strf, the format of the stream: for a picture BITMAPINFOHEADER (the
// size and the codec's four characters), for a sound WAVEFORMATEX (the format tag, channels,
// sample rate, average bytes a second and bits a sample); strn names the stream. AVI keeps no
// language.
namespace tal::meta::tagging {

namespace {

// Longer than this is not a running time but garbage: a hundred thousand hours, in milliseconds.
constexpr double longest = 3.6e11;

struct Chunk {
    std::string id;
    std::string form;
    Walk body;
};

// The next chunk from the parent's cursor, or none when the parent has ended. A LIST or RIFF
// chunk carries its form (four characters) before the content. The padding byte after an odd size
// is skipped. A body that fits in the window is loaded right after the header, so that the part
// holds it in memory.
[[nodiscard]] Task<std::optional<Chunk>> next(Walk& parent, Gathered& out) {
    if (parent.left() < 8) co_return std::nullopt;

    co_await parent.need(4 + 4 + 4);

    std::string id = parent.chars(4);
    const std::uint32_t size = parent.le32();

    if (size > parent.left()) out.damaged();

    if (size <= windowed()) co_await parent.need(size);

    Walk body = parent.part(size);

    if ((size & 1u) != 0 && parent.left() > 0) parent.skip(1);

    std::string form;

    if (id == "LIST" || id == "RIFF") form = body.chars(4);

    co_return Chunk{std::move(id), std::move(form), std::move(body)};
}

struct Heard {
    std::uint32_t frame = 0;
    std::uint32_t frames = 0;
    std::optional<std::uint32_t> whole;
    std::optional<std::chrono::milliseconds> stream;
};

// A stream header: for the first video stream, its length in units of scale/rate seconds.
void stream_read(Walk& body, Heard& heard) {
    if (heard.stream || body.chars(4) != "vids") return;

    body.skip(16);

    const std::uint64_t scale = body.le32();
    const std::uint64_t rate = body.le32();

    body.skip(4);

    const std::uint64_t length = body.le32();

    if (body.spoiled() || scale == 0 || rate == 0 || length == 0) return;

    // In floating point: the length and the scale are 32-bit each, and their product with a
    // thousand does not fit in 64 bits.
    const double lasting = static_cast<double>(length) * static_cast<double>(scale) * 1000.0 / static_cast<double>(rate);

    if (lasting < longest) heard.stream = std::chrono::milliseconds{static_cast<std::int64_t>(lasting)};
}

// A stream list: the kind from strh (whose length also goes to `stream_read`), the format from
// strf, the name from strn. None for a stream that is neither a picture, a sound nor text.
[[nodiscard]] Task<std::optional<Stream>> listed_stream(Walk& body, Heard& heard, Gathered& out) {
    Stream made;
    std::string kind;

    while (true) {
        std::optional<Chunk> inner = co_await next(body, out);

        if (!inner) break;

        Walk& one = inner->body;

        if (inner->id == "strh") {
            co_await one.need(4 + 16 + 4 + 4 + 4 + 4);

            Walk again = one;

            kind = again.chars(4);
            stream_read(one, heard);
        } else if (inner->id == "strf" && kind == "vids") {
            co_await one.need(4 + 4 + 4 + 4 + 4);
            one.skip(4);

            const std::uint32_t wide = one.le32();
            const std::uint32_t high = one.le32();

            one.skip(4);

            const std::string fourcc = one.chars(4);

            if (one.spoiled()) continue;

            made.codec = vfw_codec(fourcc);

            // A negative height (rows from the top down) is the same size.
            const std::uint32_t tall = (high & 0x80000000u) != 0 ? ~high + 1 : high;

            if (wide > 0 && wide < 0x10000) made.width = wide;

            if (tall > 0 && tall < 0x10000) made.height = tall;
        } else if (inner->id == "strf" && kind == "auds") {
            co_await one.need(2 + 2 + 4 + 4 + 2 + 2);

            const std::uint16_t tag = one.le16();
            const std::uint16_t channels = one.le16();
            const std::uint32_t rate = one.le32();
            const std::uint32_t bytes = one.le32();

            one.skip(2);

            const std::uint16_t bits = one.le16();

            if (one.spoiled()) continue;

            made.codec = acm_codec(tag);

            if (channels > 0) made.channels = channels;

            if (rate > 0) made.rate = rate;

            if (bytes > 0 && bytes < 0x10000000u) made.bitrate = bytes * 8;

            if (made.codec == "pcm" && bits > 0) made.depth = bits;
        } else if (inner->id == "strn") {
            // The name up to its zero; a broken sequence becomes the replacement character.
            co_await one.need(256);
            made.name = utf8(one.bytes(static_cast<std::size_t>(std::min<std::uint64_t>(one.left(), 256))));
        }
    }

    if (kind == "vids") made.kind = Stream::Kind::video;
    else if (kind == "auds") made.kind = Stream::Kind::audio;
    else if (kind == "txts") made.kind = Stream::Kind::subtitle;
    else co_return std::nullopt;

    co_return made;
}

// The headers list: avih, the stream lists and the OpenDML list.
[[nodiscard]] Task<void> headers_read(Walk& body, Heard& heard, Gathered& out) {
    while (true) {
        std::optional<Chunk> one = co_await next(body, out);

        if (!one) break;

        if (one->id == "avih") {
            co_await one->body.need(4 + 12 + 4);
            heard.frame = one->body.le32();
            one->body.skip(12);
            heard.frames = one->body.le32();
        } else if (one->id == "LIST" && one->form == "strl") {
            std::optional<Stream> stream = co_await listed_stream(one->body, heard, out);

            if (stream) out.into.streams.push_back(std::move(*stream));
        } else if (one->id == "LIST" && one->form == "odml") {
            while (true) {
                std::optional<Chunk> inner = co_await next(one->body, out);

                if (!inner) break;

                if (inner->id == "dmlh") {
                    co_await inner->body.need(4);

                    const std::uint32_t frames = inner->body.le32();

                    if (!inner->body.spoiled() && frames > 0) heard.whole = frames;
                }
            }
        }
    }
}

}

// Parses an AVI file of `size` bytes: the headers list of the first RIFF piece. Sets the type and
// the duration; a file without the headers list is marked damaged.
Task<void> avi(Reader& from, std::uint64_t size, Gathered& out) {
    Walk file{from, size, 0, size};

    out.into.type = "video/x-msvideo";

    const std::optional<Chunk> riff = co_await next(file, out);

    if (!riff || riff->id != "RIFF" || riff->form != "AVI ") {
        out.damaged();

        co_return;
    }

    Walk inside = riff->body;
    Heard heard;
    bool found = false;

    while (true) {
        std::optional<Chunk> one = co_await next(inside, out);

        if (!one) break;

        if (one->id != "LIST" || one->form != "hdrl") continue;

        found = true;
        co_await headers_read(one->body, heard, out);

        break;
    }

    if (!found) {
        out.damaged();

        co_return;
    }

    std::optional<std::chrono::milliseconds> lasting = heard.stream;
    const std::uint64_t frame = heard.frame;

    if (heard.whole && frame > 0) lasting = std::chrono::milliseconds{static_cast<std::int64_t>(*heard.whole * frame / 1000)};
    else if (!lasting && heard.frames > 0 && frame > 0) lasting = std::chrono::milliseconds{static_cast<std::int64_t>(heard.frames * frame / 1000)};

    if (lasting && lasting->count() > 0) out.into.properties.duration = lasting;
}

}
