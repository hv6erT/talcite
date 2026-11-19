#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>

#include "tal/pixels.hpp"
#include "tal/trouble.hpp"

#include "jpeging.hpp"

// One input of libFuzzer as a JPEG, whose canvas is small enough to keep the runs fast, through
// our decoder: the frame must be the canvas of the header, hold four bytes a pixel and be opaque.
// A damaged file may only end in `Trouble`; nothing may crash, hang, read out of bounds or leak.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> given{reinterpret_cast<const std::byte*>(data), size};
    const std::optional<tal::media::Size> header = tal::media::size_of(given);

    if (tal::media::format_of(given) != tal::media::Format::jpeg) return 0;

    if (header && std::uint64_t{header->width} * header->height > (1U << 16)) return 0;

    try {
        tal::media::jpeging::jpeged(given, [&](std::span<const std::byte> rgba, tal::media::Size seen, std::chrono::milliseconds) {
            if (rgba.size() != std::size_t{seen.width} * seen.height * 4) std::terminate();

            if (header && !(seen == *header)) std::terminate();

            for (std::size_t at = 3; at < rgba.size(); at += 4)
                if (rgba[at] != std::byte{255}) std::terminate();

            return true;
        });
    } catch (const tal::Trouble&) {
    }

    return 0;
}
