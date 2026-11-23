#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <vector>

#include "tal/text.hpp"
#include "tal/trouble.hpp"

#include "rendering.hpp"

// What a renderer's process says, from libFuzzer: the input, cut into pieces of the sizes its first
// byte gives, read as frames and every frame read as each of the protocol's answers (a hello, a
// trouble, the pages of a document, a picture of the size its first bytes claim) and as each of
// what a process tells unasked (a change, a refusal, a view lost). Nothing may crash, hang or read
// out of bounds; a frame breaks the protocol only as `Trouble` malformed; a frame is never longer
// than the most allowed; a picture read has exactly the size asked and the bytes of it, a page has
// an area, a title or an address is at most 64 KiB of UTF-8 and a refusal has a reason the
// protocol names; what is read is written back the same.
namespace {

// Reads `frame` as every answer of the protocol, checking what comes out.
void read_as_answers(const tal::rendering::Frame& frame) {
    const std::span<const std::byte> payload{frame.payload};

    if (frame.payload.size() + tal::rendering::head > tal::rendering::widest) std::terminate();

    try {
        (void)tal::rendering::greeting(payload);
    } catch (const tal::Trouble& why) {
        if (why.kind() != tal::Kind::malformed) std::terminate();
    }

    try {
        (void)tal::rendering::failure(payload);
    } catch (const tal::Trouble& why) {
        if (why.kind() != tal::Kind::malformed) std::terminate();
    }

    try {
        for (const tal::docs::Page& one : tal::rendering::paged(payload))
            if (!(one.width > 0) || !(one.height > 0)) std::terminate();
    } catch (const tal::Trouble& why) {
        if (why.kind() != tal::Kind::malformed) std::terminate();
    }

    try {
        const tal::rendering::Changed made = tal::rendering::changing(payload);

        if (made.title.size() > tal::rendering::widest_said || made.address.size() > tal::rendering::widest_said) std::terminate();

        if (!tal::text::valid(made.title) || !tal::text::valid(made.address)) std::terminate();

        if (tal::rendering::changing(tal::rendering::changed_with(made)).title != made.title) std::terminate();
    } catch (const tal::Trouble& why) {
        if (why.kind() != tal::Kind::malformed) std::terminate();
    }

    try {
        const tal::rendering::Refused made = tal::rendering::refusing(payload);

        if (made.reason > tal::rendering::last_refusal || made.address.size() > tal::rendering::widest_said) std::terminate();

        if (tal::rendering::refusing(tal::rendering::refused_with(made)).address != made.address) std::terminate();
    } catch (const tal::Trouble& why) {
        if (why.kind() != tal::Kind::malformed) std::terminate();
    }

    try {
        (void)tal::rendering::losing(payload);
    } catch (const tal::Trouble& why) {
        if (why.kind() != tal::Kind::malformed) std::terminate();
    }

    try {
        tal::rendering::Reading sizes{payload};
        const tal::media::Size asked{sizes.u32() % 64 + 1, sizes.u32() % 64 + 1};
        tal::rendering::Reading from{payload};
        const tal::media::Pixels made = tal::rendering::pictured(from, asked);

        if (!(made.size() == asked) || made.stills().front().rgba.size() != std::size_t{asked.width} * asked.height * 4) std::terminate();
    } catch (const tal::Trouble& why) {
        if (why.kind() != tal::Kind::malformed && why.kind() != tal::Kind::other) std::terminate();
    }
}

}

// One input of libFuzzer (see above).
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> given{reinterpret_cast<const std::byte*>(data), size};
    const std::size_t piece = size > 0 ? std::to_integer<std::size_t>(given[0]) % 32 + 1 : 1;
    tal::rendering::Unframing frames{std::size_t{1} << 16};

    try {
        for (std::size_t at = 0; at < size; at += piece) {
            frames.add(given.subspan(at, std::min(piece, size - at)));

            for (std::optional<tal::rendering::Frame> one = frames.next(); one; one = frames.next()) read_as_answers(*one);
        }
    } catch (const tal::Trouble& why) {
        if (why.kind() != tal::Kind::malformed) std::terminate();
    }

    return 0;
}
