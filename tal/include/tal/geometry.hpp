#pragma once

namespace tal {

enum class Axis { vertical, horizontal };

struct Spot {
    int x = 0;
    int y = 0;

    friend bool operator==(const Spot&, const Spot&) = default;
};

struct Size {
    int wide = 0;
    int high = 0;

    friend bool operator==(const Size&, const Size&) = default;
};

struct Sides {
    int top = 0;
    int right = 0;
    int bottom = 0;
    int left = 0;

    friend bool operator==(const Sides&, const Sides&) = default;
};

struct Area {
    Spot at;
    Size size;

    [[nodiscard]] bool contains(Spot one) const;

    [[nodiscard]] Area inner(Sides less) const;

    [[nodiscard]] Area common(Area other) const;

    friend bool operator==(const Area&, const Area&) = default;
};

struct Limits {
    Size least;
    Size most;

    friend bool operator==(const Limits&, const Limits&) = default;
};

}
