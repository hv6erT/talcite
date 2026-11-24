#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tal/drawing.hpp"
#include "tal/pixels.hpp"
#include "tal/trouble.hpp"

#include "flattening.hpp"
#include "stroking.hpp"
#include "tal/testing.hpp"

// Drawing (E7a): coverage against the exact area of polygons on every pixel (rectangles of
// fractional edges, triangles, concave shapes, a vertex a trillion pixels away), circles against
// their area within the tolerance, whole-pixel moves and quarter turns, the two rules, strokes
// against the fills they equal (butt, square and round caps, miter, bevel and round joins, the
// miter limit, dashes and dots, a closed path's dash over its start, a pen flattened by the
// matrix), clips, gradients, patterns and images, the two spaces of blending, the state, the
// points inside and on, the rules of a path, the flattening within its tolerance, and every
// malformed number and every limit.

namespace {

using tal::Kind;
using tal::drawing::Blending;
using tal::drawing::Cap;
using tal::drawing::Color;
using tal::drawing::Gradient;
using tal::drawing::Join;
using tal::drawing::Matrix;
using tal::drawing::Path;
using tal::drawing::Pattern;
using tal::drawing::Pen;
using tal::drawing::Point;
using tal::drawing::Raster;
using tal::drawing::Rect;
using tal::drawing::Rule;
using tal::drawing::Stroke;

constexpr Color white{1, 1, 1, 1};
constexpr double byte = 1.0 / 255;

// The kind of the trouble `work` throws, none when it throws nothing.
std::optional<Kind> trouble_of(const std::function<void()>& work) {
    try {
        work();
    } catch (const tal::Trouble& why) {
        return why.kind();
    }

    return std::nullopt;
}

// The pixel (`x`, `y`) of `pixels` as four bytes.
std::array<int, 4> pixel(const tal::media::Pixels& pixels, int x, int y) {
    const tal::Bytes& rgba = pixels.stills().front().rgba;
    const std::size_t at = (static_cast<std::size_t>(y) * pixels.size().width + static_cast<std::size_t>(x)) * 4;

    return {std::to_integer<int>(rgba[at]), std::to_integer<int>(rgba[at + 1]), std::to_integer<int>(rgba[at + 2]), std::to_integer<int>(rgba[at + 3])};
}

// The alpha of the pixel (`x`, `y`) of `pixels`, 0 to 1.
double alpha(const tal::media::Pixels& pixels, int x, int y) { return pixel(pixels, x, y)[3] / 255.0; }

// The sum of the alphas of `pixels`, in pixels.
double covered(const tal::media::Pixels& pixels) {
    double sum = 0;

    for (int y = 0; y < static_cast<int>(pixels.size().height); ++y)
        for (int x = 0; x < static_cast<int>(pixels.size().width); ++x) sum += alpha(pixels, x, y);

    return sum;
}

// The area of the polygon `points` (any orientation).
double area(const std::vector<Point>& points) {
    double twice = 0;

    for (std::size_t at = 0; at < points.size(); ++at) {
        const Point& p = points[at];
        const Point& q = points[(at + 1) % points.size()];

        twice += p.x * q.y - q.x * p.y;
    }

    return std::abs(twice) / 2;
}

// `polygon` cut to the half-plane where `side` is not negative.
std::vector<Point> halved(const std::vector<Point>& polygon, const std::function<double(Point)>& side) {
    std::vector<Point> made;

    for (std::size_t at = 0; at < polygon.size(); ++at) {
        const Point p = polygon[at];
        const Point q = polygon[(at + 1) % polygon.size()];
        const double sp = side(p);
        const double sq = side(q);

        if (sp >= 0) made.push_back(p);

        if ((sp >= 0) != (sq >= 0)) {
            const double t = sp / (sp - sq);

            made.push_back(Point{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t});
        }
    }

    return made;
}

// The exact area of `polygon` within the pixel (`x`, `y`) (Sutherland and Hodgman against its
// four sides).
double exact(const std::vector<Point>& polygon, int x, int y) {
    std::vector<Point> cut = polygon;

    cut = halved(cut, [x](Point p) { return p.x - x; });
    cut = halved(cut, [x](Point p) { return x + 1 - p.x; });
    cut = halved(cut, [y](Point p) { return p.y - y; });
    cut = halved(cut, [y](Point p) { return y + 1 - p.y; });

    return cut.size() < 3 ? 0 : area(cut);
}

// The polygon `points` as a path.
Path polygon_of(const std::vector<Point>& points) {
    Path made;

    made.move(points.front());

    for (std::size_t at = 1; at < points.size(); ++at) made.line(points[at]);

    made.close();

    return made;
}

// Whether every pixel of `pixels` has the alpha of the exact area of `polygon` on it, within half
// a step of a byte.
bool exactly(const tal::media::Pixels& pixels, const std::vector<Point>& polygon) {
    for (int y = 0; y < static_cast<int>(pixels.size().height); ++y)
        for (int x = 0; x < static_cast<int>(pixels.size().width); ++x)
            if (std::abs(alpha(pixels, x, y) - exact(polygon, x, y)) > 0.5 * byte + 1e-6) return false;

    return true;
}

// `polygon` filled white on a transparent raster of `wide` by `high` in sRGB, through `matrix`.
tal::media::Pixels filled(int wide, int high, const std::vector<Point>& polygon, const Matrix& matrix = {}, Rule rule = Rule::nonzero) {
    Raster raster{tal::media::Size{static_cast<std::uint32_t>(wide), static_cast<std::uint32_t>(high)}, Blending::srgb};
    Pen pen{raster};

    pen.place(matrix);
    pen.fill(polygon_of(polygon), white, rule);

    return raster.pixels();
}

// `path` stroked white in `style` on a transparent raster of `wide` by `high` in sRGB.
tal::media::Pixels stroked(int wide, int high, const Path& path, const Stroke& style, const Matrix& matrix = {}) {
    Raster raster{tal::media::Size{static_cast<std::uint32_t>(wide), static_cast<std::uint32_t>(high)}, Blending::srgb};
    Pen pen{raster};

    pen.place(matrix);
    pen.stroke(path, white, style);

    return raster.pixels();
}

// The straight line from `from` to `to` as a path.
Path segment(Point from, Point to) {
    Path made;

    made.move(from);
    made.line(to);

    return made;
}

// Whether the alphas of `one` and `other` are alike within `bytes` steps of a byte (the colours of
// pixels all but clear being rounded far apart).
bool covers_alike(const tal::media::Pixels& one, const tal::media::Pixels& other, int bytes) {
    if (!(one.size() == other.size())) return false;

    const tal::Bytes& p = one.stills().front().rgba;
    const tal::Bytes& q = other.stills().front().rgba;

    for (std::size_t at = 3; at < p.size(); at += 4)
        if (std::abs(std::to_integer<int>(p[at]) - std::to_integer<int>(q[at])) > bytes) return false;

    return true;
}

// Whether `one` and `other` are alike within `bytes` steps of a byte on every channel.
bool alike(const tal::media::Pixels& one, const tal::media::Pixels& other, int bytes = 0) {
    if (!(one.size() == other.size())) return false;

    const tal::Bytes& p = one.stills().front().rgba;
    const tal::Bytes& q = other.stills().front().rgba;

    for (std::size_t at = 0; at < p.size(); ++at)
        if (std::abs(std::to_integer<int>(p[at]) - std::to_integer<int>(q[at])) > bytes) return false;

    return true;
}

// A small image of `wide` by `high` whose pixel (x, y) is (40 x + 10, 60 y + 20, 100, 255).
tal::media::Pixels ramp(std::uint32_t wide, std::uint32_t high, int alpha_of = 255) {
    tal::Bytes rgba(std::size_t{wide} * high * 4);

    for (std::uint32_t y = 0; y < high; ++y) {
        for (std::uint32_t x = 0; x < wide; ++x) {
            const std::size_t at = (std::size_t{y} * wide + x) * 4;

            rgba[at] = std::byte{static_cast<unsigned char>(40 * x + 10)};
            rgba[at + 1] = std::byte{static_cast<unsigned char>(60 * y + 20)};
            rgba[at + 2] = std::byte{100};
            rgba[at + 3] = std::byte{static_cast<unsigned char>(alpha_of)};
        }
    }

    std::vector<tal::media::Still> stills;

    stills.push_back(tal::media::Still{std::move(rgba), std::chrono::milliseconds{0}});

    return tal::media::Pixels{tal::media::Size{wide, high}, std::move(stills)};
}

// The sRGB value `one` (0 to 1) in linear light.
double light_of(double one) { return one <= 0.04045 ? one / 12.92 : std::pow((one + 0.055) / 1.055, 2.4); }

// The linear value `one` (0 to 1) in sRGB.
double srgb_of(double one) { return one <= 0.0031308 ? one * 12.92 : 1.055 * std::pow(one, 1 / 2.4) - 0.055; }

}

