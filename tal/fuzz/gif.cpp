#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>

#include "tal/pixels.hpp"
#include "tal/trouble.hpp"

#include "gifting.hpp"

// One input of libFuzzer as an image: its format and size from the header, then, for a GIF whose
// canvas is small enough to keep the runs fast, every frame of our decoder. Each frame must be the
// canvas of the header, hold four bytes a pixel and last at least a hundredth of a second; the
// first is shrunk to a few pixels on the way. A damaged file may only end in `Trouble`; nothing may
// crash, hang, read out of bounds or leak.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> given{reinterpret_cast<const std::byte*>(data), size};
    const std::optional<tal::media::Size> header = tal::media::size_of(given);

    if (tal::media::format_of(given) != tal::media::Format::gif) return 0;

    if (header && std::uint64_t{header->width} * header->height > (1U << 16)) return 0;

    std::size_t frames = 0;

    try {
        tal::media::gifting::gifted(given, [&](std::span<const std::byte> rgba, tal::media::Size seen, std::chrono::milliseconds after) {
            if (!header || !(seen == *header) || rgba.size() != std::size_t{seen.width} * seen.height * 4) std::terminate();

            if (after < std::chrono::milliseconds{10}) std::terminate();

            if (frames++ == 0) {
                const tal::media::Pixels one{seen, {tal::media::Still{tal::Bytes(rgba.begin(), rgba.end()), after}}};

                if (!(one.scaled({3, 2}).size() == tal::media::Size{3, 2})) std::terminate();
            }

            return frames < 64;
        });
    } catch (const tal::Trouble&) {
        return 0;
    }

    return 0;
}
