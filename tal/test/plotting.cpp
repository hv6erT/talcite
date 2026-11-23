#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "tal/drawing.hpp"
#include "tal/fonts.hpp"
#include "tal/pixels.hpp"
#include "tal/plotting.hpp"
#include "tal/text.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

// Plots (E8): the ticks of scales straight and logarithmic, at round steps, as many as asked, for
// ranges ordinary and extreme; scales fitted to data; labels in the language's way with a true
// minus and a common power of ten; the layout of a plot, its margins and its ticks as their labels
// fit, none of them meeting; a plot on a raster, its frame sharp on its pixels, its series clipped
// and broken where a value is not a number, its legend where the data leave room; and a trace of
// samples from several threads.

namespace {

using tal::Kind;
using tal::drawing::Color;
using tal::drawing::Font;
using tal::drawing::Pen;
using tal::drawing::Point;
using tal::drawing::Raster;
using tal::drawing::Rect;
using tal::plotting::Corner;
using tal::plotting::Layout;
using tal::plotting::Plot;
using tal::plotting::Scale;
using tal::plotting::Series;
using tal::plotting::Spacing;
using tal::plotting::Trace;

// The kind of the trouble `work` throws, none when it throws nothing.
std::optional<Kind> trouble_of(const std::function<void()>& work) {
    try {
        work();
    } catch (const tal::Trouble& why) {
        return why.kind();
    }

    return std::nullopt;
}

// Whether `one` and `other` hold the same values within a billionth of the larger.
bool alike(const std::vector<double>& one, const std::vector<double>& other) {
    if (one.size() != other.size()) return false;

    for (std::size_t at = 0; at < one.size(); ++at)
        if (std::abs(one[at] - other[at]) > 1e-9 * std::max({1e-300, std::abs(one[at]), std::abs(other[at])})) return false;

    return true;
}

// The alpha of the pixel (`x`, `y`) of `pixels`, 0 to 1.
double alpha(const tal::media::Pixels& pixels, int x, int y) {
    const std::size_t at = (static_cast<std::size_t>(y) * pixels.size().width + static_cast<std::size_t>(x)) * 4 + 3;

    return std::to_integer<int>(pixels.stills().front().rgba[at]) / 255.0;
}

}