TRIAL("Drawing: a rectangle of fractional edges, triangles and a concave shape cover each pixel by its exact area") {
    const std::vector<Point> box{{1.25, 2.5}, {4.75, 2.5}, {4.75, 4.5}, {1.25, 4.5}};

    OUGHT(exactly(filled(8, 8, box), box));
    SAME(alpha(filled(8, 8, box), 0, 2), 0.0);
    SAME(pixel(filled(8, 8, box), 2, 3), (std::array<int, 4>{255, 255, 255, 255}));

    const std::vector<std::vector<Point>> shapes{
        {{0.3, 0.2}, {7.6, 1.1}, {3.2, 6.9}},
        {{6.9, 0.1}, {0.1, 3.3}, {5.5, 7.7}},
        {{1, 1}, {7, 1}, {7, 7}, {4, 3}, {1, 7}},
        {{0.5, 3.5}, {3.5, 0.5}, {6.5, 3.5}, {3.5, 6.5}},
        {{2.01, 0}, {2.02, 8}, {2.99, 8}},
    };

    for (const std::vector<Point>& shape : shapes) OUGHT(exactly(filled(8, 8, shape), shape));

    const std::vector<Point> far{{-1e12, 4}, {6.5, 0.5}, {6.5, 7.5}};

    OUGHT(exactly(filled(8, 8, far), far));

    const std::vector<Point> outside{{-5, -5}, {-1, -5}, {-1, 20}, {-5, 20}};

    SAME(covered(filled(8, 8, outside)), 0.0);

    const std::vector<Point> over{{-3, -3}, {12, -3}, {12, 12}, {-3, 12}};

    SAME(covered(filled(8, 8, over)), 64.0);
}

TRIAL("Drawing: a circle covers its area within the tolerance of flattening; whole-pixel moves are exact, quarter turns alike") {
    Raster raster{tal::media::Size{32, 32}, Blending::srgb};
    Pen pen{raster};
    Path circle;
    const double r = 10;

    circle.arc(Point{16, 16}, r, 0, 2 * std::numbers::pi);
    pen.fill(circle, white);

    const double sum = covered(raster.pixels());
    const double ideal = std::numbers::pi * r * r;

    OUGHT(sum <= ideal + 32 * 32 * 0.5 * byte);
    OUGHT(sum >= ideal - 2.0 / 3.0 * 0.1 * 2 * std::numbers::pi * r - 32 * 32 * 0.5 * byte);

    const std::vector<Point> shape{{1.3, 0.7}, {9.1, 2.2}, {6.4, 8.8}, {3.3, 5.1}};
    const tal::media::Pixels here = filled(20, 20, shape);
    const tal::media::Pixels moved = filled(20, 20, shape, Matrix::moved(5, 7));

    for (int y = 0; y < 13; ++y)
        for (int x = 0; x < 15; ++x) SAME(pixel(moved, x + 5, y + 7), pixel(here, x, y));

    const Matrix quarter{0, 1, -1, 0, 20, 0};
    const tal::media::Pixels turned = filled(20, 20, shape, quarter);

    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 20; ++x) OUGHT(std::abs(pixel(turned, 19 - y, x)[3] - pixel(here, x, y)[3]) <= 1);

    const Matrix by_angle = Matrix::turned(std::numbers::pi / 2);

    OUGHT(std::abs(by_angle.a) < 1e-15 && std::abs(by_angle.b - 1) < 1e-15 && std::abs(by_angle.c + 1) < 1e-15 && std::abs(by_angle.d) < 1e-15);
    SAME(Matrix::moved(1, 2).then(Matrix::scaled(3, 4)).applied(Point{1, 1}), (Point{6, 12}));
    SAME(Matrix::scaled(3, 4).then(Matrix::moved(1, 2)).applied(Point{1, 1}), (Point{4, 6}));

    const Matrix some{2, 0.5, -1, 3, 4, -7};
    const Point there = some.inverted()->applied(some.applied(Point{3, -2}));

    OUGHT(std::abs(there.x - 3) < 1e-12 && std::abs(there.y + 2) < 1e-12);
    OUGHT(!Matrix::scaled(1, 0).inverted());
}

TRIAL("Drawing: nonzero fills a star's middle and both rings of one turning; even-odd leaves them out") {
    std::vector<Point> star;

    for (int one = 0; one < 5; ++one) {
        const double angle = -std::numbers::pi / 2 + one * 4 * std::numbers::pi / 5;

        star.push_back(Point{16 + 15 * std::cos(angle), 16 + 15 * std::sin(angle)});
    }

    SAME(alpha(filled(32, 32, star, {}, Rule::nonzero), 16, 16), 1.0);
    SAME(alpha(filled(32, 32, star, {}, Rule::evenodd), 16, 16), 0.0);
    SAME(alpha(filled(32, 32, star, {}, Rule::evenodd), 16, 5), 1.0);

    const auto rings = [](bool against, Rule rule) {
        Raster raster{tal::media::Size{32, 32}, Blending::srgb};
        Pen pen{raster};
        Path two;

        two.arc(Point{16, 16}, 14, 0, 2 * std::numbers::pi);
        two.close();
        two.arc(Point{16, 16}, 6, against ? 2 * std::numbers::pi : 0, against ? 0 : 2 * std::numbers::pi, against);
        two.close();
        pen.fill(two, white, rule);

        return raster.pixels();
    };

    SAME(alpha(rings(false, Rule::nonzero), 16, 16), 1.0);
    SAME(alpha(rings(false, Rule::evenodd), 16, 16), 0.0);
    SAME(alpha(rings(true, Rule::nonzero), 16, 16), 0.0);
    SAME(alpha(rings(true, Rule::nonzero), 16, 5), 1.0);
}

TRIAL("Drawing: shapes of one path overlapping within a pixel cover their union by nonzero and what one of them alone covers by even-odd; a thick ring's inner side as near as its outer") {
    const auto two = [](bool opposite) {
        Path made;

        made.rectangle(Rect{Point{2.3, 1}, 4.2, 2});

        if (opposite) {
            made.move(Point{2.6, 1});
            made.line(Point{2.6, 3});
            made.line(Point{7.2, 3});
            made.line(Point{7.2, 1});
            made.close();
        } else {
            made.rectangle(Rect{Point{2.6, 1}, 4.6, 2});
        }

        return made;
    };
    const auto drawn = [](const Path& path, Rule rule) {
        Raster raster{tal::media::Size{8, 4}, Blending::srgb};
        Pen pen{raster};

        pen.fill(path, white, rule);

        return raster.pixels();
    };
    const auto near = [](double one, double other) { return std::abs(one - other) <= 0.01; };

    OUGHT(near(alpha(drawn(two(false), Rule::nonzero), 2, 1), 0.7));
    OUGHT(near(alpha(drawn(two(false), Rule::nonzero), 6, 1), 1.0));
    OUGHT(near(alpha(drawn(two(false), Rule::nonzero), 7, 2), 0.2));
    OUGHT(near(alpha(drawn(two(false), Rule::evenodd), 2, 1), 0.3));
    OUGHT(near(alpha(drawn(two(false), Rule::evenodd), 6, 2), 0.5));
    OUGHT(near(alpha(drawn(two(true), Rule::nonzero), 2, 1), 0.3));
    OUGHT(near(alpha(drawn(two(true), Rule::nonzero), 6, 1), 0.5));

    Path three;

    three.rectangle(Rect{Point{0, 1}, 8, 2});
    three.rectangle(Rect{Point{2.3, 1}, 5.7, 2});
    three.rectangle(Rect{Point{2.6, 1}, 5.4, 2});
    OUGHT(near(alpha(drawn(three, Rule::evenodd), 2, 1), 0.7));
    OUGHT(near(alpha(drawn(three, Rule::nonzero), 2, 1), 1.0));

    for (const auto& [width, radius] : {std::pair{8.0, 10.0}, std::pair{12.0, 9.0}}) {
        Path ring;
        Stroke style;

        ring.arc(Point{24, 16}, radius, 0, 2 * std::numbers::pi);
        ring.close();
        style.width = width;

        const tal::media::Pixels pixels = stroked(48, 32, ring, style);
        double most = 0;

        for (int y = 0; y < 32; ++y) {
            for (int x = 0; x < 48; ++x) {
                int in = 0;

                for (int i = 0; i < 32; ++i) {
                    for (int j = 0; j < 32; ++j) {
                        const double from = std::hypot(x + (i + 0.5) / 32 - 24, y + (j + 0.5) / 32 - 16);

                        in += from >= radius - width / 2 && from <= radius + width / 2 ? 1 : 0;
                    }
                }

                most = std::max(most, std::abs(alpha(pixels, x, y) - in / 1024.0));
            }
        }

        OUGHT(most < 0.1);
    }
}

