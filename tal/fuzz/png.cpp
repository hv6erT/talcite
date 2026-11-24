#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>

#include "tal/pixels.hpp"
#include "tal/trouble.hpp"

#include "pnging.hpp"

// One input of libFuzzer twice. As a PNG, whose canvas is small enough to keep the runs fast,
// through our decoder: the frame must be the canvas of the header and hold four bytes a pixel, and
// a damaged file may only end in `Trouble`. As pixels, the bytes after the first, which gives the
// width, taken as RGBA: encoded by the library's encoder (`media::encoded`) and decoded again,
// they must come back whole.
// Nothing may crash, hang, read out of bounds or leak.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::byte> given{reinterpret_cast<const std::byte*>(data), size};
    const std::optional<tal::media::Size> header = tal::media::size_of(given);

    if (tal::media::format_of(given) == tal::media::Format::png && header && std::uint64_t{header->width} * header->height <= (1U << 16)) {
        try {
            tal::media::pnging::pnged(given, [&](std::span<const std::byte> rgba, tal::media::Size seen, std::chrono::milliseconds) {
                if (!(seen == *header) || rgba.size() != std::size_t{seen.width} * seen.height * 4) std::terminate();

                return true;
            });
        } catch (const tal::Trouble&) {
        }
    }

    if (size < 5) return 0;

    const std::uint32_t width = 1 + data[0] % 16;
    const std::uint32_t height = static_cast<std::uint32_t>((size - 1) / 4 / width);

    if (height == 0) return 0;

    const std::span<const std::byte> rgba = given.subspan(1, std::size_t{width} * height * 4);
    const tal::Bytes encoded = tal::media::encoded(tal::media::Pixels{{width, height}, {tal::media::Still{tal::Bytes(rgba.begin(), rgba.end()), {}}}});
    bool back = false;

    tal::media::pnging::pnged(encoded, [&](std::span<const std::byte> again, tal::media::Size seen, std::chrono::milliseconds) {
        if (!(seen == tal::media::Size{width, height}) || !std::equal(again.begin(), again.end(), rgba.begin(), rgba.end())) std::terminate();

        back = true;

        return true;
    });

    if (!back) std::terminate();

    return 0;
}
