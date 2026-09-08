#pragma once

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "talcite/compatibilities.hpp"
#include "talcite/line.hpp"
#include "talcite/style.hpp"
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

    template <typename Value>
    [[nodiscard]] Value get(std::string_view key) const {
        return taken<Value>(key);
    }

    template <typename Value>
    [[nodiscard]] Value plain(std::string_view key) const {
        return spared<Value>(key);
    }

    [[nodiscard]] Colors colors() const { return colors_; }

    void colors(Colors given);

    [[nodiscard]] bool unicode() const { return unicode_; }

    void unicode(bool given);

private:
    template <typename Value>
    [[nodiscard]] Value taken(std::string_view key) const;

    template <typename Value>
    [[nodiscard]] Value spared(std::string_view key) const;

    void framed();

    Styles styles_;
    Glyphs glyphs_;
    Colors colors_ = Colors::mono;
    bool unicode_ = false;
    std::array<Lines, 4> lines_;
};

}
