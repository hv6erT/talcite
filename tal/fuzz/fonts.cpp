#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>

#include "tal/drawing.hpp"
#include "tal/fonts.hpp"
#include "tal/trouble.hpp"

// One input of libFuzzer as a font file, read by our reader of TrueType: a font broken where it
// starts may only be Trouble (malformed, unsupported, full); one that reads lays out a line of
// Latin, Polish, Greek and mathematics, and every point of its outline must be finite. Nothing may
// crash, hang, read out of bounds or leak.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    using namespace tal::drawing;

    try {
        const Font font = Font::read(tal::Bytes{reinterpret_cast<const std::byte*>(data), reinterpret_cast<const std::byte*>(data) + size});
        const Path path = font.outlined("Aa AV \xC4\x85\xC3\xA9\xC5\xBB \xCE\xA9 \xE2\x88\xAB 0123", 50, Point{1, 2}, Align::center, Baseline::middle);

        for (const Path::Piece& piece : path.pieces())
            for (const Point& one : piece.points)
                if (!std::isfinite(one.x) || !std::isfinite(one.y)) std::terminate();

        (void)font.measured("AV", 12);
        (void)font.covers(U'A');
        (void)font.name();
    } catch (const tal::Trouble&) {
    }

    return 0;
}
