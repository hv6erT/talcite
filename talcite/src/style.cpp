#include "talcite/style.hpp"

#include <array>
#include <cstdint>

// The 256-color palette, one for the library and the applications: sixteen basic colors with
// xterm's values, a 6x6x6 cube and a grey ramp. The terminal finds the nearest color in it when
// writing in 16 and 256 colors, and an application reads from it the colors a skin file gives by
// number. Keeping a single table is what guarantees that `blue` read from a file goes back to the
// terminal as `blue`.
namespace talcite {

namespace {

constexpr std::array<std::array<std::uint8_t, 3>, 16> basics{{{0, 0, 0},
                                                            {128, 0, 0},
                                                            {0, 128, 0},
                                                            {128, 128, 0},
                                                            {0, 0, 128},
                                                            {128, 0, 128},
                                                            {0, 128, 128},
                                                            {192, 192, 192},
                                                            {128, 128, 128},
                                                            {255, 0, 0},
                                                            {0, 255, 0},
                                                            {255, 255, 0},
                                                            {0, 0, 255},
                                                            {255, 0, 255},
                                                            {0, 255, 255},
                                                            {255, 255, 255}}};

constexpr std::array<std::uint8_t, 6> steps{0, 95, 135, 175, 215, 255};

// An opaque color with the given components.
Color solid(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    Color made;

    made.transparent = false;
    made.red = red;
    made.green = green;
    made.blue = blue;

    return made;
}

}

// The color for a palette number: 0-15 are the basic colors, 16-231 the cube, 232-255 the grey ramp
// in steps of ten from eight. Outside the palette, `std::nullopt`.
std::optional<Color> Color::indexed(int which) {
    if (which < 0 || which > 255) return std::nullopt;

    if (which < 16) {
        const auto& one = basics[static_cast<std::size_t>(which)];

        return solid(one[0], one[1], one[2]);
    }

    if (which >= 232) {
        const auto grey = static_cast<std::uint8_t>(8 + 10 * (which - 232));

        return solid(grey, grey, grey);
    }

    const int cube = which - 16;

    return solid(steps[static_cast<std::size_t>(cube / 36)], steps[static_cast<std::size_t>((cube / 6) % 6)],
                 steps[static_cast<std::size_t>(cube % 6)]);
}

}
