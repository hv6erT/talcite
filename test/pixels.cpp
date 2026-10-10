#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "talcite/pixels.hpp"

using namespace talcite;

namespace {

// The trials write pixels as plain numbers, because that is how they read; the conversion to
// `Bytes` lives in one place, so that `std::byte{}` does not spread over a hundred lines of
// expectations.
Bytes lump(const std::vector<std::uint8_t>& bytes) {
    Bytes made;

    made.reserve(bytes.size());

    for (const std::uint8_t one : bytes) made.push_back(std::byte{one});

    return made;
}

// An image made of the given pixels, four bytes for each.
Pixels made(ui::Size size, const std::vector<std::uint8_t>& bytes,
            std::chrono::milliseconds after = std::chrono::milliseconds{0}) {
    return Pixels{size, {Still{lump(bytes), after}}};
}

// Channel `which` (0 red, 1 green, 2 blue, 3 alpha) of the pixel at (`x`, `y`) in frame `still`.
std::uint8_t band(const Pixels& one, int x, int y, std::size_t which, std::size_t still = 0) {
    const std::size_t at = (static_cast<std::size_t>(y) *
                                static_cast<std::size_t>(one.size().wide) +
                            static_cast<std::size_t>(x)) *
                           4;

    return std::to_integer<std::uint8_t>(one.stills()[still].bytes[at + which]);
}

}

// --- Construction -------------------------------------------------------------------------------

TRIAL("Pixels: an empty one carries nothing") {
    const Pixels one;

    ought(!one.held());
    same(one.size().wide, 0);
    ought(one.stills().empty());
}

TRIAL("Pixels: a frame that does not match the size is an error") {
    bool threw = false;

    try {
        (void)made({2, 2}, std::vector<std::uint8_t>(8, 0));
    } catch (const std::logic_error&) {
        threw = true;
    }

    ought(threw);
}

TRIAL("Pixels: frames without a size are an error") {
    bool threw = false;

    try {
        (void)made({0, 0}, std::vector<std::uint8_t>{});
    } catch (const std::logic_error&) {
        threw = true;
    }

    ought(threw);
}

TRIAL("Pixels: several frames keep their delays") {
    const Pixels one{{1, 1},
                     {Still{lump({1, 2, 3, 255}), std::chrono::milliseconds{40}},
                      Still{lump({4, 5, 6, 255}), std::chrono::milliseconds{80}}}};

    same(one.stills().size(), static_cast<std::size_t>(2));
    same(one.stills()[0].after.count(), static_cast<long long>(40));
    same(one.stills()[1].after.count(), static_cast<long long>(80));
}

// --- Linear light -------------------------------------------------------------------------------
//
// A byte in the file is not brightness; it carries a value along the gamma curve. The average
// brightness of black and white is 0.5, and the byte for 0.5 is 188, not 127. Averaging the raw
// bytes darkens every edge, which is what most programs do.

TRIAL("Pixels: black and white give 188 at half, not 127") {
    const Pixels one = made({2, 1}, {0, 0, 0, 255, 255, 255, 255, 255});

    const Pixels half = one.scaled({1, 1});

    same(static_cast<int>(band(half, 0, 0, 0)), 188);
    same(static_cast<int>(band(half, 0, 0, 3)), 255);
}

TRIAL("Pixels: a uniform color survives downscaling unchanged") {
    const Pixels red = made({4, 4}, [] {
        std::vector<std::uint8_t> these(64, 0);

        for (std::size_t at = 0; at < these.size(); at += 4) {
            these[at] = 200;
            these[at + 3] = 255;
        }

        return these;
    }());

    const Pixels small = red.scaled({2, 2});

    same(static_cast<int>(band(small, 0, 0, 0)), 200);
    same(static_cast<int>(band(small, 1, 1, 0)), 200);
    same(static_cast<int>(band(small, 1, 1, 1)), 0);
}

// --- Alpha --------------------------------------------------------------------------------------
//
// A transparent pixel carries some color in the file, usually black. Counted on a par with the
// visible ones it makes a dark outline around every cut-out shape, a bug that most fast scalers
// have and that nobody can name afterwards. So the color is weighted by alpha and divided by it.

TRIAL("Pixels: a transparent neighbor does not darken the color") {
    const Pixels one = made({2, 1}, {255, 0, 0, 255, 0, 0, 0, 0});

    const Pixels half = one.scaled({1, 1});

    // Red stays full; naive averaging would give 128 here.
    same(static_cast<int>(band(half, 0, 0, 0)), 255);
    same(static_cast<int>(band(half, 0, 0, 1)), 0);

    // Transparency averages normally: half from the visible pixel and half from the invisible one.
    same(static_cast<int>(band(half, 0, 0, 3)), 128);
}

TRIAL("Pixels: pure transparency stays transparent") {
    const Pixels one = made({2, 1}, {255, 0, 0, 0, 0, 255, 0, 0});

    const Pixels half = one.scaled({1, 1});

    same(static_cast<int>(band(half, 0, 0, 3)), 0);
    same(static_cast<int>(band(half, 0, 0, 0)), 0);
}