TRIAL("Drawing: a stroke is the fill it equals: butt and square caps, round caps by their area") {
    const Path line = segment(Point{2, 4}, Point{10, 4});
    Stroke style;

    style.width = 3;

    const std::vector<Point> butt{{2, 2.5}, {10, 2.5}, {10, 5.5}, {2, 5.5}};

    OUGHT(alike(stroked(16, 8, line, style), filled(16, 8, butt)));

    style.cap = Cap::square;

    const std::vector<Point> square{{0.5, 2.5}, {11.5, 2.5}, {11.5, 5.5}, {0.5, 5.5}};

    OUGHT(alike(stroked(16, 8, line, style), filled(16, 8, square)));

    style.cap = Cap::round;

    const double round = covered(stroked(16, 8, line, style));
    const double ideal = 8 * 3 + std::numbers::pi * 1.5 * 1.5;

    OUGHT(round <= ideal + 16 * 8 * 0.5 * byte && round >= ideal - 0.2 * 2 * std::numbers::pi * 1.5 - 16 * 8 * 0.5 * byte);

    const Path upward = segment(Point{4, 10}, Point{4, 2});
    const std::vector<Point> standing{{3, 2}, {5, 2}, {5, 10}, {3, 10}};
    Stroke thin;

    thin.width = 2;
    OUGHT(alike(stroked(8, 12, upward, thin), filled(8, 12, standing)));
    SAME(covered(stroked(8, 12, segment(Point{4, 4}, Point{4, 4}), style)), 0.0);

    thin.width = 0;
    SAME(covered(stroked(8, 12, upward, thin)), 0.0);
}

TRIAL("Drawing: joins of a right angle turning either way (miter, bevel, round), the miter limit, a pen flattened with the path") {
    Path corner;

    corner.move(Point{2, 10});
    corner.line(Point{10, 10});
    corner.line(Point{10, 2});

    Stroke style;

    style.width = 2;

    const std::vector<Point> ell{{2, 9}, {9, 9}, {9, 2}, {11, 2}, {11, 11}, {2, 11}};

    OUGHT(alike(stroked(14, 14, corner, style), filled(14, 14, ell), 1));

    style.join = Join::bevel;
    OUGHT(std::abs(covered(stroked(14, 14, corner, style)) - (area(ell) - 0.5)) < 0.05);

    style.join = Join::round;
    const double rounded = covered(stroked(14, 14, corner, style));
    const double cut = area(ell) - (1 - std::numbers::pi / 4);

    OUGHT(rounded <= cut + 0.02 && rounded >= cut - 2.0 / 3.0 * 0.1 * std::numbers::pi / 2 - 0.02);

    Path other;
    Stroke turned;

    other.move(Point{2, 4});
    other.line(Point{10, 4});
    other.line(Point{10, 12});
    turned.width = 2;

    const std::vector<Point> mirrored{{2, 3}, {11, 3}, {11, 12}, {9, 12}, {9, 5}, {2, 5}};

    OUGHT(alike(stroked(14, 14, other, turned), filled(14, 14, mirrored), 1));

    turned.join = Join::bevel;
    OUGHT(std::abs(covered(stroked(14, 14, other, turned)) - (area(mirrored) - 0.5)) < 0.05);

    turned.join = Join::round;

    const double round_other = covered(stroked(14, 14, other, turned));

    OUGHT(round_other <= cut + 0.02 && round_other >= cut - 2.0 / 3.0 * 0.1 * std::numbers::pi / 2 - 0.02);

    Path sharp;

    sharp.move(Point{2, 20});
    sharp.line(Point{30, 18});
    sharp.line(Point{2, 16});

    Stroke pointed;

    pointed.width = 2;
    pointed.miter = 10;

    const double ratio = 1 / std::sin(std::atan2(2.0, 28.0));

    OUGHT(ratio > 10 && ratio < 20);
    SAME(alpha(stroked(48, 32, sharp, pointed), 33, 17), 0.0);

    pointed.miter = 20;
    OUGHT(alpha(stroked(48, 32, sharp, pointed), 33, 17) > 0.5);

    Path circle;

    circle.arc(Point{20, 20}, 8, 0, 2 * std::numbers::pi);
    circle.close();

    Stroke ring;

    ring.width = 2;

    const tal::media::Pixels wide = stroked(40, 40, circle, ring, Matrix{2, 0, 0, 1, -20, 0});

    double across = 0;
    double down = 0;

    for (int x = 28; x < 40; ++x) across += alpha(wide, x, 20);

    for (int y = 0; y < 16; ++y) down += alpha(wide, 20, y);

    SAME(alpha(wide, 20, 12), 1.0);
    SAME(alpha(wide, 20, 10), 0.0);
    OUGHT(std::abs(across - 4) < 0.3);
    OUGHT(std::abs(down - 2) < 0.1);
}

TRIAL("Drawing: dashes laid from the offset, an odd list counted twice, dots of dashes of no length, a dash over the start of a closed path") {
    const Path line = segment(Point{0, 2}, Point{20, 2});
    Stroke style;

    style.width = 2;
    style.dashes = {4, 2};

    const tal::media::Pixels dashed = stroked(20, 4, line, style);

    for (int x = 0; x < 20; ++x) SAME(alpha(dashed, x, 1), x % 6 < 4 ? 1.0 : 0.0);

    style.offset = 1;

    const tal::media::Pixels shifted = stroked(20, 4, line, style);

    for (int x = 0; x < 20; ++x) SAME(alpha(shifted, x, 1), (x + 1) % 6 < 4 ? 1.0 : 0.0);

    style.offset = -1;

    const tal::media::Pixels back = stroked(20, 4, line, style);

    for (int x = 0; x < 20; ++x) SAME(alpha(back, x, 1), (x + 5) % 6 < 4 ? 1.0 : 0.0);

    style.offset = 0;
    style.dashes = {3};

    const tal::media::Pixels odd = stroked(20, 4, line, style);

    for (int x = 0; x < 20; ++x) SAME(alpha(odd, x, 1), x % 6 < 3 ? 1.0 : 0.0);

    style.dashes = {3, -1};
    SAME(covered(stroked(20, 4, line, style)), 40.0);

    style.dashes = {0, 0};
    SAME(covered(stroked(20, 4, line, style)), 40.0);

    Stroke dots;

    dots.width = 2;
    dots.cap = Cap::round;
    dots.dashes = {0, 5};

    const tal::media::Pixels dotted = stroked(24, 4, segment(Point{2, 2}, Point{22, 2}), dots);
    const double each = std::numbers::pi;

    OUGHT(std::abs(covered(dotted) - 5 * each) < 5 * 0.2 * 2 * std::numbers::pi + 24 * 4 * 0.5 * byte);
    OUGHT(alpha(dotted, 2, 1) > 0.5 && alpha(dotted, 7, 1) > 0.5 && alpha(dotted, 4, 1) == 0);

    dots.cap = Cap::butt;
    SAME(covered(stroked(24, 4, segment(Point{2, 2}, Point{22, 2}), dots)), 0.0);

    Path square;

    square.rectangle(Rect{Point{4, 4}, 16, 16});

    Stroke around;

    around.width = 2;
    around.dashes = {10, 6};
    around.offset = 4;

    const tal::media::Pixels closed = stroked(24, 24, square, around);

    SAME(alpha(closed, 4, 3), 1.0);
    SAME(alpha(closed, 3, 4), 1.0);
    SAME(alpha(closed, 3, 3), 1.0);
    SAME(alpha(closed, 10, 3), 0.0);
    SAME(alpha(closed, 3, 8), 0.0);
}

