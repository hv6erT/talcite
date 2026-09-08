#pragma once

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "talcite/area.hpp"

namespace talcite {

struct Color {
    bool transparent = true;
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;

    friend constexpr bool operator==(Color, Color) = default;
};

struct Style {
    Color ink;
    Color paper;
    bool bold = false;
    bool faint = false;
    bool italic = false;
    bool under = false;
    bool reverse = false;
    bool crossed = false;

    [[nodiscard]] static Style merge(Style base, Style over);

    friend constexpr bool operator==(Style, Style) = default;
};

enum class Shown : std::uint8_t { own, shade, none };

struct Cell {
    char32_t letter = U' ';
    Style style;
    Shown shown = Shown::own;

    friend constexpr bool operator==(Cell, Cell) = default;
};

[[nodiscard]] ui::Size measured(std::string_view what);

class View {
public:
    View() = default;

    explicit View(ui::Size given);

    [[nodiscard]] ui::Size size() const { return size_; }

    void letter(ui::Spot at, char32_t what, const Style& how);

    void string(ui::Spot at, std::string_view what, const Style& how);

    void fill(const ui::Area& where, const Style& how);

    void shade(const ui::Area& where, const Style& how);

    void bare(const ui::Area& where);

    void within(const View& one, ui::Spot at);

    [[nodiscard]] Cell at(ui::Spot where) const;

private:
    [[nodiscard]] bool holds(ui::Spot where) const;

    Cell& cell(ui::Spot where);

    void broken(ui::Spot where);

    ui::Size size_;
    std::vector<Cell> cells_;
};

}