// --- Scaling ------------------------------------------------------------------------------------

// The box filter takes all the source pixels, so a thin bright line does not vanish just because
// it missed the sample. Nearest neighbor would lose it in three cases out of four.
TRIAL("Pixels: a thin line does not vanish when downscaling") {
    const Pixels one =
        made({4, 1}, {0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255});

    const Pixels small = one.scaled({1, 1});

    ought(band(small, 0, 0, 0) > 0);
}

TRIAL("Pixels: downscaling merges squares evenly") {
    const Pixels one = made({2, 2}, {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0,
                                     255});

    const Pixels small = one.scaled({1, 1});

    same(small.size().wide, 1);
    same(small.size().high, 1);
    same(static_cast<int>(band(small, 0, 0, 0)), 255);
}

// Upscaling uses nearest neighbor, that is sharp squares, and that is the right answer for an icon
// and for pixel art, not an oversight. Blurred pixel art looks worse.
TRIAL("Pixels: upscaling multiplies pixels instead of blurring") {
    const Pixels one = made({2, 1}, {10, 0, 0, 255, 200, 0, 0, 255});

    const Pixels big = one.scaled({4, 1});

    same(big.size().wide, 4);
    same(static_cast<int>(band(big, 0, 0, 0)), 10);
    same(static_cast<int>(band(big, 1, 0, 0)), 10);
    same(static_cast<int>(band(big, 2, 0, 0)), 200);
    same(static_cast<int>(band(big, 3, 0, 0)), 200);
}

TRIAL("Pixels: the same size gives back the same image") {
    const Pixels one = made({2, 1}, {1, 2, 3, 255, 4, 5, 6, 255});

    const Pixels same_size = one.scaled({2, 1});

    same(static_cast<int>(band(same_size, 1, 0, 1)), 5);
    same(same_size.size().wide, 2);
}

TRIAL("Pixels: a zero target carries nothing") {
    const Pixels one = made({2, 1}, {1, 2, 3, 255, 4, 5, 6, 255});

    ought(!one.scaled({0, 4}).held());
    ought(!one.scaled({4, 0}).held());
    ought(!Pixels{}.scaled({4, 4}).held());
}

TRIAL("Pixels: delays survive scaling") {
    const Pixels one{{2, 1},
                     {Still{lump({0, 0, 0, 255, 255, 255, 255, 255}), std::chrono::milliseconds{40}},
                      Still{lump({255, 255, 255, 255, 0, 0, 0, 255}), std::chrono::milliseconds{90}}}};

    const Pixels half = one.scaled({1, 1});

    same(half.stills().size(), static_cast<std::size_t>(2));
    same(half.stills()[0].after.count(), static_cast<long long>(40));
    same(half.stills()[1].after.count(), static_cast<long long>(90));
    same(static_cast<int>(band(half, 0, 0, 0, 1)), 188);
}

// --- Fitting -------------------------------------------------------------------------------------
//
// A cell is not a square. Some terminals have 10 by 20 pixels, others 9 by 21; without its size a
// circle comes out as an egg. Zero means "the terminal did not say", and then 1:2 is assumed.

TRIAL("fitting: a square in a 1:2 cell takes half as many rows") {
    const ui::Size made_size = fitting({100, 100}, {40, 40}, {10, 20});

    same(made_size.wide, 40);
    same(made_size.high, 20);
}

TRIAL("fitting: an unknown cell means 1:2") {
    same(fitting({100, 100}, {40, 40}, {0, 0}).high, 20);
    same(fitting({100, 100}, {40, 40}, {0, 0}).wide, 40);
}

TRIAL("fitting: a square cell does not squeeze the height") {
    const ui::Size one = fitting({100, 100}, {40, 40}, {10, 10});

    same(one.wide, 40);
    same(one.high, 40);
}

TRIAL("fitting: a wide image takes the full width") {
    const ui::Size one = fitting({100, 50}, {40, 20}, {10, 20});

    same(one.wide, 40);
    same(one.high, 10);
}

TRIAL("fitting: a tall image is limited by the height of the space") {
    const ui::Size one = fitting({50, 100}, {40, 20}, {10, 20});

    same(one.wide, 20);
    same(one.high, 20);
}

// An image taking zero cells is an image nobody sees, and the caller asked for a preview.
TRIAL("fitting: always at least one cell") {
    same(fitting({1000, 1}, {40, 20}, {10, 20}).high, 1);
    same(fitting({1, 1000}, {40, 20}, {10, 20}).wide, 1);
}

TRIAL("fitting: anything zero gives nothing") {
    same(fitting({0, 10}, {40, 20}, {10, 20}).wide, 0);
    same(fitting({10, 0}, {40, 20}, {10, 20}).wide, 0);
    same(fitting({10, 10}, {0, 20}, {10, 20}).wide, 0);
    same(fitting({10, 10}, {40, 0}, {10, 20}).wide, 0);
}
