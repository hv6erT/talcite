#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
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

struct Placement {
    ui::Spot at;
    Picture picture;
};

[[nodiscard]] ui::Size measured(std::string_view what);

class View {
public:
    View() = default;

    explicit View(ui::Size given);

    [[nodiscard]] ui::Size size() const { return size_; }

    void put(ui::Spot at, char32_t what, const Style& how);

    void put(ui::Spot at, std::string_view what, const Style& how);

    void fill(const ui::Area& where, const Style& how);

    void shade(const ui::Area& where, const Style& how);

    void foreground(ui::Spot at, char32_t what, const Style& how);

    void clear(const ui::Area& where);

    void draw(const View& one, ui::Spot at);

    void picture(const ui::Area& where, Picture what);

    [[nodiscard]] const std::vector<Picture>& pictures() const { return pictures_; }

    void link(const ui::Area& where, Hyperlink what);

    [[nodiscard]] const std::vector<Hyperlink>& links() const { return links_; }

    [[nodiscard]] std::vector<Placement> bared(const ui::Area& room);

    [[nodiscard]] Cell at(ui::Spot where) const;

private:
    [[nodiscard]] bool holds(ui::Spot where) const;

    Cell& cell(ui::Spot where);

    void broken(ui::Spot where);

    ui::Size size_;
    std::vector<Cell> cells_;
    std::vector<Picture> pictures_;
    std::vector<Hyperlink> links_;
};

}
