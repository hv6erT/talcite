#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "tal/drawing.hpp"
#include "tal/fonts.hpp"
#include "tal/text.hpp"

namespace tal::plotting {

enum class Spacing { linear, logarithmic };

class Scale {
public:
    Scale() = default;

    Scale(double low, double high, Spacing spacing = Spacing::linear);

    [[nodiscard]] static Scale fitted(std::span<const double> values, Spacing spacing = Spacing::linear);

    [[nodiscard]] double low() const { return low_; }

    [[nodiscard]] double high() const { return high_; }

    [[nodiscard]] Spacing spacing() const { return spacing_; }

    [[nodiscard]] double placed(double value, double from, double to) const;

    [[nodiscard]] double valued(double pixel, double from, double to) const;

    [[nodiscard]] Scale zoomed(double about, double factor) const;

    [[nodiscard]] Scale shifted(double pixels, double from, double to) const;

    [[nodiscard]] std::vector<double> ticks(std::size_t most) const;

    [[nodiscard]] std::vector<double> minor(std::size_t most) const;

    friend bool operator==(const Scale&, const Scale&) = default;

private:
    double low_ = 0;
    double high_ = 1;
    Spacing spacing_ = Spacing::linear;
};

struct Labels {
    std::vector<std::string> said;
    int power = 0;
};

[[nodiscard]] Labels labelled(const Scale& scale, std::span<const double> ticks, const text::Language& language);

enum class Mark { none, circle, square, cross, triangle };

struct Series {
    std::string name;
    std::vector<drawing::Point> points;
    std::optional<drawing::Color> color;
    double width = 1.5;
    bool line = true;
    Mark mark = Mark::none;
    double size = 5;
};

struct Axis {
    std::string title;
    std::optional<Scale> scale;
    Spacing spacing = Spacing::linear;
    bool grid = false;
};

enum class Corner { automatic, upper_left, upper_right, lower_left, lower_right, none };

struct Theme {
    drawing::Color ink{0.85F, 0.86F, 0.88F, 1};
    std::optional<drawing::Color> paper;
    drawing::Color grid{0.85F, 0.86F, 0.88F, 0.18F};
    drawing::Color backing{0.05F, 0.06F, 0.08F, 0.85F};
    std::vector<drawing::Color> palette;
};

[[nodiscard]] Theme dark();

[[nodiscard]] Theme light();

enum class Readout { automatic, across, nearest };

struct Nearest {
    std::size_t series = 0;
    std::size_t point = 0;
    double distance = 0;

    friend bool operator==(const Nearest&, const Nearest&) = default;
};

class Plot;

struct Layout {
    drawing::Rect data;
    Scale x;
    Scale y;
    std::vector<double> x_ticks;
    std::vector<double> y_ticks;
    Corner legend = Corner::none;

    [[nodiscard]] std::optional<drawing::Point> at(drawing::Point pixel) const;

    [[nodiscard]] std::optional<Nearest> nearest(const Plot& plot, drawing::Point pixel, double within) const;
};

class Plot {
public:
    std::string title;
    Axis x;
    Axis y;
    std::vector<Series> series;
    Corner legend = Corner::automatic;
    Theme theme = dark();
    double letters = 12;
    std::optional<text::Language> language;
    std::optional<drawing::Point> cursor;
    Readout readout = Readout::automatic;
    bool cursor_line = true;
    int readout_digits = 1;

    [[nodiscard]] Layout laid(const drawing::Font& font, drawing::Rect area) const;

    Layout drawn(drawing::Pen& pen, const drawing::Font& font, drawing::Rect area) const;
};

class Trace {
public:
    explicit Trace(double window, std::size_t most = 100000);

    void add(double time, double value);

    void clear();

    [[nodiscard]] std::vector<drawing::Point> points() const;

    [[nodiscard]] std::optional<Scale> span() const;

private:
    mutable std::mutex guard_;
    double window_;
    std::size_t most_;
    std::deque<drawing::Point> kept_;
};

}
