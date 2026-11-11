#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tal/geometry.hpp"
#include "tal/style.hpp"
#include "tal/text.hpp"

namespace tal {

struct Cell {
    std::string cluster = " ";
    Look look;
    std::uint32_t picture = 0;
    std::uint32_t link = 0;

    friend bool operator==(const Cell&, const Cell&) = default;
};

class Canvas;

class Surface {
public:
    explicit Surface(Size size, text::Rendering how = {});

    [[nodiscard]] Size size() const;

    [[nodiscard]] const Cell& at(Spot one) const;

    [[nodiscard]] Canvas canvas(Area part);

    [[nodiscard]] std::span<const Area> damaged() const;

    void forget();

    [[nodiscard]] std::string_view address(std::uint32_t link) const;

private:
    friend class Canvas;

    [[nodiscard]] Cell& cell(Spot one);

    [[nodiscard]] bool inside(Spot one) const;

    void placed(Spot here, std::string_view cluster, int wide, const Look& look, Area clip, int& from, int& to);

    void blanked(Spot one, int& from, int& to);

    void relinked(Cell& one, std::uint32_t link);

    [[nodiscard]] std::uint32_t interned(std::string_view address);

    void released(std::uint32_t link);

    void spoiled(Area part);

    Size size_;
    text::Rendering rendering_;
    std::vector<Cell> cells_;
    std::vector<Area> damaged_;
    std::vector<std::string> addresses_;
    std::vector<std::size_t> holders_;
    std::vector<std::uint32_t> spare_;
    std::map<std::string, std::uint32_t, std::less<>> known_;
};

class Canvas {
public:
    [[nodiscard]] Size size() const;

    [[nodiscard]] Canvas within(Area part) const;

    int put(Spot at, std::string_view text, const Look& look);

    void fill(Area part, const Look& look);

    void frame(Area part, const Lines& lines, const Look& look);

    void picture(Area part, std::uint32_t id);

    void link(Area part, std::string_view address);

    void shade(Area part, const Look& look);

private:
    friend class Surface;

    Canvas(Surface& onto, Spot origin, Size size, Area clip);

    [[nodiscard]] Area cut(Area part) const;

    Surface* onto_;
    Spot origin_;
    Size size_;
    Area clip_;
};

}
