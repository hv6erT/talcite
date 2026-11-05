#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/area.hpp"
#include "talcite/style.hpp"

namespace talcite {

enum class Shown : std::uint8_t { own, shade, none, foreground };

struct Cell {
    char32_t letter = U' ';
    Style style;
    Shown shown = Shown::own;
    std::uint32_t picture = 0;
    std::uint32_t link = 0;

    friend constexpr bool operator==(Cell, Cell) = default;
};

struct Hyperlink {
    std::uint32_t mark = 0;
    std::string address;
};

[[nodiscard]] Hyperlink hyperlink(std::string address);

class Pixels;

struct Picture {
    std::uint32_t mark = 0;
    ui::Size size;
    std::shared_ptr<const std::string> sixel;
    std::shared_ptr<const Pixels> pixels;
    std::size_t still = 0;
};

[[nodiscard]] ui::Size measured(std::string_view what);

class Snapshot {
public:
    Snapshot() = default;

    [[nodiscard]] ui::Size size() const { return size_; }

    [[nodiscard]] Cell at(ui::Spot where) const;

    [[nodiscard]] const std::vector<Picture>& pictures() const { return pictures_; }

    [[nodiscard]] const std::vector<Hyperlink>& links() const { return links_; }

protected:
    [[nodiscard]] bool holds(ui::Spot where) const;

    ui::Size size_;
    std::vector<Cell> cells_;
    std::vector<Picture> pictures_;
    std::vector<Hyperlink> links_;
};

}