// Dashes are laid only where they can be seen; what lies beyond the view moves the pattern by its
// true length. A line from a million pixels away and a quarter circle of radius 1e7 ending at the
// view (each far more dashes than a stroke may have) show within the view the dashes their length
// puts there: the phase is the length so far, modulo the pattern's eight; so does a cubic curve
// fifty thousand pixels away with a cusp at five eighths of it (the first four fifths of the
// curve of (0, 0), (1, 1), (0, 1), (1, 0)), whose speed falls to nothing and rises again, so that
// Gauss and Legendre over its halves miss (its length by the midpoint rule on 200000 parts here,
// near enough for a phase). A line through the view
// and a million pixels past it keeps whole the dash that leaves the view (from 46 to 50, the view
// with its reach ending at 49). A dashed circle seen under a zoom of 6e8 draws, and does so
// without flattening the whole circle.
TRIAL("Drawing: dashes laid only where seen, at the phase of the true length before them; a dashed circle far zoomed in draws") {
    const auto striped = [](const tal::media::Pixels& pixels, double phase) {
        for (int x = 0; x < 48; ++x) {
            const double start = std::fmod(phase + x, 8.0);
            const double end = start + 1;

            if (end <= 4 - 0.01 || (start >= 0.01 && end <= 4)) {
                if (std::abs(alpha(pixels, x, 15) - 1) > 0.02 || std::abs(alpha(pixels, x, 16) - 1) > 0.02) return false;
            } else if (start >= 4.01 && end <= 7.99) {
                if (alpha(pixels, x, 15) > 0.02 || alpha(pixels, x, 16) > 0.02) return false;
            }
        }

        return true;
    };

    Stroke dashed;

    dashed.width = 2;
    dashed.cap = Cap::butt;
    dashed.dashes = {4, 4};

    Raster straight{tal::media::Size{48, 32}, Blending::srgb};
    Pen ruler{straight};

    ruler.stroke(segment(Point{-1e6, 16}, Point{100, 16}), white, dashed);
    OUGHT(striped(straight.pixels(), 1e6));

    Raster across{tal::media::Size{48, 32}, Blending::srgb};
    Pen through{across};
    Stroke round = dashed;

    round.join = Join::round;
    through.stroke(segment(Point{-1e6 - 2, 16}, Point{1e6, 16}), white, round);
    OUGHT(striped(across.pixels(), 1e6 + 2));
    OUGHT(alpha(across.pixels(), 47, 16) > 0.98);

    const double radius = 1e7;
    Path bend;

    bend.move(Point{-radius, 16 + radius});
    bend.arc(Point{0, 16 + radius}, radius, std::numbers::pi, 1.5 * std::numbers::pi);
    bend.line(Point{100, 16});

    Raster curved{tal::media::Size{48, 32}, Blending::srgb};
    Pen compass{curved};

    compass.stroke(bend, white, dashed);
    OUGHT(striped(curved.pixels(), std::fmod(std::numbers::pi / 2 * radius, 8.0)));

    const Point wave[4] = {Point{-5e4, 16}, Point{-5e4 + 8000, 16 + 8000}, Point{-5e4 + 3200, 16 + 9600}, Point{-5e4 + 6080, 16 + 4800}};
    double travelled = 0;

    for (int part = 0; part < 200000; ++part) {
        const double t = (part + 0.5) / 200000;
        const double u = 1 - t;
        const double dx = 3 * (u * u * (wave[1].x - wave[0].x) + 2 * u * t * (wave[2].x - wave[1].x) + t * t * (wave[3].x - wave[2].x));
        const double dy = 3 * (u * u * (wave[1].y - wave[0].y) + 2 * u * t * (wave[2].y - wave[1].y) + t * t * (wave[3].y - wave[2].y));

        travelled += std::hypot(dx, dy) / 200000;
    }

    Path swing;

    swing.move(wave[0]);
    swing.cubic(wave[1], wave[2], wave[3]);
    swing.line(Point{-1000, 16});
    swing.line(Point{100, 16});

    Raster swung{tal::media::Size{48, 32}, Blending::srgb};
    Pen pendulum{swung};

    pendulum.stroke(swing, white, dashed);
    OUGHT(striped(swung.pixels(), std::fmod(travelled + std::hypot(wave[3].x + 1000, wave[3].y - 16) + 1000, 8.0)));

    Path circle;
    Stroke rim;

    circle.arc(Point{120, 120}, 120, 0, 2 * std::numbers::pi);
    rim.width = 1e-8;
    rim.dashes = {1e-8, 1e-8};

    Raster zoomed{tal::media::Size{48, 32}, Blending::srgb};
    Pen lens{zoomed};

    lens.transform(Matrix{6e8, 0, 0, 6e8, -6e8 * 120 + 24, -6e8 * 0 + 16});
    lens.stroke(circle, white, rim);
    OUGHT(covered(zoomed.pixels()) > 1);
}

TRIAL("Drawing: a clip lets through its coverage, clips multiply, a restore lifts them") {
    Raster raster{tal::media::Size{12, 12}, Blending::srgb};
    Pen pen{raster};
    Path inner;
    Path all;

    inner.rectangle(Rect{Point{2, 2}, 4.5, 6});
    all.rectangle(Rect{Point{0, 0}, 12, 12});
    pen.save();
    pen.clip(inner);
    pen.fill(all, white);

    tal::media::Pixels seen = raster.pixels();

    SAME(alpha(seen, 3, 3), 1.0);
    SAME(alpha(seen, 1, 3), 0.0);
    SAME(pixel(seen, 6, 3)[3], 128);
    SAME(alpha(seen, 3, 9), 0.0);

    Path other;

    other.rectangle(Rect{Point{4, 0}, 8, 12});
    pen.clip(other);
    raster.clear();
    pen.fill(all, white);
    seen = raster.pixels();
    SAME(alpha(seen, 3, 3), 0.0);
    SAME(alpha(seen, 5, 3), 1.0);

    pen.restore();
    raster.clear();
    pen.fill(all, white);
    SAME(covered(raster.pixels()), 144.0);

    Path none;

    pen.save();
    pen.clip(none);
    pen.fill(all, Color{1, 0, 0, 1});
    pen.restore();
    SAME(pixel(raster.pixels(), 5, 5), (std::array<int, 4>{255, 255, 255, 255}));
}

TRIAL("Drawing: gradients in the space of blending, padded beyond their ends, of no stops nothing; patterns repeated or not; images whole and in part") {
    for (const Blending blending : {Blending::srgb, Blending::linear}) {
        Raster raster{tal::media::Size{10, 2}, blending};
        Pen pen{raster};
        Path all;
        Gradient across = Gradient::linear(Point{0, 0}, Point{10, 0});

        across.stop(0, Color{1, 0, 0, 1});
        across.stop(1, Color{0, 0, 1, 1});
        all.rectangle(Rect{Point{0, 0}, 10, 2});
        pen.fill(all, across);

        const tal::media::Pixels seen = raster.pixels();

        for (int x = 0; x < 10; ++x) {
            const double t = (x + 0.5) / 10;
            const double red = blending == Blending::srgb ? 1 - t : srgb_of(1 - t);
            const double blue = blending == Blending::srgb ? t : srgb_of(t);

            OUGHT(std::abs(pixel(seen, x, 0)[0] - red * 255) <= 1);
            OUGHT(std::abs(pixel(seen, x, 0)[2] - blue * 255) <= 1);
        }
    }

    Raster raster{tal::media::Size{20, 20}, Blending::srgb};
    Pen pen{raster};
    Path all;

    all.rectangle(Rect{Point{0, 0}, 20, 20});

    Gradient round = Gradient::radial(Point{10.5, 10.5}, 0, Point{10.5, 10.5}, 5);

    round.stop(0, Color{0, 1, 0, 1});
    round.stop(1, Color{0, 0, 0, 1});
    pen.fill(all, round);
    SAME(pixel(raster.pixels(), 10, 10), (std::array<int, 4>{0, 255, 0, 255}));
    SAME(pixel(raster.pixels(), 1, 1), (std::array<int, 4>{0, 0, 0, 255}));

    raster.clear();
    pen.fill(all, Gradient::linear(Point{0, 0}, Point{1, 0}));
    SAME(covered(raster.pixels()), 0.0);
    Gradient endless = Gradient::linear(Point{-1e308, 0}, Point{1e308, 0});

    endless.stop(0, white);
    endless.stop(1, white);
    raster.clear();
    pen.fill(all, endless);
    SAME(covered(raster.pixels()), 0.0);

    Pattern far_away{ramp(2, 2)};

    far_away.place(Matrix::scaled(1e-300, 1e-300));
    pen.fill(all, far_away);
    SAME(covered(raster.pixels()), 0.0);
    THREW(round.stop(1.5, white));
    THREW((void)Gradient::radial(Point{}, -1, Point{}, 2));

    const tal::media::Pixels checker = ramp(2, 2);
    Pattern tiles{checker};

    raster.clear();
    pen.smoothing(false);
    pen.fill(all, tiles);

    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 20; ++x) SAME(pixel(raster.pixels(), x, y), pixel(checker, x % 2, y % 2));

    Pattern once{checker, false, false};

    once.place(Matrix::moved(3, 4));
    raster.clear();
    pen.fill(all, once);
    SAME(pixel(raster.pixels(), 3, 4), pixel(checker, 0, 0));
    SAME(pixel(raster.pixels(), 4, 5), pixel(checker, 1, 1));
    SAME(pixel(raster.pixels(), 5, 4)[3], 0);
    SAME(pixel(raster.pixels(), 2, 4)[3], 0);

    for (const Blending blending : {Blending::srgb, Blending::linear}) {
        for (const bool smooth : {false, true}) {
            const tal::media::Pixels picture = ramp(4, 3);
            Raster onto{tal::media::Size{4, 3}, blending};
            Pen drawing{onto};

            drawing.smoothing(smooth);
            drawing.draw(picture, Rect{Point{0, 0}, 4, 3});
            OUGHT(alike(onto.pixels(), picture));
        }
    }

    const tal::media::Pixels picture = ramp(4, 3);
    Raster onto{tal::media::Size{8, 8}, Blending::srgb};
    Pen drawing{onto};

    drawing.smoothing(false);
    drawing.draw(picture, Rect{Point{1, 1}, 2, 2}, Rect{Point{5, 5}, -2, -2});
    SAME(pixel(onto.pixels(), 3, 3), pixel(picture, 1, 1));
    SAME(pixel(onto.pixels(), 4, 4), pixel(picture, 2, 2));
    SAME(pixel(onto.pixels(), 2, 3)[3], 0);

    onto.clear();
    drawing.draw(picture, Rect{Point{2, 1}, 8, 8}, Rect{Point{0, 0}, 8, 8});
    SAME(pixel(onto.pixels(), 0, 0), pixel(picture, 2, 1));
    SAME(pixel(onto.pixels(), 1, 1), pixel(picture, 3, 2));
    SAME(pixel(onto.pixels(), 2, 0)[3], 0);

    SAME(trouble_of([&] { drawing.draw(picture, Rect{Point{0, 0}, 4, 3}, 1); }), std::optional<Kind>{Kind::other});
    drawing.draw(tal::media::Pixels{}, Rect{Point{0, 0}, 4, 3});
}

