#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "tal/pixels.hpp"

namespace tal::drawing {

inline constexpr std::size_t most_pieces = std::size_t{1} << 22;

inline constexpr std::size_t most_dashes = std::size_t{1} << 16;

struct Point {
    double x = 0;
    double y = 0;

    friend bool operator==(const Point&, const Point&) = default;
};

struct Rect {
    Point at;
    double wide = 0;
    double high = 0;

    friend bool operator==(const Rect&, const Rect&) = default;
};

struct Matrix {
    double a = 1;
    double b = 0;
    double c = 0;
    double d = 1;
    double e = 0;
    double f = 0;

    [[nodiscard]] static Matrix moved(double x, double y);

    [[nodiscard]] static Matrix scaled(double x, double y);

    [[nodiscard]] static Matrix turned(double radians);

    [[nodiscard]] Point applied(Point one) const;

    [[nodiscard]] Matrix then(const Matrix& next) const;

    [[nodiscard]] std::optional<Matrix> inverted() const;

    friend bool operator==(const Matrix&, const Matrix&) = default;
};

struct Color {
    float r = 0;
    float g = 0;
    float b = 0;
    float a = 1;

    friend bool operator==(const Color&, const Color&) = default;
};

enum class Rule : std::uint8_t { nonzero, evenodd };

enum class Join : std::uint8_t { miter, round, bevel };

enum class Cap : std::uint8_t { butt, round, square };

enum class Blending : std::uint8_t { linear, srgb };

class Gradient {
public:
    enum class Shape : std::uint8_t { linear, radial };

    [[nodiscard]] static Gradient linear(Point from, Point to);

    [[nodiscard]] static Gradient radial(Point inner, double from, Point outer, double to);

    void stop(double at, Color color);

    [[nodiscard]] Shape shape() const { return shape_; }

    [[nodiscard]] Point first() const { return first_; }

    [[nodiscard]] Point second() const { return second_; }

    [[nodiscard]] double inner() const { return inner_; }

    [[nodiscard]] double outer() const { return outer_; }

    [[nodiscard]] const std::vector<std::pair<double, Color>>& stops() const { return stops_; }

private:
    Gradient(Shape shape, Point first, Point second, double inner, double outer);

    Shape shape_;
    Point first_;
    Point second_;
    double inner_;
    double outer_;
    std::vector<std::pair<double, Color>> stops_;
};

class Pattern {
public:
    explicit Pattern(media::Pixels image, bool across = true, bool down = true, std::size_t still = 0);

    void place(const Matrix& where);

    [[nodiscard]] const media::Pixels& image() const { return *image_; }

    [[nodiscard]] std::size_t still() const { return still_; }

    [[nodiscard]] bool across() const { return across_; }

    [[nodiscard]] bool down() const { return down_; }

    [[nodiscard]] const Matrix& placed() const { return placed_; }

private:
    std::shared_ptr<const media::Pixels> image_;
    std::size_t still_;
    bool across_;
    bool down_;
    Matrix placed_;
};

using Paint = std::variant<Color, Gradient, Pattern>;

class Path {
public:
    enum class Kind : std::uint8_t { move, line, quad, cubic, arc, close };

    struct Piece {
        Kind kind = Kind::move;
        std::array<Point, 3> points{};
        double rx = 0;
        double ry = 0;
        double turn = 0;
        double from = 0;
        double sweep = 0;
    };

    void move(Point to);

    void line(Point to);

    void quad(Point control, Point to);

    void cubic(Point first, Point second, Point to);

    void arc(Point center, double radius, double from, double to, bool backwards = false);

    void ellipse(Point center, double rx, double ry, double turn, double from, double to, bool backwards = false);

    void rectangle(Rect box);

    void close();

    [[nodiscard]] bool empty() const { return pieces_.empty(); }

    [[nodiscard]] const std::vector<Piece>& pieces() const { return pieces_; }

private:
    void started(Point at);

    void added(const Piece& one);

    std::vector<Piece> pieces_;
    std::optional<Point> start_;
    std::optional<Point> last_;
};

struct Stroke {
    double width = 1;
    Join join = Join::miter;
    Cap cap = Cap::butt;
    double miter = 10;
    std::vector<double> dashes;
    double offset = 0;
};

class Raster {
public:
    explicit Raster(media::Size size, Blending blending = Blending::linear);

    [[nodiscard]] media::Size size() const { return size_; }

    [[nodiscard]] Blending blending() const { return blending_; }

    void clear(Color with = Color{0, 0, 0, 0});

    [[nodiscard]] media::Pixels pixels() const;

private:
    friend class Pen;

    media::Size size_;
    Blending blending_;
    std::vector<std::uint16_t> samples_;
};

class Pen;

class Recording {
public:
    explicit Recording(media::Size size, Blending blending = Blending::linear);

    ~Recording();

    Recording(Recording&& other) noexcept;
    Recording& operator=(Recording&& other) noexcept;
    Recording(const Recording&) = delete;
    Recording& operator=(const Recording&) = delete;

    [[nodiscard]] media::Size size() const { return size_; }

    [[nodiscard]] Blending blending() const { return blending_; }

    [[nodiscard]] std::size_t steps() const;

    void played(Pen& onto) const;

private:
    friend class Pen;
    friend std::string svg(const Recording& recording);

    struct Step;
    struct Steps;

    media::Size size_;
    Blending blending_;
    std::unique_ptr<Steps> steps_;
};

[[nodiscard]] std::string svg(const Recording& recording);

class Pen {
public:
    explicit Pen(Raster& onto);

    explicit Pen(Recording& onto);

    Pen(const Pen&) = delete;
    Pen& operator=(const Pen&) = delete;

    ~Pen();

    void save();

    void restore();

    void transform(const Matrix& more);

    void place(const Matrix& whole);

    [[nodiscard]] const Matrix& placed() const;

    void opacity(float alpha);

    void smoothing(bool on);

    void fill(const Path& path, const Paint& paint, Rule rule = Rule::nonzero);

    void stroke(const Path& path, const Paint& paint, const Stroke& style = {});

    void clip(const Path& path, Rule rule = Rule::nonzero);

    void draw(const media::Pixels& image, Rect into, std::size_t still = 0);

    void draw(const media::Pixels& image, Rect from, Rect into, std::size_t still = 0);

    [[nodiscard]] bool inside(const Path& path, Point one, Rule rule = Rule::nonzero) const;

    [[nodiscard]] bool on(const Path& path, Point one, const Stroke& style = {}) const;

private:
    struct State;
    struct Work;
    struct Lines;

    void painted(const Lines& lines, Rule rule, const Paint& paint);

    void recorded(Recording::Step step);

    Raster* onto_ = nullptr;
    Recording* recording_ = nullptr;
    std::vector<State> saved_;
    std::unique_ptr<State> state_;
    std::unique_ptr<Work> work_;
};

}