// Ticks fall on 1, 2 and 5 times a power of ten, no more than asked but as many as the least such
// step gives, every one within the scale and each above the last; tiny and huge ranges alike, those
// narrower than the numbers can tell apart and those among the least numbers, negative ones through
// an exact zero, never a negative one; no more than ten thousand, however many are asked. On a
// logarithmic scale the powers of ten, every second one when fewer are asked; within a decade 1, 2
// and 5 times one.
TRIAL("Plotting: ticks at round steps, as many as asked, for ordinary and extreme ranges, straight and logarithmic") {
    SAME(Scale(0, 10).ticks(6), (std::vector<double>{0, 2, 4, 6, 8, 10}));
    SAME(Scale(0, 10).ticks(11).size(), std::size_t{11});
    SAME(Scale(0, 10).ticks(3), (std::vector<double>{0, 5, 10}));
    OUGHT(alike(Scale(-0.3, 0.7).ticks(6), {-0.2, 0, 0.2, 0.4, 0.6}));
    SAME(Scale(-0.3, 0.7).ticks(6)[1], 0.0);
    OUGHT(!std::signbit(Scale(-0.1, 1).ticks(6).front()));
    OUGHT(alike(Scale(-1e-7, 2e-7).ticks(2), {0, 2e-7}));
    SAME(Scale(0.1, 0.9).ticks(2), (std::vector<double>{0.5}));
    SAME(Scale(0, 1).ticks(1000000).size(), std::size_t{10000});
    OUGHT(alike(Scale(1e-12, 3e-12).ticks(5), {1e-12, 1.5e-12, 2e-12, 2.5e-12, 3e-12}) || alike(Scale(1e-12, 3e-12).ticks(5), {1e-12, 2e-12, 3e-12}));

    for (const auto& [low, high] : {std::pair{0.0, 1e300}, std::pair{-1e300, 1e300}, std::pair{1e-300, 3e-300}, std::pair{1e15, 1e15 + 8}, std::pair{-7.3, -7.1}, std::pair{1e17, 1e17 + 64}, std::pair{0.0, 5e-323}, std::pair{-1e-320, 1e-320}})
        for (const std::size_t most : {2U, 5U, 9U, 38U}) {
            const std::vector<double> ticks = Scale(low, high).ticks(most);

            OUGHT(!ticks.empty() && ticks.size() <= most);

            for (const double one : ticks) OUGHT(std::isfinite(one) && one >= low - 1e-9 * std::abs(low) && one <= high + 1e-9 * std::abs(high));

            for (std::size_t at = 1; at < ticks.size(); ++at) OUGHT(ticks[at - 1] < ticks[at]);

            const std::vector<double> minor = Scale(low, high).minor(most);

            for (const double one : minor) OUGHT(std::isfinite(one) && one >= low - 1e-9 * std::abs(low) && one <= high + 1e-9 * std::abs(high));

            for (std::size_t at = 1; at < minor.size(); ++at) OUGHT(minor[at - 1] < minor[at]);
        }

    OUGHT(alike(Scale(1, 1e6, Spacing::logarithmic).ticks(10), {1, 10, 100, 1e3, 1e4, 1e5, 1e6}));
    OUGHT(alike(Scale(1, 1e6, Spacing::logarithmic).ticks(4), {1, 100, 1e4, 1e6}));
    OUGHT(alike(Scale(2, 9, Spacing::logarithmic).ticks(5), {2, 5}));
    OUGHT(alike(Scale(1, 9, Spacing::logarithmic).ticks(3), {1, 2, 5}));
    OUGHT(Scale(1, 9, Spacing::logarithmic).ticks(2).size() <= 2);
    OUGHT(alike(Scale(1, 100, Spacing::logarithmic).minor(10), {2, 3, 4, 5, 6, 7, 8, 9, 20, 30, 40, 50, 60, 70, 80, 90}));
    OUGHT(alike(Scale(0, 4).minor(3), {0.5, 1, 1.5, 2.5, 3, 3.5}));
    OUGHT(alike(Scale(0, 1).minor(6), {0.05, 0.1, 0.15, 0.25, 0.3, 0.35, 0.45, 0.5, 0.55, 0.65, 0.7, 0.75, 0.85, 0.9, 0.95}));
}