TRIAL("Drawing: blending in linear light or in sRGB, opacity, translucent images back whole; the state saved and restored") {
    const auto half = [](Blending blending) {
        Raster raster{tal::media::Size{2, 2}, blending};
        Pen pen{raster};
        Path all;

        raster.clear(Color{0, 0, 0, 1});
        all.rectangle(Rect{Point{0, 0}, 2, 2});
        pen.opacity(0.5F);
        pen.fill(all, white);

        return pixel(raster.pixels(), 0, 0);
    };

    int nearest = 0;

    for (int one = 0; one < 256; ++one)
        if (std::abs(light_of(one / 255.0) - 0.5) < std::abs(light_of(nearest / 255.0) - 0.5)) nearest = one;

    SAME(half(Blending::srgb), (std::array<int, 4>{128, 128, 128, 255}));
    SAME(half(Blending::linear), (std::array<int, 4>{nearest, nearest, nearest, 255}));
    OUGHT(std::abs(nearest - srgb_of(0.5) * 255) < 1);

    Raster raster{tal::media::Size{4, 4}, Blending::srgb};
    Pen pen{raster};
    Path all;

    all.rectangle(Rect{Point{0, 0}, 4, 4});
    pen.opacity(0.5F);
    pen.fill(all, white);
    SAME(pixel(raster.pixels(), 0, 0)[3], 128);

    pen.opacity(7);
    raster.clear();
    pen.fill(all, white);
    SAME(pixel(raster.pixels(), 0, 0)[3], 255);

    for (const Blending blending : {Blending::srgb, Blending::linear}) {
        for (const int alpha_of : {1, 64, 128, 200}) {
            const tal::media::Pixels picture = ramp(4, 3, alpha_of);
            Raster onto{tal::media::Size{4, 3}, blending};
            Pen drawing{onto};

            drawing.draw(picture, Rect{Point{0, 0}, 4, 3});
            OUGHT(alike(onto.pixels(), picture, alpha_of > 1 ? 0 : blending == Blending::srgb ? 1 : 8));
        }
    }

    pen.opacity(1);
    pen.save();
    pen.transform(Matrix::moved(2, 0));
    pen.opacity(0.25F);
    pen.smoothing(false);
    SAME(pen.placed(), Matrix::moved(2, 0));
    pen.transform(Matrix::scaled(2, 2));
    SAME(pen.placed().applied(Point{1, 1}), (Point{4, 2}));
    pen.restore();
    SAME(pen.placed(), Matrix{});
    raster.clear();
    pen.fill(all, white);
    SAME(pixel(raster.pixels(), 0, 0)[3], 255);
    SAME(trouble_of([&] { pen.restore(); }), std::optional<Kind>{Kind::other});
}

TRIAL("Drawing: points inside a path and on its stroke, in the raster's pixels, through the matrix") {
    Raster raster{tal::media::Size{4, 4}};
    Pen pen{raster};
    Path box;

    box.rectangle(Rect{Point{1, 1}, 4, 2});
    OUGHT(pen.inside(box, Point{2, 2}));
    OUGHT(pen.inside(box, Point{1, 2}));
    OUGHT(pen.inside(box, Point{5, 3}));
    OUGHT(!pen.inside(box, Point{0.5, 2}));
    OUGHT(!pen.inside(box, Point{2, 3.5}));

    pen.transform(Matrix::moved(10, 0));
    OUGHT(pen.inside(box, Point{12, 2}));
    OUGHT(!pen.inside(box, Point{2, 2}));

    Stroke style;

    style.width = 1;
    OUGHT(pen.on(box, Point{11, 1.4}));
    OUGHT(!pen.on(box, Point{12, 2}));
    OUGHT(!pen.on(box, Point{11, 0.4}));
    style.width = 0;
    OUGHT(!pen.on(box, Point{11, 1}, style));

    std::vector<Point> star;

    for (int one = 0; one < 5; ++one) {
        const double angle = -std::numbers::pi / 2 + one * 4 * std::numbers::pi / 5;

        star.push_back(Point{16 + 15 * std::cos(angle), 16 + 15 * std::sin(angle)});
    }

    pen.place(Matrix{});
    OUGHT(pen.inside(polygon_of(star), Point{16, 16}, Rule::nonzero));
    OUGHT(!pen.inside(polygon_of(star), Point{16, 16}, Rule::evenodd));
}

TRIAL("Drawing: a path keeps the rules of Canvas: moves after closing and before curves, a line before an arc, an arc's sweep, a rectangle closed") {
    using Kind = Path::Kind;
    const auto kinds = [](const Path& path) {
        std::vector<Kind> made;

        for (const Path::Piece& one : path.pieces()) made.push_back(one.kind);

        return made;
    };

    Path closing;

    closing.move(Point{1, 1});
    closing.line(Point{2, 1});
    closing.close();
    closing.close();
    closing.line(Point{3, 3});
    SAME(kinds(closing), (std::vector<Kind>{Kind::move, Kind::line, Kind::close, Kind::move, Kind::line}));
    SAME(closing.pieces()[3].points[0], (Point{1, 1}));

    Path lone;

    lone.line(Point{5, 5});
    lone.quad(Point{6, 6}, Point{7, 5});
    SAME(kinds(lone), (std::vector<Kind>{Kind::move, Kind::quad}));

    Path curved;

    curved.cubic(Point{1, 2}, Point{3, 4}, Point{5, 6});
    SAME(kinds(curved), (std::vector<Kind>{Kind::move, Kind::cubic}));
    SAME(curved.pieces()[0].points[0], (Point{1, 2}));

    Path arcs;

    arcs.move(Point{0, 0});
    arcs.arc(Point{10, 10}, 5, 0, std::numbers::pi / 2);
    SAME(kinds(arcs), (std::vector<Kind>{Kind::move, Kind::line, Kind::arc}));
    OUGHT(std::abs(arcs.pieces()[1].points[0].x - 15) < 1e-12 && std::abs(arcs.pieces()[1].points[0].y - 10) < 1e-12);
    OUGHT(std::abs(arcs.pieces()[2].sweep - std::numbers::pi / 2) < 1e-12);

    const auto sweep = [](double from, double to, bool backwards) {
        Path one;

        one.arc(Point{0, 0}, 1, from, to, backwards);

        return one.pieces().back().sweep;
    };

    OUGHT(std::abs(sweep(0, -std::numbers::pi / 2, false) - 3 * std::numbers::pi / 2) < 1e-12);
    OUGHT(std::abs(sweep(0, -std::numbers::pi / 2, true) + std::numbers::pi / 2) < 1e-12);
    OUGHT(std::abs(sweep(0, 7, false) - 2 * std::numbers::pi) < 1e-12);
    OUGHT(std::abs(sweep(7, 0, true) + 2 * std::numbers::pi) < 1e-12);
    SAME(sweep(1, 1, false), 0.0);

    Path box;

    box.rectangle(Rect{Point{1, 2}, 3, 4});
    SAME(kinds(box), (std::vector<Kind>{Kind::move, Kind::line, Kind::line, Kind::line, Kind::close, Kind::move}));
    OUGHT(!box.empty());
    OUGHT(Path{}.empty());
}

TRIAL("Drawing: curves flattened within a tenth of a pixel once mapped, a curve beyond the view in one line") {
    using tal::drawing::flattening::flattened;
    using tal::drawing::flattening::Line;

    Path curves;

    curves.move(Point{0, 0});
    curves.quad(Point{50, 80}, Point{100, 0});
    curves.cubic(Point{120, 90}, Point{160, -90}, Point{200, 10});

    const Matrix stretched = Matrix::scaled(3, 1.5);
    const std::vector<Line> lines = flattened(curves, stretched, 0.1);

    SAME(lines.size(), std::size_t{1});

    const auto quad = [&](double t) {
        const double u = 1 - t;

        return stretched.applied(Point{2 * u * t * 50 + t * t * 100, 2 * u * t * 80});
    };
    const auto cubic = [&](double t) {
        const double u = 1 - t;

        return stretched.applied(Point{u * u * u * 100 + 3 * u * u * t * 120 + 3 * u * t * t * 160 + t * t * t * 200, 3 * u * u * t * 90 - 3 * u * t * t * 90 + t * t * t * 10});
    };
    const auto distance = [&](Point at) {
        double nearest = 1e300;

        const std::vector<Point>& points = lines.front().points;

        for (std::size_t one = 0; one + 1 < points.size(); ++one) {
            const Point p = points[one];
            const Point q = points[one + 1];
            const double dx = q.x - p.x;
            const double dy = q.y - p.y;
            const double t = std::clamp(((at.x - p.x) * dx + (at.y - p.y) * dy) / (dx * dx + dy * dy), 0.0, 1.0);

            nearest = std::min(nearest, std::hypot(at.x - p.x - t * dx, at.y - p.y - t * dy));
        }

        return nearest;
    };

    for (int one = 0; one <= 1000; ++one) {
        OUGHT(distance(quad(one / 1000.0)) <= 0.1 + 1e-9);
        OUGHT(distance(cubic(one / 1000.0)) <= 0.1 + 1e-9);
    }

    OUGHT(lines.front().points.size() < 400);

    Path far;

    far.move(Point{-100, 0});
    far.cubic(Point{-1e15, 50}, Point{-1e15, -50}, Point{-100, 10});

    const std::vector<Line> beyond = flattened(far, Matrix{}, 0.1, tal::drawing::flattening::Bounds{0, 0, 10, 10});

    SAME(beyond.front().points.size(), std::size_t{2});
    SAME(trouble_of([&] { (void)flattened(far, Matrix{}, 0.1); }), std::optional<Kind>{Kind::full});
}

