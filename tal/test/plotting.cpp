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
using tal::plotting::Bins;
using tal::plotting::Binning;
using tal::plotting::Corner;
using tal::plotting::Grouping;
using tal::plotting::Layout;
using tal::plotting::Nearest;
using tal::plotting::Plot;
using tal::plotting::Readout;
using tal::plotting::Scale;
using tal::plotting::Series;
using tal::plotting::Shape;
using tal::plotting::Spacing;
using tal::plotting::Tally;
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

// The channel `channel` (0 red, 1 green, 2 blue) of the pixel (`x`, `y`) of `pixels`, 0 to 1.
double tone(const tal::media::Pixels& pixels, int x, int y, int channel) {
    const std::size_t at = (static_cast<std::size_t>(y) * pixels.size().width + static_cast<std::size_t>(x)) * 4 + static_cast<std::size_t>(channel);

    return std::to_integer<int>(pixels.stills().front().rgba[at]) / 255.0;
}

// The columns of `pixels` from `from` to `to` (exclusive) in which any pixel of the rows from
// `top` to `bottom` (exclusive) is inked.
int inked_columns(const tal::media::Pixels& pixels, int from, int to, int top, int bottom) {
    int made = 0;

    for (int x = from; x < to; ++x) {
        bool any = false;

        for (int y = top; y < bottom && !any; ++y) any = alpha(pixels, x, y) > 0.1;

        made += any ? 1 : 0;
    }

    return made;
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

// A pixel gives back the value placed there, on straight, logarithmic and the widest scales, and
// beyond the ends as far as the numbers go. A zoom draws both ends towards a value by its factor
// (by the logarithms on a logarithmic scale), out for a factor below one; a shift moves the values
// by so many pixels, either way along the axis. Factors and values that are not numbers, and ends
// that leave the numbers or meet, are malformed.
TRIAL("Plotting: values under pixels as the reverse of placing them; scales zoomed about a value and shifted by pixels") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const Scale straight(-3, 7);
    const Scale logarithmic(0.01, 1000, Spacing::logarithmic);
    const Scale widest(-1.7e308, 1.7e308);

    for (const double value : {-3.0, -1.25, 0.0, 2.5, 7.0})
        OUGHT(std::abs(straight.valued(straight.placed(value, 40, 340), 40, 340) - value) < 1e-12);

    for (const double value : {0.01, 0.5, 3.0, 1000.0})
        OUGHT(std::abs(logarithmic.valued(logarithmic.placed(value, 200, 0), 200, 0) / value - 1) < 1e-12);

    SAME(straight.valued(40, 40, 340), -3.0);
    SAME(straight.valued(340, 40, 340), 7.0);
    SAME(straight.valued(640, 40, 340), 17.0);
    SAME(widest.valued(0, 0, 100), -1.7e308);
    SAME(widest.valued(50, 0, 100), 0.0);
    SAME(widest.valued(100, 0, 100), 1.7e308);
    OUGHT(std::isinf(widest.valued(200, 0, 100)));

    SAME(Scale(0, 10).zoomed(5, 2), Scale(2.5, 7.5));
    SAME(Scale(0, 10).zoomed(0, 4), Scale(0, 2.5));
    SAME(Scale(0, 10).zoomed(5, 0.5), Scale(-5, 15));
    OUGHT(std::abs(Scale(1, 100, Spacing::logarithmic).zoomed(10, 2).low() - std::sqrt(10.0)) < 1e-12);
    OUGHT(std::abs(Scale(1, 100, Spacing::logarithmic).zoomed(10, 2).high() - std::pow(10.0, 1.5)) < 1e-9);
    SAME(Scale(-1.7e308, 1.7e308).zoomed(0, 2), Scale(-0.85e308, 0.85e308));
    SAME(Scale(-1.7e308, 1.7e308).zoomed(1.7e308, 2), Scale(0, 1.7e308));
    SAME(Scale(0, 10).shifted(30, 0, 300), Scale(-1, 9));
    SAME(Scale(0, 10).shifted(10, 100, 0), Scale(1, 11));
    OUGHT(std::abs(Scale(1, 100, Spacing::logarithmic).shifted(50, 0, 100).low() - 0.1) < 1e-12);

    for (const double factor : {0.0, -2.0, nan, std::numeric_limits<double>::infinity()})
        SAME(trouble_of([&] { (void)Scale(0, 10).zoomed(5, factor); }), std::optional<Kind>{Kind::malformed});

    SAME(trouble_of([&] { (void)Scale(0, 10).zoomed(nan, 2); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([] { (void)Scale(1, 10, Spacing::logarithmic).zoomed(0, 2); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([] { (void)Scale(-1e308, 1e308).zoomed(0, 0.1); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([] { (void)Scale(0, 1).zoomed(0.5, 1e300); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([] { (void)Scale(0, 1e308).shifted(-200, 0, 100); }), std::optional<Kind>{Kind::malformed});
}

// The values under a pixel of the data, none outside it, the ends exact on its edges. The point
// nearest a pixel among all series, as the layout places them: points that are not numbers left
// out (a series may begin with one), none farther than asked, the first of equals.
TRIAL("Plotting: the values under a pixel of the data and the point of the series nearest it") {
    const Font font = Font::builtin();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    Plot plot;
    Series one;
    Series other;

    plot.x.scale = Scale(0, 10);
    plot.y.scale = Scale(0, 10);
    one.points = {Point{1, 1}, Point{nan, nan}, Point{5, 5}};
    other.points = {Point{nan, nan}, Point{5, 5}, Point{9, 2}};
    plot.series = {one, other};

    const Layout laid = plot.laid(font, Rect{Point{0, 0}, 300, 200});
    const double left = laid.data.at.x;
    const double right = laid.data.at.x + laid.data.wide;
    const double top = laid.data.at.y;
    const double bottom = laid.data.at.y + laid.data.high;

    SAME(laid.at(Point{left, bottom}), (std::optional<Point>{Point{0, 10 * 0.0}}));
    SAME(laid.at(Point{right, top}), (std::optional<Point>{Point{10, 10}}));
    OUGHT(std::abs(laid.at(Point{(left + right) / 2, (top + bottom) / 2})->x - 5) < 1e-12);
    SAME(laid.at(Point{left - 1, top}), std::optional<Point>{});
    SAME(laid.at(Point{left, bottom + 1}), std::optional<Point>{});
    SAME(laid.at(Point{nan, top}), std::optional<Point>{});

    const Point five{laid.x.placed(5, left, right), laid.y.placed(5, bottom, top)};
    const Point nine{laid.x.placed(9, left, right), laid.y.placed(2, bottom, top)};

    SAME(laid.nearest(plot, five, 1), (std::optional<Nearest>{Nearest{0, 2, 0}}));
    SAME(laid.nearest(plot, Point{nine.x + 3, nine.y + 4}, 10), (std::optional<Nearest>{Nearest{1, 2, 5}}));
    SAME(laid.nearest(plot, Point{nine.x + 3, nine.y + 4}, 4.9), std::optional<Nearest>{});
    SAME(laid.nearest(Plot{}, five, 100), std::optional<Nearest>{});
}

// A cursor over the data draws a faint line down it, a ring in its color around each point read
// and a box of values beside the cursor, left of it near the right edge, wider for more digits;
// samples along x are read at the cursor's x, every series, as `Readout::across` reads them;
// `Readout::nearest` reads only the one nearest point of all; a series that turns back along x is
// read at its point nearest the cursor. No line when not wanted; a cursor outside the data draws
// nothing more.
TRIAL("Plotting: a cursor over the data reads the series under it in a box beside it, as asked") {
    const Font font = Font::builtin();
    Plot plot;
    Series red;
    Series green;

    plot.x.scale = Scale(0, 10);
    plot.y.scale = Scale(0, 10);
    plot.legend = Corner::none;
    red.color = Color{1, 0, 0, 1};
    red.line = false;
    green.color = Color{0, 1, 0, 1};
    green.line = false;

    for (int step = 0; step <= 10; ++step) {
        red.points.push_back(Point{static_cast<double>(step), 2});
        green.points.push_back(Point{static_cast<double>(step), 6});
    }

    plot.series = {red, green};

    const Rect area{Point{0, 0}, 400, 240};
    const auto drawn = [&](const Plot& one) {
        Raster raster{tal::media::Size{400, 240}};
        Pen pen{raster};

        (void)one.drawn(pen, font, area);

        return raster.pixels();
    };
    const Layout laid = plot.laid(font, area);
    const double left = laid.data.at.x;
    const double right = laid.data.at.x + laid.data.wide;
    const double top = laid.data.at.y;
    const double bottom = laid.data.at.y + laid.data.high;
    const auto pixel = [&](double x, double y) { return Point{laid.x.placed(x, left, right), laid.y.placed(y, bottom, top)}; };
    const tal::media::Pixels plain = drawn(plot);
    const int rows_from = static_cast<int>(top) + 2;
    const int rows_to = static_cast<int>(bottom) - 2;

    plot.cursor = Point{laid.x.placed(3, left, right) + 0.5, (top + bottom) / 2};

    const tal::media::Pixels read = drawn(plot);
    const int column = static_cast<int>(std::floor(plot.cursor->x));
    const Point red_three = pixel(3, 2);
    const Point green_three = pixel(3, 6);
    const double ring = plot.letters * 0.35;

    OUGHT(alpha(read, column, static_cast<int>(bottom) - 3) > 0.3 && alpha(plain, column, static_cast<int>(bottom) - 3) == 0);
    OUGHT(tone(read, static_cast<int>(red_three.x + ring), static_cast<int>(red_three.y), 0) > 0.3);
    OUGHT(tone(read, static_cast<int>(green_three.x + ring), static_cast<int>(green_three.y), 1) > 0.3);
    OUGHT(inked_columns(read, column + 2, static_cast<int>(right) - 1, rows_from, rows_to) > inked_columns(plain, column + 2, static_cast<int>(right) - 1, rows_from, rows_to) + 20);

    plot.readout = Readout::across;
    OUGHT(drawn(plot).stills().front().rgba == read.stills().front().rgba);

    plot.readout = Readout::nearest;
    plot.cursor = Point{green_three.x + 0.5, green_three.y + 1};

    const tal::media::Pixels nearest = drawn(plot);

    OUGHT(tone(nearest, static_cast<int>(green_three.x + ring), static_cast<int>(green_three.y), 1) > 0.3);
    SAME(tone(nearest, static_cast<int>(red_three.x + ring), static_cast<int>(red_three.y), 0), tone(plain, static_cast<int>(red_three.x + ring), static_cast<int>(red_three.y), 0));

    plot.readout = Readout::automatic;
    plot.cursor = Point{right - 4, (top + bottom) / 2};

    const tal::media::Pixels edge = drawn(plot);

    OUGHT(inked_columns(edge, static_cast<int>(left) + 1, static_cast<int>(right) - 10, rows_from, rows_to) > inked_columns(plain, static_cast<int>(left) + 1, static_cast<int>(right) - 10, rows_from, rows_to) + 20);

    plot.cursor = Point{laid.x.placed(3, left, right) + 0.5, (top + bottom) / 2};
    plot.readout_digits = 0;

    const int narrow = inked_columns(drawn(plot), column + 2, static_cast<int>(right) - 1, rows_from, rows_to);

    plot.readout_digits = 4;

    const int wide = inked_columns(drawn(plot), column + 2, static_cast<int>(right) - 1, rows_from, rows_to);

    OUGHT(wide > narrow + 10);

    plot.cursor_line = false;
    SAME(alpha(drawn(plot), column, static_cast<int>(bottom) - 3), 0.0);

    Plot looped = plot;
    Series back;

    back.color = Color{0, 0, 1, 1};
    back.line = false;
    back.points = {Point{2, 8}, Point{8, 8}, Point{2, 4}};
    looped.series = {back};
    looped.cursor = Point{pixel(2, 4).x + 0.5, pixel(2, 4).y - 3};

    const tal::media::Pixels turned = drawn(looped);

    OUGHT(tone(turned, static_cast<int>(pixel(2, 4).x + ring), static_cast<int>(pixel(2, 4).y), 2) > 0.3);
    SAME(tone(turned, static_cast<int>(pixel(2, 8).x + ring), static_cast<int>(pixel(2, 8).y), 2), 0.0);

    plot.cursor = Point{left - 5, (top + bottom) / 2};
    OUGHT(drawn(plot).stills().front().rgba == plain.stills().front().rgba);
}

// Bins by the rule of Freedman and Diaconis raised to a round width, from a multiple of it, the
// greatest value in the last, values that are not finite left out; by Sturges where the quartiles
// meet, a tenth of the value where all are one, no finer than the least numbers allow, and widened
// while there would be too many; a width or a count asked for, a count around a value where all are
// one. Widths that are not positive finite numbers and counts of none are malformed, too many bins
// asked for full. The bars of a histogram meet, its shares sum to one and its density covers an
// area of one.
TRIAL("Plotting: values binned by Freedman and Diaconis at round widths, by a width or a count asked for; histograms as counts, shares and densities") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<double> hundred;

    for (int one = 1; one <= 100; ++one) hundred.push_back(one);

    SAME(tal::plotting::binned(hundred), (Bins{0, 50, {49, 50, 1}}));

    hundred.push_back(nan);
    hundred.push_back(std::numeric_limits<double>::infinity());
    SAME(tal::plotting::binned(hundred), (Bins{0, 50, {49, 50, 1}}));
    SAME(tal::plotting::binned(std::vector<double>{3, 3, 3}), (Bins{3, 0.5, {3}}));
    SAME(tal::plotting::binned(std::vector<double>{0}), (Bins{0, 1, {1}}));
    SAME(tal::plotting::binned(std::vector<double>{0, 4}), (Bins{0, 5, {2}}));
    SAME(tal::plotting::binned(std::vector<double>{0, 0, 0, 0, 0, 0, 0, 10}), (Bins{0, 5, {7, 0, 1}}));
    SAME(tal::plotting::binned(std::vector<double>{}), Bins{});
    SAME(tal::plotting::binned(std::vector<double>{0.1, 0.3, 0.6}, Binning{.width = 0.25}), (Bins{0, 0.25, {1, 1, 1}}));
    SAME(tal::plotting::binned(std::vector<double>{0, 10}, Binning{.count = 4}), (Bins{0, 2.5, {1, 0, 0, 1}}));
    SAME(tal::plotting::binned(std::vector<double>{5, 5}, Binning{.count = 3}), (Bins{4.25, 0.5, {0, 2, 0}}));

    std::vector<double> outlying;

    for (int one = 0; one < 1000; ++one) outlying.push_back(one);

    outlying.push_back(1e12);

    const Bins widened = tal::plotting::binned(outlying);
    std::size_t all = 0;

    for (const std::size_t one : widened.counts) all += one;

    OUGHT(widened.counts.size() <= (std::size_t{1} << 20) && widened.counts.size() > 1000);
    SAME(all, std::size_t{1001});

    const double least = std::numeric_limits<double>::denorm_min();

    for (const std::vector<double>& tiny : {std::vector<double>{least, 3 * least, 7 * least}, std::vector<double>{0, least}, std::vector<double>{least, 2 * least, 1e-300}}) {
        const Bins kept = tal::plotting::binned(tiny);
        std::size_t counted = 0;

        for (const std::size_t one : kept.counts) counted += one;

        SAME(counted, tiny.size());
        OUGHT(kept.width > 0 && kept.counts.size() <= (std::size_t{1} << 20));
        OUGHT(tal::plotting::binned(tiny, Binning{.count = 5}).width > 0);
    }

    const Bins widest = tal::plotting::binned(std::vector<double>{-1.7e308, 0, 1.7e308});

    all = 0;

    for (const std::size_t one : widest.counts) all += one;

    SAME(all, std::size_t{3});

    for (const double width : {0.0, -1.0, nan, std::numeric_limits<double>::infinity()})
        SAME(trouble_of([&] { (void)tal::plotting::binned(std::vector<double>{1, 2}, Binning{.width = width}); }), std::optional<Kind>{Kind::malformed});

    SAME(trouble_of([] { (void)tal::plotting::binned(std::vector<double>{1, 2}, Binning{.count = 0}); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([] { (void)tal::plotting::binned(std::vector<double>{1, 2}, Binning{.count = (std::size_t{1} << 20) + 1}); }), std::optional<Kind>{Kind::full});
    SAME(trouble_of([] { (void)tal::plotting::binned(std::vector<double>{0, 1}, Binning{.width = 1e-9}); }), std::optional<Kind>{Kind::full});

    const Bins three{0, 0.5, {1, 2, 1}};
    const Series counted = tal::plotting::histogram(three);
    const Series shared = tal::plotting::histogram(three, Tally::share);
    const Series dense = tal::plotting::histogram(three, Tally::density);

    SAME(counted.shape, Shape::bars);
    SAME(counted.bar, std::optional<double>{0.5});
    SAME(counted.points, (std::vector<Point>{Point{0.25, 1}, Point{0.75, 2}, Point{1.25, 1}}));
    SAME(shared.points, (std::vector<Point>{Point{0.25, 0.25}, Point{0.75, 0.5}, Point{1.25, 0.25}}));

    double area = 0;

    for (const Point& one : dense.points) area += one.y * 0.5;

    SAME(area, 1.0);
    SAME(tal::plotting::histogram(Bins{0, 1, {0, 0}}, Tally::density).points, (std::vector<Point>{Point{0.5, 0}, Point{1.5, 0}}));
}

// Bars stand on zero (on the low end of a logarithmic scale) up to their values, as wide as asked;
// several series side by side, one on another (each above the last at the same x) or over one
// another, half seen through; steps turn square at each point; bars that meet leave no seam; bars
// of no width asked for take eight tenths of the distance between them, and no line joins them;
// the fitted scales take in the feet and both sides of the bars; the legend shows bars as a
// square; the readout reads the bar under the cursor, nothing between bars.
TRIAL("Plotting: bars on their feet side by side, stacked or over one another; steps; scales around the bars; the bar under the cursor read") {
    const Font font = Font::builtin();
    const Rect area{Point{0, 0}, 400, 300};
    Plot plot;
    Series one;
    Series two;

    plot.x.scale = Scale(0, 10);
    plot.y.scale = Scale(0, 10);
    plot.legend = Corner::none;
    one.shape = Shape::bars;
    one.bar = 2;
    one.color = Color{1, 0, 0, 1};
    one.points = {Point{5, 4}};
    two.shape = Shape::bars;
    two.bar = 2;
    two.color = Color{0, 1, 0, 1};
    two.points = {Point{5, 3}};

    const auto drawn = [&](const Plot& made) {
        Raster raster{tal::media::Size{400, 300}};
        Pen pen{raster};

        (void)made.drawn(pen, font, area);

        return raster.pixels();
    };
    const Layout laid = plot.laid(font, area);
    const auto pixel = [&](double x, double y) {
        return std::pair{static_cast<int>(laid.x.placed(x, laid.data.at.x, laid.data.at.x + laid.data.wide)), static_cast<int>(laid.y.placed(y, laid.data.at.y + laid.data.high, laid.data.at.y))};
    };
    const auto tone_at = [&](const tal::media::Pixels& pixels, double x, double y, int channel) {
        const auto [px, py] = pixel(x, y);

        return tone(pixels, px, py, channel);
    };
    const auto alpha_at = [&](const tal::media::Pixels& pixels, double x, double y) {
        const auto [px, py] = pixel(x, y);

        return alpha(pixels, px, py);
    };

    plot.series = {one};

    const tal::media::Pixels alone = drawn(plot);

    SAME(alpha_at(alone, 5, 2), 1.0);
    SAME(alpha_at(alone, 4.5, 3.5), 1.0);
    SAME(alpha_at(alone, 5, 5), 0.0);
    SAME(alpha_at(alone, 3.5, 2), 0.0);
    SAME(alpha_at(alone, 6.5, 2), 0.0);

    plot.series = {one, two};

    const tal::media::Pixels side = drawn(plot);

    SAME(tone_at(side, 4.5, 1, 0), 1.0);
    SAME(tone_at(side, 5.5, 1, 1), 1.0);
    SAME(alpha_at(side, 5.5, 3.5), 0.0);

    plot.bars = Grouping::stacked;

    const tal::media::Pixels stacked = drawn(plot);

    SAME(tone_at(stacked, 5, 2, 0), 1.0);
    SAME(tone_at(stacked, 5, 6, 1), 1.0);
    SAME(alpha_at(stacked, 5, 7.5), 0.0);

    plot.bars = Grouping::over;

    const tal::media::Pixels over = drawn(plot);

    OUGHT(tone_at(over, 5, 2, 0) > 0.2 && tone_at(over, 5, 2, 1) > 0.2);
    OUGHT(alpha_at(over, 5, 3.5) > 0.5 && alpha_at(over, 5, 3.5) < 0.7);

    Plot logarithmic = plot;

    logarithmic.bars = Grouping::side;
    logarithmic.y.scale = Scale(1, 1000, Spacing::logarithmic);
    logarithmic.series = {one};
    logarithmic.series.front().points = {Point{5, 100}};

    const Layout log_laid = logarithmic.laid(font, area);
    Raster log_raster{tal::media::Size{400, 300}};
    Pen log_pen{log_raster};

    (void)logarithmic.drawn(log_pen, font, area);
    SAME(alpha(log_raster.pixels(), static_cast<int>(log_laid.x.placed(5, log_laid.data.at.x, log_laid.data.at.x + log_laid.data.wide)), static_cast<int>(log_laid.data.at.y + log_laid.data.high) - 3), 1.0);

    Plot stairs = plot;
    Series step;

    step.shape = Shape::steps;
    step.width = 2;
    step.points = {Point{0, 1}, Point{5, 6}, Point{10, 1}};
    stairs.series = {step};

    const tal::media::Pixels stepped = drawn(stairs);

    OUGHT(alpha_at(stepped, 2.5, 1) > 0.5);
    OUGHT(alpha_at(stepped, 7.5, 6) > 0.5);
    SAME(alpha_at(stepped, 2.5, 3.5), 0.0);

    Plot seams;
    Series meeting;

    seams.x.scale = Scale(0, 3.3);
    seams.y.scale = Scale(0, 1);
    seams.legend = Corner::none;
    meeting.shape = Shape::bars;
    meeting.bar = 1;
    meeting.points = {Point{0.5, 1}, Point{1.5, 1}, Point{2.5, 1}};
    seams.series = {meeting};

    const Layout seams_laid = seams.laid(font, area);
    Raster seams_raster{tal::media::Size{400, 300}};
    Pen seams_pen{seams_raster};

    (void)seams.drawn(seams_pen, font, area);

    const int edge = static_cast<int>(seams_laid.x.placed(1, seams_laid.data.at.x, seams_laid.data.at.x + seams_laid.data.wide));

    OUGHT(alpha(seams_raster.pixels(), edge, static_cast<int>(seams_laid.data.at.y + seams_laid.data.high / 2)) > 0.99);

    Plot fitted;
    Series rising;

    rising.shape = Shape::bars;
    rising.points = {Point{1, 1}, Point{2, 2}, Point{3, 3}};
    fitted.series = {rising};

    const Layout fitted_laid = fitted.laid(font, area);

    OUGHT(fitted_laid.y.low() <= 0 && fitted_laid.x.low() <= 0.6 && fitted_laid.x.high() >= 3.4);

    Plot spaced = plot;
    Series pair;

    spaced.bars = Grouping::side;
    pair.shape = Shape::bars;
    pair.name = "pair";
    pair.points = {Point{4, 5}, Point{6, 5}};
    spaced.series = {pair};
    spaced.legend = Corner::upper_left;

    const tal::media::Pixels apart = drawn(spaced);

    SAME(alpha_at(apart, 4.75, 2), 1.0);
    SAME(alpha_at(apart, 5, 2), 0.0);
    SAME(alpha_at(apart, 5.75, 2), 1.0);
    SAME(alpha_at(apart, 5, 5), 0.0);
    OUGHT(tone(apart, static_cast<int>(laid.data.at.x + 21.6), static_cast<int>(laid.data.at.y + 18 + 3.5), 0) > 0.5);

    plot.bars = Grouping::side;
    plot.series = {one};
    plot.cursor = Point{laid.x.placed(5.5, laid.data.at.x, laid.data.at.x + laid.data.wide), laid.data.at.y + 10};

    const int narrow = inked_columns(drawn(plot), static_cast<int>(laid.x.placed(6.5, laid.data.at.x, laid.data.at.x + laid.data.wide)), static_cast<int>(laid.data.at.x + laid.data.wide) - 2, static_cast<int>(laid.data.at.y) + 2, static_cast<int>(laid.data.at.y + laid.data.high) - 2);

    plot.cursor = Point{laid.x.placed(8, laid.data.at.x, laid.data.at.x + laid.data.wide), laid.data.at.y + 10};
    plot.cursor_line = false;

    const tal::media::Pixels between = drawn(plot);

    plot.cursor.reset();
    OUGHT(between.stills().front().rgba == drawn(plot).stills().front().rgba);
    OUGHT(narrow > 40);
}