// A scale fitted to data widens to the ticks around them; a single value gets a tenth of itself
// each way, none at all 0 to 1; what is not finite is left out, and so is what is not positive on
// a logarithmic scale. The widest data and the least numbers still give scales, placed across
// without overflow. Ends that are not finite or not in order, a logarithmic scale from zero, are
// malformed.
TRIAL("Plotting: scales fitted to data, around one value or none; ends malformed") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double endless = std::numeric_limits<double>::infinity();

    SAME(Scale::fitted(std::vector<double>{1.3, 7.9}), (Scale(0, 8)));
    SAME(Scale::fitted(std::vector<double>{-3.2, 2, nan, endless}), (Scale(-4, 2)));
    SAME(Scale::fitted(std::vector<double>{5}), (Scale(4.5, 5.5)));
    SAME(Scale::fitted(std::vector<double>{0, 0}), (Scale(-1, 1)));
    SAME(Scale::fitted(std::vector<double>{}), (Scale(0, 1)));
    SAME(Scale::fitted(std::vector<double>{3, 450, -2, 0}, Spacing::logarithmic), (Scale(1, 1000, Spacing::logarithmic)));
    SAME(Scale::fitted(std::vector<double>{10}, Spacing::logarithmic), (Scale(10, 100, Spacing::logarithmic)));
    SAME(Scale::fitted(std::vector<double>{-1}, Spacing::logarithmic), (Scale(1, 10, Spacing::logarithmic)));
    const Scale widest = Scale::fitted(std::vector<double>{-1.7e308, 1.7e308});
    const double least = std::numeric_limits<double>::denorm_min();

    SAME(widest, Scale(-1.7e308, 1.7e308));
    SAME(widest.placed(-1.7e308, 0, 100), 0.0);
    SAME(widest.placed(0, 0, 100), 50.0);
    SAME(widest.placed(1.7e308, 0, 100), 100.0);
    OUGHT(widest.ticks(5).size() >= 3);
    SAME(Scale::fitted(std::vector<double>{1.7e308}).high(), std::numeric_limits<double>::max());
    SAME(Scale::fitted(std::vector<double>{least}).low(), 0.0);
    OUGHT(Scale::fitted(std::vector<double>{least}).high() >= least && Scale::fitted(std::vector<double>{least}).high() < 1e-320);
    SAME(Scale::fitted(std::vector<double>{least}, Spacing::logarithmic).low(), least);
    SAME(Scale::fitted(std::vector<double>{least, 1}, Spacing::logarithmic), Scale(least, 1, Spacing::logarithmic));
    OUGHT(Scale::fitted(std::vector<double>{14 * least}).ticks(38).size() <= 38);
    OUGHT(Scale::fitted(std::vector<double>{20 * least}).high() >= 20 * least);

    SAME(Scale(0, 10).placed(2.5, 100, 200), 125.0);
    SAME(Scale(0, 10).placed(2.5, 200, 100), 175.0);
    SAME(Scale(1, 100, Spacing::logarithmic).placed(10, 0, 50), 25.0);
    OUGHT(std::isnan(Scale(1, 100, Spacing::logarithmic).placed(0, 0, 50)));

    SAME(trouble_of([] { (void)Scale(1, 1); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([] { (void)Scale(2, 1); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([&] { (void)Scale(nan, 1); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([&] { (void)Scale(0, endless); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([] { (void)Scale(0, 10, Spacing::logarithmic); }), std::optional<Kind>{Kind::malformed});
}

// Labels are written as the language writes numbers, with the decimals of the step and a true
// minus; large or small values share a power of ten, a multiple of three; a logarithmic scale
// writes its powers of ten with the exponent raised.
TRIAL("Plotting: labels in the language's way, a true minus, a common power of ten, raised exponents") {
    const tal::text::Language polish{"pl-PL"};
    const tal::text::Language english{"en-US"};
    const std::vector<double> halves{-0.5, 0, 0.5, 1};

    SAME(tal::plotting::labelled(Scale(-1, 1), halves, polish).said, (std::vector<std::string>{"−0,5", "0,0", "0,5", "1,0"}));
    SAME(tal::plotting::labelled(Scale(-1, 1), halves, english).said, (std::vector<std::string>{"−0.5", "0.0", "0.5", "1.0"}));
    SAME(tal::plotting::labelled(Scale(-1, 1), halves, english).power, 0);

    const std::vector<double> large{0, 2e5, 4e5};
    const tal::plotting::Labels thousands = tal::plotting::labelled(Scale(0, 4e5), large, english);

    SAME(thousands.power, 3);
    SAME(thousands.said, (std::vector<std::string>{"0", "200", "400"}));

    const std::vector<double> small{0, 2e-5, 4e-5};
    const tal::plotting::Labels millionths = tal::plotting::labelled(Scale(0, 4e-5), small, english);

    SAME(millionths.power, -6);
    SAME(millionths.said, (std::vector<std::string>{"0", "20", "40"}));

    const std::vector<double> powers{1, 10, 1000, 0.01};

    SAME(tal::plotting::labelled(Scale(0.01, 1000, Spacing::logarithmic), powers, english).said, (std::vector<std::string>{"10⁰", "10¹", "10³", "10⁻²"}));
}

// A plot lays itself out in its rectangle: the data begin right of the widest label of y, its
// bottom axis takes as many ticks as their labels fit side by side, a narrow plot fewer, and no two
// labels of x come within a letter, nor the last beyond the plot, at any width; the ticks of y
// stand two and a half letters apart at any height; ticks fall within the scales, at least two on
// each axis however small (round ones where any two fit, else the ends of the scale), no more than
// two hundred however large; the legend goes to a corner the data leave free, or where it is asked,
// or nowhere when nothing has a name.
TRIAL("Plotting: the layout leaves room for the labels, as many ticks as fit, none meeting; the legend where the data leave room") {
    const Font font = Font::builtin();
    Plot plot;

    plot.x.title = "t [s]";
    plot.y.title = "E [J]";
    plot.y.scale = Scale(-123456, 123456);
    plot.x.scale = Scale(0, 1000);
    plot.letters = 12;

    const Layout wide = plot.laid(font, Rect{Point{0, 0}, 600, 300});
    const Layout narrow = plot.laid(font, Rect{Point{0, 0}, 160, 300});

    OUGHT(wide.data.at.x > font.measured("−100", 12).wide);
    OUGHT(wide.data.wide > 400 && wide.data.high > 200);
    OUGHT(narrow.x_ticks.size() < wide.x_ticks.size() && narrow.x_ticks.size() >= 2);

    for (const Layout* one : {&wide, &narrow}) {
        const tal::plotting::Labels labels = tal::plotting::labelled(one->x, one->x_ticks, tal::text::current());

        for (std::size_t at = 0; at + 1 < one->x_ticks.size(); ++at) {
            const double here = one->x.placed(one->x_ticks[at], one->data.at.x, one->data.at.x + one->data.wide);
            const double next = one->x.placed(one->x_ticks[at + 1], one->data.at.x, one->data.at.x + one->data.wide);

            OUGHT(here + font.measured(labels.said[at], 12).wide / 2 < next - font.measured(labels.said[at + 1], 12).wide / 2);
        }

        for (const double tick : one->x_ticks) OUGHT(tick >= 0 && tick <= 1000);
        for (const double tick : one->y_ticks) OUGHT(tick >= -123456 && tick <= 123456);
    }

    for (const Scale& across : {Scale(0, 1000), Scale(-0.00123, 0.00456), Scale(0, 123456789), Scale(-7.25, 3.5)})
        for (double width = 90; width <= 800; width += 1) {
            Plot swept;

            swept.x.scale = across;
            swept.y.scale = Scale(0, 1);

            const Layout laid = swept.laid(font, Rect{Point{0, 0}, width, 200});
            const tal::plotting::Labels labels = tal::plotting::labelled(laid.x, laid.x_ticks, tal::text::current());
            const double last = laid.x.placed(laid.x_ticks.back(), laid.data.at.x, laid.data.at.x + laid.data.wide);

            OUGHT(last + font.measured(labels.said.back(), 12).wide / 2 <= width);

            for (std::size_t at = 0; at + 1 < laid.x_ticks.size(); ++at) {
                const double here = laid.x.placed(laid.x_ticks[at], laid.data.at.x, laid.data.at.x + laid.data.wide);
                const double next = laid.x.placed(laid.x_ticks[at + 1], laid.data.at.x, laid.data.at.x + laid.data.wide);

                if (laid.x_ticks.size() > 2) OUGHT(here + font.measured(labels.said[at], 12).wide / 2 + 12 - 1e-9 <= next - font.measured(labels.said[at + 1], 12).wide / 2);
            }
        }

    for (double height = 40; height <= 400; height += 9) {
        Plot tall;

        tall.y.scale = Scale(-123456, 123456);

        const Layout laid = tall.laid(font, Rect{Point{0, 0}, 300, height});

        OUGHT(laid.y_ticks.size() >= 2);

        for (std::size_t at = 0; laid.y_ticks.size() > 2 && at + 1 < laid.y_ticks.size(); ++at) OUGHT(laid.y.placed(laid.y_ticks[at + 1], 0, laid.data.high) - laid.y.placed(laid.y_ticks[at], 0, laid.data.high) >= 30 - 1e-9);
    }

    const Layout endless = plot.laid(font, Rect{Point{0, 0}, 1e40, 1e40});
    Plot fine;

    fine.x.scale = Scale(1e17, 1e17 + 64);
    fine.y.scale = Scale(-1e-320, 1e-320);

    for (const double side : {100.0, 400.0, 1e6}) {
        const Layout laid = fine.laid(font, Rect{Point{0, 0}, side, side});

        OUGHT(laid.x_ticks.size() >= 2 && laid.x_ticks.size() <= 200 && laid.y_ticks.size() >= 2 && laid.y_ticks.size() <= 200);

        for (std::size_t at = 1; at < laid.x_ticks.size(); ++at) OUGHT(laid.x_ticks[at - 1] < laid.x_ticks[at]);

        for (std::size_t at = 1; at < laid.y_ticks.size(); ++at) OUGHT(laid.y_ticks[at - 1] < laid.y_ticks[at]);
    }

    OUGHT(endless.x_ticks.size() <= 200 && endless.y_ticks.size() <= 200 && endless.x_ticks.size() >= 2 && endless.y_ticks.size() >= 2);

    SAME(wide.legend, Corner::none);

    Plot low;

    low.y.scale = Scale(0.1, 0.9);
    low.x.scale = Scale(0.1, 0.9);
    OUGHT(alike(low.laid(font, Rect{Point{0, 0}, 60, 40}).y_ticks, {0.2, 0.8}) && low.laid(font, Rect{Point{0, 0}, 60, 40}).x_ticks.size() >= 2);

    Series rising;

    rising.name = "rising";

    for (int step = 0; step <= 100; ++step) rising.points.push_back(Point{step * 10.0, -123456 + step * 2469.12});

    plot.series.push_back(rising);

    SAME(plot.laid(font, Rect{Point{0, 0}, 600, 300}).legend, Corner::upper_left);

    plot.series.front().points.clear();

    for (int step = 0; step <= 100; ++step) plot.series.front().points.push_back(Point{step * 10.0, 123456 - step * 2469.12});

    SAME(plot.laid(font, Rect{Point{0, 0}, 600, 300}).legend, Corner::upper_right);

    plot.legend = Corner::lower_left;
    SAME(plot.laid(font, Rect{Point{0, 0}, 600, 300}).legend, Corner::lower_left);
    plot.legend = Corner::none;
    SAME(plot.laid(font, Rect{Point{0, 0}, 600, 300}).legend, Corner::none);
}

// On a raster: the frame stands whole on pixels at the edges of the data; a line two pixels wide
// at the middle of y covers each pixel of a column as much as it lies over it; a value that is not
// a number breaks the line, so the pixels between the points on either side stay clear; a series
// going beyond the scale is clipped to the data. A line or mark of any width or size buries the
// data, drawn at once; labels of ticks whose step overflows still come.
TRIAL("Plotting: a plot on a raster, its frame sharp, its line broken where a value is not a number and clipped to the data") {
    const Font font = Font::builtin();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Raster raster{tal::media::Size{240, 160}};
    Pen pen{raster};
    Plot plot;
    Series line;

    plot.x.scale = Scale(0, 10);
    plot.y.scale = Scale(0, 10);
    plot.legend = Corner::none;
    line.color = Color{1, 0, 0, 1};
    line.width = 2;
    line.points = {Point{0, 5}, Point{4, 5}, Point{nan, nan}, Point{6, 5}, Point{10, 5}, Point{10, 30}};
    plot.series.push_back(line);

    const Layout laid = plot.drawn(pen, font, Rect{Point{0, 0}, 240, 160});
    const tal::media::Pixels pixels = raster.pixels();
    const int left = static_cast<int>(std::round(laid.data.at.x));
    const int top = static_cast<int>(std::round(laid.data.at.y));
    const int right = static_cast<int>(std::round(laid.data.at.x + laid.data.wide)) - 1;
    const int bottom = static_cast<int>(std::round(laid.data.at.y + laid.data.high)) - 1;

    const double level = laid.y.placed(5, laid.data.at.y + laid.data.high, laid.data.at.y);

    for (int y = top + 2; y < bottom - 2; ++y) {
        SAME(alpha(pixels, left, y), 1.0);
        SAME(alpha(pixels, right, y), 1.0);
        OUGHT(std::abs(alpha(pixels, left + 2, y) - std::clamp(std::min(y + 1.0, level + 1) - std::max(y + 0.0, level - 1), 0.0, 1.0)) < 0.02);
    }

    const int middle = static_cast<int>(laid.y.placed(5, laid.data.at.y + laid.data.high, laid.data.at.y));
    const int gap_x = static_cast<int>(laid.x.placed(5, laid.data.at.x, laid.data.at.x + laid.data.wide));
    const int line_x = static_cast<int>(laid.x.placed(2, laid.data.at.x, laid.data.at.x + laid.data.wide));

    OUGHT(alpha(pixels, line_x, middle) > 0.9);
    SAME(alpha(pixels, gap_x, middle), 0.0);

    for (int y = 0; y < top - 1; ++y) SAME(alpha(pixels, right - 1, y), 0.0);

    Raster wide_raster{tal::media::Size{240, 160}};
    Pen wide_pen{wide_raster};

    plot.series.front().width = 1e15;
    plot.series.front().mark = tal::plotting::Mark::circle;
    plot.series.front().size = 1e15;
    plot.series.front().name = "wide";
    plot.legend = Corner::upper_left;

    const Layout buried = plot.drawn(wide_pen, font, Rect{Point{0, 0}, 240, 160});

    SAME(alpha(wide_raster.pixels(), static_cast<int>(buried.data.at.x + buried.data.wide / 2), static_cast<int>(buried.data.at.y + buried.data.high - 3)), 1.0);
    SAME(alpha(wide_raster.pixels(), 1, 1), 0.0);
    SAME(alpha(wide_raster.pixels(), 238, 1), 0.0);
    SAME(alpha(wide_raster.pixels(), static_cast<int>(buried.data.at.x + 2 * 0.4 * plot.letters + plot.letters), 1), 0.0);
    SAME(tal::plotting::labelled(Scale(-1.7e308, 1.7e308), std::vector<double>{-1.7e308, 1.7e308}, tal::text::Language{"en-US"}).said.size(), std::size_t{2});
}

// The grid lies on single pixels at the ticks, faint, nothing between them; a mark stands whole
// around its point without a line; a point that is not a number gets none; the titles are
// written, x below the labels, y turned up at the left, the plot's above the data.
TRIAL("Plotting: grid lines on single pixels at the ticks, marks around their points, the titles where they belong") {
    const Font font = Font::builtin();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Raster raster{tal::media::Size{300, 200}};
    Pen pen{raster};
    Plot plot;
    Series marks;

    plot.title = "Plot";
    plot.x.title = "x";
    plot.y.title = "y";
    plot.x.scale = Scale(0, 10);
    plot.y.scale = Scale(0, 10);
    plot.x.grid = true;
    plot.legend = Corner::none;
    marks.line = false;
    marks.mark = tal::plotting::Mark::square;
    marks.size = 8;
    marks.color = Color{0, 1, 0, 1};
    marks.points = {Point{3, 7}, Point{nan, 5}};
    plot.series.push_back(marks);

    const Layout laid = plot.drawn(pen, font, Rect{Point{0, 0}, 300, 200});
    const tal::media::Pixels pixels = raster.pixels();
    const double bottom = laid.data.at.y + laid.data.high;
    const int row = static_cast<int>(laid.y.placed(1.5, bottom, laid.data.at.y));
    const auto across = [&](double value) { return laid.x.placed(value, laid.data.at.x, laid.data.at.x + laid.data.wide); };
    const int grid = static_cast<int>(std::floor(across(laid.x_ticks[1]) - 0.5) + 0.5);

    OUGHT(alpha(pixels, grid, row) > 0.1 && alpha(pixels, grid, row) < 0.3);
    SAME(alpha(pixels, grid - 1, row), 0.0);
    SAME(alpha(pixels, grid + 1, row), 0.0);

    const Point mark{across(3), laid.y.placed(7, bottom, laid.data.at.y)};

    for (int dx = -3; dx <= 3; ++dx)
        for (int dy = -3; dy <= 3; ++dy) SAME(alpha(pixels, static_cast<int>(mark.x) + dx, static_cast<int>(mark.y) + dy), 1.0);

    SAME(alpha(pixels, static_cast<int>(mark.x) + 7, static_cast<int>(mark.y)), 0.0);

    const auto inked = [&](int x0, int y0, int x1, int y1) {
        int many = 0;

        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) many += alpha(pixels, x, y) > 0.5 ? 1 : 0;

        return many;
    };

    OUGHT(inked(0, 0, 300, static_cast<int>(laid.data.at.y) - 1) > 10);
    OUGHT(inked(0, 185, 300, 200) > 5);
    OUGHT(inked(0, 40, 14, 160) > 5);
}

// A line of thousands of points is drawn with only the segments it needs, its pixels within a
// tenth of a pixel's worth of the line through every point (a stroke of all of them, clipped the
// same, compared inside the data); a lone spike among flat points keeps its tip, a line its last
// turn and one going back over itself the stretch it covered.
TRIAL("Plotting: a dense line drawn with fewer segments looks as the line through every point, its spikes kept") {
    const Font font = Font::builtin();
    Plot plot;
    Series wave;

    plot.x.scale = Scale(0, 1);
    plot.y.scale = Scale(-1.5, 1.5);
    plot.legend = Corner::none;
    wave.color = Color{1, 1, 1, 1};

    for (int step = 0; step <= 4000; ++step) {
        const double x = step / 4000.0;

        wave.points.push_back(Point{x, std::sin(x * 40) * std::cos(x * 7) + (step == 2000 ? 0.4 : 0.0)});
    }

    plot.series.push_back(wave);

    Raster thinned{tal::media::Size{400, 200}};
    Pen thin_pen{thinned};
    const Layout laid = plot.drawn(thin_pen, font, Rect{Point{0, 0}, 400, 200});

    plot.series.front().line = false;

    Raster whole{tal::media::Size{400, 200}};
    Pen whole_pen{whole};

    (void)plot.drawn(whole_pen, font, Rect{Point{0, 0}, 400, 200});

    tal::drawing::Path every;
    tal::drawing::Path inside;
    tal::drawing::Stroke style;

    for (std::size_t at = 0; at < wave.points.size(); ++at) {
        const Point placed{laid.x.placed(wave.points[at].x, laid.data.at.x, laid.data.at.x + laid.data.wide), laid.y.placed(wave.points[at].y, laid.data.at.y + laid.data.high, laid.data.at.y)};

        if (at == 0) every.move(placed);
        else every.line(placed);
    }

    style.width = wave.width;
    style.join = tal::drawing::Join::round;
    style.cap = tal::drawing::Cap::round;
    inside.rectangle(laid.data);
    whole_pen.save();
    whole_pen.clip(inside);
    whole_pen.stroke(every, Color{1, 1, 1, 1}, style);
    whole_pen.restore();

    Raster turns{tal::media::Size{200, 200}};
    Pen turns_pen{turns};
    Plot turning;
    Series corner;
    Series back;

    turning.x.scale = Scale(0, 1);
    turning.y.scale = Scale(0, 1);
    turning.legend = Corner::none;
    corner.points = {Point{0, 0.2}, Point{0.5, 0.2}, Point{0.5, 0.8}};
    back.points = {Point{0.1, 0.9}, Point{0.9, 0.9}, Point{0.3, 0.9}};
    turning.series = {corner, back};

    const Layout turned = turning.drawn(turns_pen, font, Rect{Point{0, 0}, 200, 200});
    const tal::media::Pixels turned_pixels = turns.pixels();
    const auto inked = [&](double x, double y) { return alpha(turned_pixels, static_cast<int>(turned.x.placed(x, turned.data.at.x, turned.data.at.x + turned.data.wide)), static_cast<int>(turned.y.placed(y, turned.data.at.y + turned.data.high, turned.data.at.y))) > 0.3; };

    OUGHT(inked(0.5, 0.5) && inked(0.8, 0.9));

    const tal::media::Pixels one = thinned.pixels();
    const tal::media::Pixels other = whole.pixels();
    double most = 0;

    for (int y = static_cast<int>(laid.data.at.y) + 2; y < static_cast<int>(laid.data.at.y + laid.data.high) - 2; ++y)
        for (int x = static_cast<int>(laid.data.at.x) + 2; x < static_cast<int>(laid.data.at.x + laid.data.wide) - 2; ++x) most = std::max(most, std::abs(alpha(one, x, y) - alpha(other, x, y)));

    OUGHT(most < 0.12);

    const double tip = laid.y.placed(std::sin(0.5 * 40) * std::cos(0.5 * 7) + 0.4, laid.data.at.y + laid.data.high, laid.data.at.y);

    OUGHT(alpha(one, static_cast<int>(laid.x.placed(0.5, laid.data.at.x, laid.data.at.x + laid.data.wide)), static_cast<int>(tip)) > 0.3);
}

// A trace keeps the last window of samples, no more than its room; a time before the last starts
// it anew; its span is the window ending at the last sample, the least step below it where the
// window is lost against the time, from the least number where it reaches beyond. Two threads
// adding while a third reads leave every copy in order of time. A window that is not positive, or
// room for fewer than two samples, is malformed.
TRIAL("Plotting: a trace keeps the last window, within its room, anew after time goes back; from several threads in order") {
    Trace trace{2.0, 1000};

    OUGHT(!trace.span().has_value());

    for (int step = 0; step <= 50; ++step) trace.add(step * 0.1, step);

    const std::vector<Point> kept = trace.points();

    SAME(kept.front().x >= 3.0 - 1e-9, true);
    SAME(kept.back(), (Point{5.0, 50}));
    SAME(trace.span(), (std::optional<Scale>{Scale(3.0, 5.0)}));

    Trace short_one{100.0, 3};

    for (int step = 0; step < 10; ++step) short_one.add(step, step);

    SAME(short_one.points().size(), std::size_t{3});

    short_one.add(1, 7);
    SAME(short_one.points(), (std::vector<Point>{Point{1, 7}}));
    short_one.clear();
    OUGHT(short_one.points().empty() && !short_one.span().has_value());

    Trace late{1.0};

    late.add(1e17, 1);
    SAME(late.span()->high(), 1e17);
    OUGHT(late.span()->low() < 1e17);
    late.add(std::numeric_limits<double>::lowest(), 2);
    SAME(late.span()->low(), std::numeric_limits<double>::lowest());
    OUGHT(late.span()->high() > std::numeric_limits<double>::lowest());

    Trace vast{1e308};

    vast.add(-1.7e308, 1);
    SAME(vast.span(), (std::optional<Scale>{Scale(std::numeric_limits<double>::lowest(), -1.7e308)}));

    Trace shared{1e9, 100000};
    std::atomic<bool> going{true};
    std::atomic<int> ordered{0};
    std::thread reader([&] {
        while (going.load()) {
            const std::vector<Point> copy = shared.points();

            for (std::size_t at = 1; at < copy.size(); ++at)
                if (copy[at].x < copy[at - 1].x) ordered.store(1);
        }
    });
    std::atomic<int> clock{0};
    const auto adding = [&] {
        for (int step = 0; step < 2000; ++step) {
            const int now = clock.fetch_add(1);

            shared.add(now, now);
        }
    };
    std::thread one(adding);
    std::thread two(adding);

    one.join();
    two.join();
    going.store(false);
    reader.join();

    SAME(ordered.load(), 0);
    OUGHT(shared.points().size() <= 4000);

    SAME(trouble_of([] { Trace{0}; }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([] { Trace{1, 1}; }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([] { Trace{std::numeric_limits<double>::infinity()}; }), std::optional<Kind>{Kind::malformed});
}