TRIAL("Drawing: a curve or an arc of any length passing through the view costs the lines within it and draws as near; strokes and questions alike, dashes by the true length, caps and joins by the curve's own direction") {
    using tal::drawing::flattening::Bounds;
    using tal::drawing::flattening::flattened;
    using tal::drawing::flattening::Line;

    const double high = 1e9;
    const double half = 223607;
    const double bend = (4 * high + 64) / (4 * half * half);
    const auto valley = [&](double x) { return 16 - bend * (x - 24) * (x - 24); };
    Path cup;

    cup.move(Point{24 - half, -high});
    cup.quad(Point{24, high + 32}, Point{24 + half, -high});
    cup.close();

    std::size_t points = 0;

    std::size_t corners = 0;

    for (const auto& line : flattened(cup, Matrix{}, 0.1, Bounds{0, 0, 48, 32})) {
        points += line.points.size();
        corners += static_cast<std::size_t>(std::count(line.corners.begin(), line.corners.end(), true));
    }

    OUGHT(points < 3000);
    SAME(corners, std::size_t{2});

    Path loop;
    double measured = 0;

    loop.arc(Point{24, 16}, 10, 0, 2 * std::numbers::pi);

    const std::vector<Line> measuring = flattened(loop, Matrix{}, 0.1, std::nullopt, 0, tal::drawing::flattening::Dashes{{5, 5}, 0, 0.01, 2});

    for (const double one : measuring.front().lengths) measured += one;

    OUGHT(std::abs(measured - 20 * std::numbers::pi) < 1e-6);

    const Line long_one{.points = {Point{0, 2}, Point{20, 2}}, .corners = {true, true}, .lengths = {40}};
    Stroke halves;

    halves.width = 2;
    halves.dashes = {10, 30};

    for (const Line& piece : tal::drawing::stroking::outlined({long_one}, halves, 0.1))
        for (const Point& one : piece.points) OUGHT(one.x >= -1e-9 && one.x <= 5 + 1e-9);

    Path clockwise;
    Path anticlockwise;

    clockwise.arc(Point{24, 16}, 10, 0, 2);
    anticlockwise.arc(Point{24, 16}, 10, 2, 0, true);
    SAME(flattened(clockwise, Matrix{}, 0.1, std::nullopt, 1e-3).front().points.size(), flattened(anticlockwise, Matrix{}, 0.1, std::nullopt, 1e-3).front().points.size());

    std::vector<Point> near;

    for (double x = -10; x <= 58; x += 0.05) near.push_back(Point{x, valley(x)});

    near.push_back(Point{58, -100});
    near.push_back(Point{-10, -100});

    Raster raster{tal::media::Size{48, 32}, Blending::srgb};
    Pen pen{raster};

    pen.fill(cup, white);
    OUGHT(covers_alike(raster.pixels(), filled(48, 32, near), 30));
    OUGHT(pen.inside(cup, Point{24, 15.7}));
    OUGHT(!pen.inside(cup, Point{24, 16.3}));
    OUGHT(pen.inside(cup, Point{2, valley(2) - 0.3}));
    OUGHT(!pen.inside(cup, Point{2, valley(2) + 0.3}));

    Path wheel;

    wheel.arc(Point{24, high + 16}, high, 0, 2 * std::numbers::pi);
    points = 0;

    for (const auto& line : flattened(wheel, Matrix{}, 0.1, Bounds{0, 0, 48, 32})) points += line.points.size();

    OUGHT(points < 3000);

    Path away;
    Path vast;

    away.move(Point{4.5, 88.5});
    away.cubic(Point{4.5, 88.5}, Point{88.5, 0}, Point{1.6e69, 10.5});
    vast.arc(Point{24, 1e150}, 1e150, 0, 2 * std::numbers::pi);

    for (const Path* one : {&away, &vast}) {
        points = 0;

        for (const auto& line : flattened(*one, Matrix{}, 0.1, Bounds{-120, -120, 168, 152})) points += line.points.size();

        OUGHT(points < 3000);
    }

    Path stretched;

    stretched.arc(Point{0, 16}, 10, -1, 1);
    points = 0;

    for (const auto& line : flattened(stretched, Matrix{1e15, 0, 0, 1, -1e16 + 24, 0}, 0.1, Bounds{0, 0, 48, 32})) points += line.points.size();

    OUGHT(points < 3000);

    Path centered;

    centered.arc(Point{0, 0}, 10, -1, 1);

    for (const Path* one : {&away, &stretched, &centered}) {
        SAME(trouble_of([&] { (void)flattened(*one, Matrix::scaled(1e308, 1e308), 0.1, Bounds{0, 0, 48, 32}); }), std::optional<Kind>{Kind::malformed});
        SAME(trouble_of([&] { (void)flattened(*one, Matrix::scaled(1e308, 1e308), 0.1); }), std::optional<Kind>{Kind::malformed});
    }

    Raster farther{tal::media::Size{48, 32}, Blending::srgb};
    Pen thick{farther};
    Stroke broad;

    broad.width = 109.5;
    broad.miter = 109.5;
    thick.stroke(away, white, broad);
    thick.stroke(vast, white, broad);

    Raster under{tal::media::Size{48, 32}, Blending::srgb};
    Pen filling{under};

    filling.fill(vast, white);
    OUGHT(std::abs(covered(under.pixels()) - 48 * 32) < 1e-6);

    Path largest;
    Raster beside{tal::media::Size{48, 32}, Blending::srgb};
    Pen loop_pen{beside};

    largest.move(Point{24, 8});
    largest.cubic(Point{1.7e308, 8}, Point{1.7e308, 24}, Point{24, 24});
    largest.close();
    loop_pen.fill(largest, white);
    OUGHT(std::abs(covered(beside.pixels()) - 24 * 16) < 1e-6);

    Raster round{tal::media::Size{48, 32}, Blending::srgb};
    Pen drawn{round};

    drawn.fill(wheel, white);
    OUGHT(covers_alike(round.pixels(), filled(48, 32, {Point{0, 16}, Point{48, 16}, Point{48, 32}, Point{0, 32}}), 1));

    const Matrix turned = Matrix::moved(-24, -16).then(Matrix::turned(0.4)).then(Matrix::moved(24, 16));
    Path open;
    Path mapped;

    open.move(Point{24 - half, -high});
    open.quad(Point{24, high + 32}, Point{24 + half, -high});
    mapped.move(turned.applied(Point{24 - half, -high}));
    mapped.quad(turned.applied(Point{24, high + 32}), turned.applied(Point{24 + half, -high}));

    Stroke style;

    style.width = 2;
    style.cap = tal::drawing::Cap::square;
    OUGHT(covers_alike(stroked(48, 32, open, style, turned), stroked(48, 32, mapped, style), 2));
    OUGHT(pen.on(open, Point{24, 16.8}, style));
    OUGHT(!pen.on(open, Point{24, 17.3}, style));

    Path hidden;

    hidden.move(Point{-1000, 16});
    hidden.cubic(Point{-1000, -500}, Point{-600, -500}, Point{-600, 16});
    hidden.line(Point{40, 16});

    double length = 0;
    Point last{-1000, 16};

    for (int one = 1; one <= 1000000; ++one) {
        const double t = one / 1000000.0;
        const double u = 1 - t;
        const Point at{u * u * u * -1000 + 3 * u * u * t * -1000 + 3 * u * t * t * -600 + t * t * t * -600, u * u * u * 16 + 3 * u * u * t * -500 + 3 * u * t * t * -500 + t * t * t * 16};

        length += std::hypot(at.x - last.x, at.y - last.y);
        last = at;
    }

    Stroke dashed;

    dashed.width = 4;
    dashed.dashes = {5, 5};

    Stroke shifted = dashed;

    shifted.offset = length;
    OUGHT(covers_alike(stroked(48, 32, hidden, dashed), stroked(48, 32, segment(Point{-600, 16}, Point{40, 16}), shifted), 30));

    const auto unseen = [](const Path& path, const Stroke& used) {
        const tal::media::Pixels seen = stroked(48, 32, path, used);
        const tal::media::Pixels whole = stroked(88, 32, path, used, Matrix::moved(40, 0));
        int most = 0;

        for (std::size_t y = 0; y < 32; ++y)
            for (std::size_t x = 0; x < 48; ++x) most = std::max(most, std::abs(std::to_integer<int>(seen.stills().front().rgba[(y * 48 + x) * 4 + 3]) - std::to_integer<int>(whole.stills().front().rgba[(y * 88 + x + 40) * 4 + 3])));

        return most;
    };
    Path bulge;
    Path spur;
    Stroke wide;
    Stroke sharp;

    bulge.move(Point{-1, 0});
    bulge.cubic(Point{-0.2, 10}, Point{-0.2, 20}, Point{-1, 30});
    wide.width = 4;
    spur.move(Point{-30, 30});
    spur.quad(Point{-20, 16}, Point{-6, 16});
    spur.line(Point{-6 - 30 * std::cos(0.2513), 16 + 30 * std::sin(0.2513)});
    sharp.width = 2;
    sharp.join = tal::drawing::Join::miter;
    sharp.miter = 10;
    SAME(unseen(bulge, wide), 0);
    SAME(unseen(spur, sharp), 0);
    OUGHT(covered(stroked(48, 32, spur, sharp)) > 0);
    sharp.miter = 1 / std::sin(0.2513 / 2) + 0.02;
    OUGHT(covered(stroked(48, 32, spur, sharp)) > 0);
    sharp.miter = 1 / std::sin(0.2513 / 2) - 0.02;
    SAME(covered(stroked(48, 32, spur, sharp)), 0.0);
    sharp.miter = 10;

    Path back;

    back.move(Point{-6 - 30 * std::cos(0.2513), 16 + 30 * std::sin(0.2513)});
    back.line(Point{-6, 16});
    back.quad(Point{-20, 16}, Point{-30, 30});
    SAME(unseen(back, sharp), 0);
    OUGHT(covered(stroked(48, 32, back, sharp)) > 0);

    Path ring;
    std::vector<Point> sectors;
    Stroke quarters;

    ring.arc(Point{24, 16}, 10, 0, 2 * std::numbers::pi);
    quarters.width = 8;
    quarters.dashes = {2 * std::numbers::pi * 10 / 8};

    Raster pieces{tal::media::Size{48, 32}, Blending::srgb};
    Pen cut{pieces};

    for (int one = 0; one < 4; ++one) {
        Path sector;
        const double from = one * std::numbers::pi / 2;

        sector.move(Point{24 + 14 * std::cos(from), 16 + 14 * std::sin(from)});

        for (int step = 1; step <= 400; ++step) sector.line(Point{24 + 14 * std::cos(from + step * std::numbers::pi / 1600), 16 + 14 * std::sin(from + step * std::numbers::pi / 1600)});

        for (int step = 400; step >= 0; --step) sector.line(Point{24 + 6 * std::cos(from + step * std::numbers::pi / 1600), 16 + 6 * std::sin(from + step * std::numbers::pi / 1600)});

        sector.close();
        cut.fill(sector, white);
    }

    OUGHT(covers_alike(stroked(48, 32, ring, quarters), pieces.pixels(), 30));
}

