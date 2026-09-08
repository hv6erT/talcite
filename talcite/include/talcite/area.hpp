#pragma once

#include <limits>

namespace talcite::ui {

inline constexpr int plenty = std::numeric_limits<int>::max();

struct Size {
    int wide = 0;
    int high = 0;

    friend constexpr bool operator==(Size, Size) = default;
};

struct Spot {
    int x = 0;
    int y = 0;

    friend constexpr bool operator==(Spot, Spot) = default;
};

struct Area {
    Spot at;
    Size size;

    [[nodiscard]] static constexpr Area crossed(Area one, Area two) {
        const int left = one.at.x > two.at.x ? one.at.x : two.at.x;
        const int top = one.at.y > two.at.y ? one.at.y : two.at.y;

        const int first = one.at.x + one.size.wide;
        const int second = two.at.x + two.size.wide;
        const int right = first < second ? first : second;

        const int lower = one.at.y + one.size.high;
        const int upper = two.at.y + two.size.high;
        const int bottom = lower < upper ? lower : upper;

        return {{left, top}, {right > left ? right - left : 0, bottom > top ? bottom - top : 0}};
    }

    friend constexpr bool operator==(Area, Area) = default;
};

[[nodiscard]] constexpr Spot operator+(Spot one, Spot two) {
    return {one.x + two.x, one.y + two.y};
}

[[nodiscard]] constexpr Spot operator-(Spot one, Spot two) {
    return {one.x - two.x, one.y - two.y};
}

[[nodiscard]] constexpr Area operator+(Area one, Spot two) { return {one.at + two, one.size}; }

[[nodiscard]] constexpr Area operator-(Area one, Spot two) { return {one.at - two, one.size}; }

}
