#include "tagging.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "tal/bytes.hpp"
#include "tal/crypto.hpp"

// FLAC and Vorbis comments, ported from talcite-adapters' tags. FLAC is the "fLaC" signature
// followed by metadata blocks: STREAMINFO (properties), VORBIS_COMMENT (fields), PICTURE
// (pictures); the rest does not matter for the description. Vorbis comments are shared by FLAC,
// Ogg Vorbis, Opus and FLAC in Ogg, so `ogg.cpp` calls `vorbis`, `streaminfo` and `picture_block`
// from here.
//
// A Vorbis comment key is case-insensitive, so it is stored in upper case: "Artist" and "ARTIST"
// are one field. The METADATA_BLOCK_PICTURE comment (a picture in Ogg, a PICTURE block in base64)
// and the older COVERART with COVERARTMIME are pictures, not fields.
namespace tal::meta::tagging {

namespace {

// How many base64 characters to decode when only describing a picture from a comment: the block
// header (type, description, dimensions, data length) fits with room to spare; the whole value is
// decoded only for the picture whose bytes are asked for.
constexpr std::size_t glimpsed = 64 * 1024;

// A picture embedded in a comment in base64: a PICTURE block or, for COVERART, bare picture bytes.
Task<void> encoded_picture(Walk& value, bool block, Gathered& out) {
    const std::uint64_t size = value.left();

    if (size > most_packet) co_return out.damaged();

    const std::size_t much = out.wanting() ? static_cast<std::size_t>(size)
                                           : static_cast<std::size_t>(std::min<std::uint64_t>(size, glimpsed));
    const Bytes taken = co_await value.take(much);
    std::string coded = text::of(taken);

    coded.resize(coded.size() / 4 * 4);

    std::optional<Bytes> raw = crypto::unbase64(coded);

    if (!raw) co_return out.damaged();

    if (block) {
        Held held{std::move(*raw)};
        Walk inner = co_await Walk::over(held);

        co_await picture_block(inner, out);

        co_return;
    }

    // COVERART: the picture size is the length after decoding the whole value, computed from the
    // base64 length without the padding.
    const std::size_t padded = coded.ends_with("==") ? 2 : coded.ends_with('=') ? 1 : 0;
    const std::size_t whole = static_cast<std::size_t>(size / 4 * 3) - (much == size ? padded : 0);

    Picture one{Role::front, image_type(*raw), {}, whole};

    if (out.pictured(std::move(one))) out.took(std::move(*raw));
}

}

// The FLAC STREAMINFO block (34 bytes), shared with FLAC in Ogg. The caller computes the bitrate,
// because only it knows where the metadata ends.
Task<void> streaminfo(Walk& block, Gathered& out) {
    co_await block.need(10 + 8);

    block.skip(10);

    const std::uint64_t packed = block.be64();

    if (block.spoiled()) co_return out.damaged();

    const auto rate = static_cast<std::uint32_t>(packed >> 44);
    const auto channels = static_cast<std::uint16_t>(((packed >> 41) & 7u) + 1);
    const auto depth = static_cast<std::uint16_t>(((packed >> 36) & 31u) + 1);
    const std::uint64_t samples = packed & 0xFFFFFFFFFull;

    if (rate == 0) co_return out.damaged();

    out.into.properties.rate = rate;
    out.into.properties.channels = channels;
    out.into.properties.depth = depth;

    if (samples > 0) out.into.properties.duration = std::chrono::milliseconds{static_cast<std::int64_t>(samples * 1000 / rate)};
}

// Vorbis comments (FLAC, Ogg Vorbis, Opus, FLAC in Ogg), starting at the vendor string length.
Task<void> vorbis(Walk& block, Gathered& out) {
    co_await block.need(4);

    const std::uint32_t vendor = block.le32();

    block.skip(vendor);

    co_await block.need(4);

    const std::uint32_t count = block.le32();

    for (std::uint32_t which = 0; which < count && !block.spoiled() && !out.done(); ++which) {
        co_await block.need(4);

        const std::uint32_t size = block.le32();

        if (size > block.left()) {
            out.damaged();

            break;
        }

        Walk comment = block.part(size);

        // The key runs up to the equals sign; no program writes a key longer than this piece. The
        // piece is read and the cursor set back past the equals sign.
        const std::uint64_t began = comment.at();
        const std::size_t seen = static_cast<std::size_t>(std::min<std::uint64_t>(size, 256));

        co_await comment.need(seen);

        const std::string head = text::of(comment.bytes(seen));
        const std::size_t equals = head.find('=');

        if (equals == std::string::npos || equals == 0) continue;

        const std::string key = upper(head.substr(0, equals));

        comment.seek(began + equals + 1);

        if (key == "METADATA_BLOCK_PICTURE") {
            co_await encoded_picture(comment, true, out);

            continue;
        }

        if (key == "COVERART") {
            co_await encoded_picture(comment, false, out);

            continue;
        }

        if (out.wanted || key == "COVERARTMIME" || comment.left() > most_text) continue;

        const Bytes value = co_await comment.take(static_cast<std::size_t>(comment.left()));

        if (std::string said = utf8(value); !said.empty()) out.field(Scheme::vorbis, key, std::move(said));
    }

    if (block.spoiled()) out.damaged();
}

// A FLAC picture block, also found in a METADATA_BLOCK_PICTURE comment after base64 decoding.
Task<void> picture_block(Walk& block, Gathered& out) {
    co_await block.need(4 + 4);

    const std::uint32_t kind = block.be32();
    const std::uint32_t typed = block.be32();

    if (typed > 256) co_return out.damaged();

    co_await block.need(typed + 4);

    std::string type = image_named(block.chars(typed));

    const std::uint32_t said = block.be32();

    if (said > most_text) co_return out.damaged();

    const Bytes told = co_await block.take(said);
    std::string described = utf8(told);

    block.skip(16);

    co_await block.need(4);

    const std::uint32_t data = block.be32();

    if (block.spoiled()) co_return out.damaged();

    if (!out.pictured({role_of(kind), std::move(type), std::move(described), data})) co_return;

    Bytes taken = co_await block.take(data);

    out.took(std::move(taken));

    if (block.spoiled()) out.damaged();
}

// Native FLAC, starting at "fLaC".
Task<void> flac(Walk& walk, Gathered& out) {
    walk.skip(4);

    out.into.type = "audio/flac";
    out.codec = "flac";

    bool last = false;

    while (!last && walk.left() >= 4 && !out.done()) {
        co_await walk.need(4);

        const std::uint8_t head = walk.u8();
        const std::uint32_t size = walk.be24();
        const std::uint8_t kind = head & 0x7F;

        last = (head & 0x80) != 0;

        if (kind == 127) co_return out.damaged();

        if (size > walk.left()) out.damaged();

        Walk block = walk.part(size);

        if (kind == 0) {
            co_await streaminfo(block, out);
        } else if (kind == 4) {
            if (size <= most_block) co_await vorbis(block, out);
        } else if (kind == 6) {
            co_await picture_block(block, out);
        }
    }

    if (walk.spoiled()) out.damaged();

    const std::optional<std::chrono::milliseconds> lasting = out.into.properties.duration;

    if (lasting && lasting->count() > 0 && walk.end() > walk.at())
        out.into.properties.bitrate =
            static_cast<std::uint32_t>((walk.end() - walk.at()) * 8 * 1000 / static_cast<std::uint64_t>(lasting->count()));
}

}