// Round caps and joins of a stroke far wider than the view: each pixel covered as the stroke's true
// shape covers it (the points within half the width of the path, sampled sixteen by sixteen),
// within the tolerance (and the numbers' own spacing there, half a pixel at 4·10¹⁵), for half
// widths from a thousand to four quadrillion pixels, reaching the view from each side, and through
// matrices that turn and scale either way; and the outline keeps only the steps of the arcs the
// view shows, a few hundred points where the whole arcs would take millions. A round part the
// matrix carries past the range of numbers is malformed; one through a matrix that flattens the
// plane ends, drawing nothing.
TRIAL("Drawing: round caps and joins far wider than the view cover as the true shape and cost the arcs seen") {
    using tal::drawing::flattening::Bounds;
    using tal::drawing::flattening::Line;

    const auto shape = [](const std::vector<Point>& points, double half, int x, int y) {
        int inside = 0;

        for (int sy = 0; sy < 16; ++sy)
            for (int sx = 0; sx < 16; ++sx) {
                const Point sample{x + (sx + 0.5) / 16, y + (sy + 0.5) / 16};
                double nearest = std::numeric_limits<double>::infinity();

                for (std::size_t at = 0; at + 1 < points.size(); ++at) {
                    const Point from = points[at];
                    const Point d{points[at + 1].x - from.x, points[at + 1].y - from.y};
                    const double along = std::clamp(((sample.x - from.x) * d.x + (sample.y - from.y) * d.y) / (d.x * d.x + d.y * d.y), 0.0, 1.0);

                    nearest = std::min(nearest, std::hypot(sample.x - (from.x + along * d.x), sample.y - (from.y + along * d.y)));
                }

                inside += nearest <= half ? 1 : 0;
            }

        return inside / 256.0;
    };

    const auto turned = [](const std::vector<Point>& offsets, int quarter) {
        std::vector<Point> made;

        for (Point one : offsets) {
            for (int turn = 0; turn < quarter; ++turn) one = Point{-one.y, one.x};

            made.push_back(Point{32 + one.x, 24 + one.y});
        }

        return made;
    };

    for (const double half : {1e3, 1e9, 1e13, 4e15})
        for (int quarter = 0; quarter < 4; ++quarter) {
            const double room = half > 1e15 ? 0.62 : 0.12;
            const std::vector<Point> capped = turned({Point{-half - half / 10 + 8, 0}, Point{-half + 8, 0}}, quarter);
            const std::vector<Point> turning = turned({Point{-half + 8, -half / 10}, Point{-half + 8, 0}, Point{-half - half / 10 + 8, 0}}, quarter);

            for (const std::vector<Point>& points : {capped, turning}) {
                Path path;
                Stroke round;

                path.move(points.front());

                for (std::size_t at = 1; at < points.size(); ++at) path.line(points[at]);

                round.width = 2 * half;
                round.cap = Cap::round;
                round.join = Join::round;

                const tal::media::Pixels drawn = stroked(64, 48, path, round);
                double most = 0;

                for (int y = 0; y < 48; ++y)
                    for (int x = 0; x < 64; ++x) most = std::max(most, std::abs(alpha(drawn, x, y) - shape(points, half, x, y)));

                OUGHT(most < room);

                Line line;

                line.points = points;
                line.corners.assign(points.size(), true);

                std::size_t count = 0;

                for (const Line& piece : tal::drawing::stroking::outlined({line}, round, 0.1, Bounds{-half - 64, -half - 48, half + 128, half + 96}, Bounds{0, 0, 64, 48})) count += piece.points.size();

                OUGHT(count < 2000);
            }
        }

    for (const double half : {1e3, 1e9, 1e13})
        for (const Matrix& matrix : {Matrix::turned(0.3).then(Matrix::scaled(1e6, 1e6)), Matrix::turned(-1.1).then(Matrix::scaled(1e-6, 1e-6))}) {
            const std::optional<Matrix> back = matrix.inverted();
            const double scale = std::hypot(matrix.a, matrix.b);
            const std::vector<Point> capped = turned({Point{-half - half / 10 + 8, 0}, Point{-half + 8, 0}}, 1);
            const std::vector<Point> turning = turned({Point{-half + 8, -half / 10}, Point{-half + 8, 0}, Point{-half - half / 10 + 8, 0}}, 3);

            for (const std::vector<Point>& points : {capped, turning}) {
                Path path;
                Stroke round;

                path.move(back->applied(points.front()));

                for (std::size_t at = 1; at < points.size(); ++at) path.line(back->applied(points[at]));

                round.width = 2 * half / scale;
                round.cap = Cap::round;
                round.join = Join::round;

                const tal::media::Pixels drawn = stroked(64, 48, path, round, matrix);
                double most = 0;

                for (int y = 0; y < 48; ++y)
                    for (int x = 0; x < 64; ++x) most = std::max(most, std::abs(alpha(drawn, x, y) - shape(points, half, x, y)));

                OUGHT(most < 0.12);
            }
        }

    Stroke vast;

    vast.width = 2e160;
    vast.cap = Cap::round;
    SAME(trouble_of([&] { (void)stroked(64, 48, segment(Point{0, 0}, Point{1, 0}), vast, Matrix::turned(0.3).then(Matrix::scaled(1e150, 1e150))); }), std::optional<Kind>{Kind::malformed});

    // A matrix that flattens the plane onto a line through the view keeps every part of a vast
    // round join in view down to turns a step of the numbers apart, where halving ends; what it
    // draws has no area.
    Path bent;
    Stroke wide;

    bent.move(Point{0, 0});
    bent.line(Point{1, 0});
    bent.line(Point{1, 1});
    wide.width = 4e142;
    wide.cap = Cap::round;
    wide.join = Join::round;

    const tal::media::Pixels flat = stroked(64, 48, bent, wide, Matrix{-48, -48, -48, -48, 24, 16});
    double thickest = 0;

    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 64; ++x) thickest = std::max(thickest, alpha(flat, x, y));

    OUGHT(thickest < 0.01);
}

