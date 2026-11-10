#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "tal/style.hpp"

namespace tal::palette {

inline constexpr std::array<Color, 16> basics{{{0, 0, 0},
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

inline constexpr std::array<std::uint8_t, 6> steps{0, 95, 135, 175, 215, 255};

[[nodiscard]] constexpr Color indexed(int which) {
    if (which < 16) return basics[static_cast<std::size_t>(which)];

    if (which >= 232) {
        const auto grey = static_cast<std::uint8_t>(8 + 10 * (which - 232));

        return Color{grey, grey, grey};
    }

    const int cube = which - 16;

    return Color{steps[static_cast<std::size_t>(cube / 36)], steps[static_cast<std::size_t>((cube / 6) % 6)],
                 steps[static_cast<std::size_t>(cube % 6)]};
}

}
