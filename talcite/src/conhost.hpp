#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/area.hpp"

namespace talcite::conhost {

struct Cell {
    char32_t glyph = U' ';
    std::uint8_t ink = 7;
    std::uint8_t paper = 0;
    bool under = false;
    bool trailing = false;

    friend bool operator==(const Cell&, const Cell&) = default;
};

struct Tracking {
    bool clicks = false;
    bool drags = false;
    bool moves = false;
    bool sgr = false;

    friend bool operator==(const Tracking&, const Tracking&) = default;
};

struct Rgb {
    int red = 0;
    int green = 0;
    int blue = 0;

    friend bool operator==(const Rgb&, const Rgb&) = default;
};

using Palette = std::array<Rgb, 16>;

[[nodiscard]] Palette classic();

[[nodiscard]] std::uint8_t nearest(const Palette& among, int red, int green, int blue);

class Screen {
public:
    Screen(ui::Size size, std::uint8_t ink, std::uint8_t paper, Palette palette = classic());

    void fed(std::string_view bytes);

    void resize(ui::Size size);

    void pixels(ui::Size cell) { pixels_ = cell; }

    [[nodiscard]] ui::Size size() const { return size_; }

    [[nodiscard]] const Cell& at(int x, int y) const;

    [[nodiscard]] ui::Spot cursor() const { return cursor_; }

    [[nodiscard]] bool shown() const { return shown_; }

    [[nodiscard]] int caret() const { return caret_; }

    [[nodiscard]] bool apart() const { return apart_; }

    [[nodiscard]] const Tracking& tracking() const { return tracking_; }

    [[nodiscard]] std::vector<int> dirty();

    [[nodiscard]] std::string answers();

    [[nodiscard]] std::optional<std::string> title();

private:
    enum class Mode : std::uint8_t { ground, escape, csi, text, swallowed, ending };

    struct Pen {
        std::uint8_t ink = 7;
        std::uint8_t paper = 0;
        bool bold = false;
        bool under = false;
        bool reverse = false;
    };

    void taken(unsigned char byte);
    void escaped(unsigned char byte);
    void gathered(unsigned char byte);
    void stringed(unsigned char byte);
    void put(char32_t one);
    void control(unsigned char one);
    void dispatched(char last);
    void moded(bool on);
    void asked_mode();
    void styled();
    void colored(std::size_t& at, bool foreground);
    void told();
    void lined();
    void erased_line(int how);
    void erased_screen(int how);
    void cleared(int x, int y, int much);
    void written(int x, int y, const Cell& one);
    void touched(int y);
    void switched(bool on, bool cursor);
    [[nodiscard]] Cell blank() const;
    [[nodiscard]] Cell inked(char32_t glyph) const;
    [[nodiscard]] int parameter(std::size_t which, int fallback) const;
    [[nodiscard]] Cell& cell(int x, int y);

    ui::Size size_;
    std::vector<Cell> cells_;
    std::vector<Cell> kept_;
    ui::Spot kept_cursor_{};
    std::vector<bool> dirty_;
    ui::Spot cursor_{};
    ui::Spot last_{-1, -1};
    ui::Spot after_{-1, -1};
    bool pending_ = false;
    bool shown_ = true;
    int caret_ = 0;
    bool wrapping_ = true;
    bool apart_ = false;
    Tracking tracking_;
    Palette palette_;
    std::optional<ui::Size> pixels_;

    std::uint8_t ink_;
    std::uint8_t paper_;
    Pen pen_;

    Mode mode_ = Mode::ground;
    bool lead_osc_ = false;
    std::string string_;
    std::vector<int> parameters_;
    char marker_ = 0;
    char intermediate_ = 0;
    std::size_t length_ = 0;
    char32_t code_ = 0;
    std::size_t left_ = 0;

    std::string answers_;
    std::optional<std::string> title_;
};

struct Pressed {
    std::uint16_t key = 0;
    char16_t unit = 0;
    std::uint32_t state = 0;
    int repeat = 1;
};

[[nodiscard]] std::string typed(const Pressed& one, char16_t& pending);

struct Pointed {
    ui::Spot at{};
    std::uint32_t buttons = 0;
    std::uint32_t flags = 0;
    std::uint32_t state = 0;
};

class Pointing {
public:
    [[nodiscard]] std::string pointed(const Pointed& one, const Tracking& wanted);

private:
    std::uint32_t held_ = 0;
};

}