TRIAL("Drawing: matrices compose in order; an odd dash list from an offset past its first round, a negative length drawn whole; images held at their edges, clips within clips; red as green; a stroke flattened in the pen's own scale") {
    for (const auto& [first, second] : {std::pair{Matrix::turned(0.3), Matrix::scaled(2, 3)}, std::pair{Matrix::scaled(2, 3), Matrix::turned(0.4)}, std::pair{Matrix{1, 0.5, -0.25, 1, 3, 4}, Matrix{2, -1, 0.5, 1.5, -2, 1}}}) {
        const Point mapped = first.then(second).applied(Point{1, 2});
        const Point by_hand = second.applied(first.applied(Point{1, 2}));

        OUGHT(std::abs(mapped.x - by_hand.x) < 1e-12 && std::abs(mapped.y - by_hand.y) < 1e-12);
    }

    const Path line = segment(Point{0, 2}, Point{20, 2});
    Stroke style;

    style.width = 2;
    style.dashes = {3};
    style.offset = 4;

    const tal::media::Pixels late = stroked(20, 4, line, style);

    for (int x = 0; x < 20; ++x) SAME(alpha(late, x, 1), (x + 4) % 6 < 3 ? 1.0 : 0.0);

    style.dashes = {-0.5, 3};
    style.offset = 0;
    SAME(covered(stroked(20, 4, line, style)), 40.0);

    Raster big{tal::media::Size{16, 12}};
    Pen held{big};

    held.draw(ramp(4, 3), Rect{Point{0, 0}, 16, 12});
    SAME(pixel(big.pixels(), 0, 0)[3], 255);
    SAME(pixel(big.pixels(), 15, 11)[3], 255);

    Raster clipped{tal::media::Size{20, 4}};
    Pen within{clipped};
    Path left;
    Path right;
    Path all;

    left.rectangle(Rect{Point{0, 0}, 4, 4});
    left.rectangle(Rect{Point{12, 0}, 4, 4});
    right.rectangle(Rect{Point{2, 0}, 18, 4});
    all.rectangle(Rect{Point{0, 0}, 20, 4});
    within.clip(left);
    within.clip(right);
    within.fill(all, white);
    SAME(alpha(clipped.pixels(), 1, 1), 0.0);
    SAME(alpha(clipped.pixels(), 3, 1), 1.0);
    SAME(alpha(clipped.pixels(), 8, 1), 0.0);
    SAME(alpha(clipped.pixels(), 13, 1), 1.0);
    SAME(alpha(clipped.pixels(), 17, 1), 0.0);

    Path cusps;
    Stroke flat;

    cusps.move(Point{10, 16});
    cusps.cubic(Point{90, 16}, Point{-50, 16}, Point{30, 16});
    flat.width = 8;
    flat.join = tal::drawing::Join::bevel;
    SAME(alpha(stroked(48, 32, cusps, flat), 37, 16), 1.0);
    SAME(alpha(stroked(48, 32, cusps, flat), 2, 16), 1.0);

    Raster linear{tal::media::Size{2, 2}, Blending::linear};
    Pen red{linear};
    Path whole;

    whole.rectangle(Rect{Point{0, 0}, 2, 2});
    red.fill(whole, Color{0.5F, 0.5F, 0, 1});
    OUGHT(std::abs(pixel(linear.pixels(), 0, 0)[0] - pixel(linear.pixels(), 0, 0)[1]) <= 1);
    OUGHT(std::abs(pixel(linear.pixels(), 0, 0)[0] - 128) <= 1);

    Path small;
    Stroke thin;

    small.arc(Point{0, 0}, 1, 0, 2 * std::numbers::pi);
    small.close();
    thin.width = 0.2;

    const tal::media::Pixels scaled = stroked(48, 48, small, thin, Matrix::scaled(20, 20).then(Matrix::moved(24, 24)));
    double most = 0;

    for (int y = 0; y < 48; ++y) {
        for (int x = 0; x < 48; ++x) {
            int in = 0;

            for (int i = 0; i < 16; ++i) {
                for (int j = 0; j < 16; ++j) {
                    const double from = std::hypot(x + (i + 0.5) / 16 - 24, y + (j + 0.5) / 16 - 24);

                    in += from >= 18 && from <= 22 ? 1 : 0;
                }
            }

            most = std::max(most, std::abs(alpha(scaled, x, y) - in / 256.0));
        }
    }

    OUGHT(most < 0.12);
}

TRIAL("Drawing: every number that is not finite is malformed and changes nothing; the limits (points, dashes, rows crossed) are full; a raster of no pixels or too many is other") {
    const double nan = std::nan("");
    const double huge = 1e308;
    Path path;

    path.move(Point{1, 1});
    SAME(trouble_of([&] { path.line(Point{nan, 1}); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([&] { path.arc(Point{1, 1}, std::numeric_limits<double>::infinity(), 0, 1); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([&] { path.arc(Point{1, 1}, -1, 0, 1); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([&] { path.rectangle(Rect{Point{huge, 0}, huge, 1}); }), std::optional<Kind>{Kind::malformed});
    SAME(path.pieces().size(), std::size_t{1});

    Raster raster{tal::media::Size{4, 4}};
    Pen pen{raster};
    Stroke bad;

    path.line(Point{3, 3});
    SAME(trouble_of([&] { pen.transform(Matrix{nan, 0, 0, 1, 0, 0}); }), std::optional<Kind>{Kind::malformed});
    SAME(pen.placed(), Matrix{});
    SAME(trouble_of([&] { pen.fill(path, Color{std::nanf(""), 0, 0, 1}); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([&] { raster.clear(Color{0, 0, 0, std::numeric_limits<float>::infinity()}); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([&] { pen.opacity(std::nanf("")); }), std::optional<Kind>{Kind::malformed});

    bad.width = nan;
    SAME(trouble_of([&] { pen.stroke(path, white, bad); }), std::optional<Kind>{Kind::malformed});
    bad.width = -1;
    SAME(trouble_of([&] { pen.stroke(path, white, bad); }), std::optional<Kind>{Kind::malformed});
    bad.width = 1;
    bad.miter = 0;
    SAME(trouble_of([&] { pen.stroke(path, white, bad); }), std::optional<Kind>{Kind::malformed});
    bad.miter = 10;
    bad.dashes = {1, nan};
    SAME(trouble_of([&] { pen.stroke(path, white, bad); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([&] { (void)pen.inside(path, Point{nan, 0}); }), std::optional<Kind>{Kind::malformed});

    Path wide;

    wide.move(Point{0, 0});
    wide.line(Point{10, 10});
    pen.place(Matrix::scaled(1e308, 1e308));
    SAME(trouble_of([&] { pen.fill(wide, white); }), std::optional<Kind>{Kind::malformed});
    SAME(covered(raster.pixels()), 0.0);

    pen.place(Matrix{});

    Stroke fine;

    fine.dashes = {1e-9, 1e-9};
    SAME(trouble_of([&] { pen.stroke(segment(Point{0, 0}, Point{1e6, 0}), white, fine); }), std::optional<Kind>{Kind::full});
    fine.dashes = {0, 1e-5};
    fine.cap = Cap::butt;
    SAME(trouble_of([&] { pen.stroke(segment(Point{0, 0}, Point{1000, 0}), white, fine); }), std::optional<Kind>{Kind::full});
    fine.dashes = {1, 1};
    pen.stroke(segment(Point{0, 0}, Point{60000, 0}), white, fine);
    fine.dashes = {1, 2};
    SAME(trouble_of([&] { pen.stroke(segment(Point{0, 0}, Point{1e300, 0}), white, fine); }), std::optional<Kind>{});
    fine.dashes = {0.35, 0.35};
    SAME(trouble_of([&] { pen.stroke(segment(Point{0, 0}, Point{1e30, 0}), white, fine); }), std::optional<Kind>{});

    Raster tall{tal::media::Size{8, 256}};
    Pen high{tall};
    Path pile;

    for (int one = 0; one < 10000; ++one) pile.rectangle(Rect{Point{1, -1}, 6, 258});

    SAME(trouble_of([&] { high.fill(pile, white); }), std::optional<Kind>{Kind::full});
    SAME(covered(tall.pixels()), 0.0);
    fine.dashes.assign(tal::drawing::most_dashes + 1, 1.0);
    SAME(trouble_of([&] { pen.stroke(segment(Point{0, 0}, Point{4, 0}), white, fine); }), std::optional<Kind>{Kind::full});

    Path enormous;

    enormous.move(Point{0, 0});
    enormous.quad(Point{2, 1e15}, Point{4, 0});
    SAME(trouble_of([&] { pen.fill(enormous, white); }), std::optional<Kind>{});
    fine.dashes = {1, 1};
    SAME(trouble_of([&] { pen.stroke(enormous, white, fine); }), std::optional<Kind>{});
    SAME(trouble_of([] { Raster{tal::media::Size{0, 4}}; }), std::optional<Kind>{Kind::other});
    SAME(trouble_of([] { Raster{tal::media::Size{4000, 4000}}; }), std::optional<Kind>{Kind::other});
    SAME(trouble_of([] { Pattern{ramp(2, 2), true, true, 1}; }), std::optional<Kind>{Kind::other});
}
