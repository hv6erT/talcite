#pragma once

#include <cstdint>
#include <optional>

namespace talcite {

struct Color {
    bool transparent = true;
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;

    [[nodiscard]] static std::optional<Color> indexed(int which);

    friend constexpr bool operator==(Color, Color) = default;
};

struct Style {
    Color foreground;
    Color background;
    bool bold = false;
    bool faint = false;
    bool italic = false;
    bool under = false;
    bool reverse = false;
    bool crossed = false;

    [[nodiscard]] static Style merge(Style base, Style over);

    friend constexpr bool operator==(Style, Style) = default;
};

}
