#pragma once

#include <string_view>

#include "talcite/area.hpp"
#include "talcite/style.hpp"
#include "talcite/view.hpp"

namespace talcite {

class Canvas {
public:
    explicit Canvas(View& sheet);

    [[nodiscard]] ui::Size size() const { return size_; }

    [[nodiscard]] ui::Area seen() const;

    [[nodiscard]] Canvas within(ui::Area where) const;

    [[nodiscard]] Canvas clipped(ui::Area room) const;

    void put(ui::Spot at, char32_t what, const Style& how);

    void put(ui::Spot at, std::string_view what, const Style& how);

    void fill(const ui::Area& where, const Style& how);

    void shade(const ui::Area& where, const Style& how);

    void foreground(ui::Spot at, char32_t what, const Style& how);

    void picture(const ui::Area& where, Picture what);

    void link(const ui::Area& where, Hyperlink what);

    [[nodiscard]] Cell at(ui::Spot where) const;

private:
    [[nodiscard]] bool holds(ui::Spot where) const;

    [[nodiscard]] ui::Area cut(const ui::Area& where) const;

    View* sheet_;
    ui::Spot origin_;
    ui::Size size_;
    ui::Area clip_;
};

}
