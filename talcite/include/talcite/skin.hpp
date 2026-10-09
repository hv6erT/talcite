#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "talcite/capabilities.hpp"
#include "talcite/line.hpp"
#include "talcite/shape.hpp"
#include "talcite/view.hpp"

namespace talcite::ui {

struct Shades {
    Shades() = default;

    Shades(Style same);

    [[nodiscard]] std::optional<Style> fitted(Colors level) const;

    std::optional<Style> mono;
    std::optional<Style> ansi;
    std::optional<Style> palette;
    std::optional<Style> rgb;
};

struct Signs {
    Signs() = default;

    Signs(char32_t same);

    [[nodiscard]] std::optional<char32_t> fitted(bool unicode) const;

    std::optional<char32_t> ascii;
    std::optional<char32_t> unicode;
};

using Styles = std::map<std::string, Shades, std::less<>>;

using Glyphs = std::map<std::string, Signs, std::less<>>;

class Skin {
public:
    Skin();

    explicit Skin(Styles styles, Glyphs glyphs = {});

    [[nodiscard]] static const Skin& plain();

    [[nodiscard]] Style style(std::string_view key) const;

    [[nodiscard]] char32_t glyph(std::string_view key) const;

    [[nodiscard]] Lines lines(Border border) const;

    [[nodiscard]] Colors colors() const { return colors_; }

    void colors(Colors given);

    [[nodiscard]] bool unicode() const { return unicode_; }

    void unicode(bool given);

private:
    void framed();

    Styles styles_;
    Glyphs glyphs_;
    Colors colors_ = Colors::mono;
    bool unicode_ = false;
    std::array<Lines, 4> lines_;
};

}
