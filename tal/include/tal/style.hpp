#pragma once

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tal/geometry.hpp"

namespace tal {

struct Color {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;

    friend bool operator==(const Color&, const Color&) = default;
};

enum class Colors { mono, ansi, palette, rgb };

enum class Border { none, thin, twin, thick };

struct Look {
    std::optional<Color> ink;
    std::optional<Color> paper;
    bool bold = false;
    bool faint = false;
    bool italic = false;
    bool under = false;
    bool reverse = false;
    bool crossed = false;

    friend bool operator==(const Look&, const Look&) = default;
};

struct Style {
    Look look;
    Border border = Border::none;
    Sides padding;
    Sides margin;

    friend bool operator==(const Style&, const Style&) = default;
};

enum class State { focus, disabled, select, current, mark, drag, hover };

class States {
public:
    States() = default;

    States(std::initializer_list<State> some);

    [[nodiscard]] bool has(State one) const;

    void set(State one, bool on);

    friend bool operator==(const States&, const States&) = default;

private:
    std::uint32_t held_ = 0;
};

struct Selection {
    std::span<const std::vector<std::string>> classes;
    States states;
    std::string_view part;
};

struct Lines {
    std::string_view upper_left;
    std::string_view upper_right;
    std::string_view lower_left;
    std::string_view lower_right;
    std::string_view across;
    std::string_view upright;
    std::string_view left_joint;
    std::string_view right_joint;
};

class StyleSheet {
public:
    StyleSheet();

    [[nodiscard]] static StyleSheet parsed(std::string_view json);

    [[nodiscard]] Style style(const Selection& who, Colors level) const;

    [[nodiscard]] std::string_view glyph(std::string_view name, bool unicode) const;

    [[nodiscard]] Lines lines(Border kind, bool unicode) const;

private:
    struct Held;

    explicit StyleSheet(std::shared_ptr<const Held> held);

    std::shared_ptr<const Held> held_;
};

}
