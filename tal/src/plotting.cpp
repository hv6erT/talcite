#include "tal/plotting.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>

#include "tal/trouble.hpp"

// Plots (E8): numbers turned into pixels with what tells them, drawn with a pen in a rectangle, so
// that a plot goes into any drawing beside a picture. A scale maps values to a stretch of pixels,
// straight or by the logarithm; its ticks fall on round values, the steps 1, 2 and 5 times a power
// of ten (Heckbert's), as many as the room for their labels allows, with minor ticks between them
// (on a logarithmic scale the powers of ten, and 2 to 9 between them). A scale fitted to data
// widens to the nearest ticks around them. Labels are written as the language writes numbers (its
// decimal sign), with a true minus, and where the values are large or small with a power of ten
// common to the axis, written once at its end.
//
// A plot lays itself out in its rectangle: the margins come from the labels, the titles and the
// common powers as they measure in the font, so that nothing written covers a line or another
// label; the ticks of the bottom axis are as many as their labels fit side by side. The series are
// clipped to the area of the data; a point that is not a number breaks a line. A legend lies in a
// corner of the data, the one with the fewest points under it unless one is asked for. A trace
// keeps samples of one value in time, the last stretch of a window, for a plot that moves with a
// simulation: it takes them from any thread and hands out a copy.
namespace tal::plotting {

namespace {

using drawing::Align;
using drawing::Baseline;
using drawing::Color;
using drawing::Font;
using drawing::Matrix;
using drawing::Path;
using drawing::Pen;
using drawing::Point;
using drawing::Rect;
using drawing::Stroke;

// The step of the series 1, 2, 5 times a power of ten at or above `raw` (a positive number).
double stepped(double raw) {
    const double power = std::pow(10.0, std::floor(std::log10(raw)));
    const double share = raw / power;

    if (share <= 1 + 1e-9) return power;

    if (share <= 2 + 1e-9) return 2 * power;

    if (share <= 5 + 1e-9) return 5 * power;

    return 10 * power;
}

// The step after `step` in the series 1, 2, 5 times a power of ten.
double after(double step) { return stepped(step * 1.5); }

// The multiples of `step` from `low` to `high`, each a whole number of steps, zero exact, no more
// than ten thousand (the step no finer than `finest`, so that the count of steps stays exact).
std::vector<double> multiples(double low, double high, double step) {
    std::vector<double> made;
    const double first = std::ceil(low / step - 1e-9);
    const double last = std::floor(high / step + 1e-9);

    for (double at = first; at <= last && made.size() < 10000; ++at) made.push_back(at == 0 ? 0.0 : at * step);

    return made;
}

// The least step of ticks from `low` to `high` whose multiples, and the fifths of them, are still
// told apart and counted exactly: ten times the spacing of the numbers at the larger end.
double finest(double low, double high) {
    const double end = std::max(std::abs(low), std::abs(high));

    return 10 * (end - std::nextafter(end, 0.0));
}

// The minor ticks from `low` to `high` between the major ones `step` apart: a fifth of a step of 1
// or 5, a quarter of a step of 2; the major ticks themselves are not among them.
std::vector<double> between(double low, double high, double step) {
    const double share = step / std::pow(10.0, std::floor(std::log10(step) + 1e-9));
    const double small = std::abs(share - 2) < 1e-6 ? step / 4 : step / 5;
    std::vector<double> made;

    for (const double one : multiples(low, high, small)) {
        const double ratio = one / step;

        if (std::abs(ratio - std::round(ratio)) > 1e-6) made.push_back(one);
    }

    return made;
}

// The step of the major ticks of the straight scale from `low` to `high`: the least that gives at
// most `most` of them, sought from the least that could (a range of a step more than `most`) and
// no finer than `finest`; the range is divided before the ends are subtracted, so that the widest
// ranges do not overflow.
double major(double low, double high, std::size_t most) {
    const double many = static_cast<double>(std::max<std::size_t>(most, 2));
    double step = std::max(stepped(high / (many + 1) - low / (many + 1)), stepped(finest(low, high)));

    while (std::floor(high / step + 1e-9) - std::ceil(low / step - 1e-9) + 1 > many) step = after(step);

    return step;
}

// The digits of `value` (a whole number) as superscripts, a minus among them.
std::string raised(long long value) {
    static constexpr std::array<std::string_view, 10> digits{"⁰", "¹", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹"};
    const std::string plain = std::to_string(value);
    std::string made;

    for (const char one : plain) made += one == '-' ? std::string{"⁻"} : std::string{digits[static_cast<std::size_t>(one - '0')]};

    return made;
}

// `written` with its leading hyphen a true minus.
std::string signed_(std::string written) {
    if (!written.empty() && written.front() == '-') written.replace(0, 1, "−");

    return written;
}

}

// The labels of `ticks` on `scale` in `language`, and the power of ten common to them (0 for
// none): on a logarithmic scale a power of ten as 10 with its exponent raised, anything else as a
// number with as many decimals as the scale shows; on a straight one numbers with the decimals of
// the step between ticks, divided by the common power where the largest is 10⁵ or more or below
// 10⁻³ (a multiple of three, as engineers write it).
Labels labelled(const Scale& scale, std::span<const double> ticks, const text::Language& language) {
    Labels made;

    if (scale.spacing() == Spacing::logarithmic) {
        for (const double one : ticks) {
            const double exponent = std::log10(one);
            const double whole = std::round(exponent);

            if (std::abs(exponent - whole) < 1e-9) made.said.push_back("10" + raised(static_cast<long long>(whole)));
            else made.said.push_back(signed_(text::number(one, std::max(0, -static_cast<int>(std::floor(exponent))), language)));
        }

        return made;
    }

    double largest = 0;

    for (const double one : ticks) largest = std::max(largest, std::abs(one));

    if (largest >= 1e5 || (largest > 0 && largest < 1e-3)) made.power = 3 * static_cast<int>(std::floor(std::log10(largest) / 3));

    const double unit = std::pow(10.0, made.power);
    const double step = ticks.size() > 1 ? (ticks[1] - ticks[0]) / unit : 1;
    const double shown = -std::floor(std::log10(std::abs(step)) + 1e-9);
    const int decimals = std::isfinite(shown) ? static_cast<int>(std::clamp(shown, 0.0, 17.0)) : 0;

    for (const double one : ticks) made.said.push_back(signed_(text::number(one / unit, decimals, language)));

    return made;
}

namespace {

// The finite values of `series` along x (`across`) or y, positive only for a logarithmic
// `spacing`.
std::vector<double> gathered(const std::vector<Series>& series, bool across, Spacing spacing) {
    std::vector<double> made;

    for (const Series& one : series)
        for (const Point& at : one.points) {
            const double value = across ? at.x : at.y;

            if (std::isfinite(value) && (spacing == Spacing::linear || value > 0)) made.push_back(value);
        }

    return made;
}

// The width of `text` in `font` at `size`.
double wide(const Font& font, std::string_view text, double size) { return font.measured(text, size).wide; }

// `text` filled in `ink` at `at` as aligned.
void written(Pen& pen, const Font& font, std::string_view text, double size, Point at, Align align, Baseline baseline, const Color& ink) {
    if (text.empty()) return;

    pen.fill(font.outlined(text, size, at, align, baseline), ink);
}

// A line from `from` to `to` of `width` in `ink`.
void ruled(Pen& pen, Point from, Point to, double width, const Color& ink) {
    Path line;

    line.move(from);
    line.line(to);

    Stroke style;

    style.width = width;
    style.cap = drawing::Cap::butt;
    pen.stroke(line, ink, style);
}

// How far a line of a series may pass from its points, in pixels: a tenth, as curves are flattened.
constexpr double thinning = 0.1;

// The distance in pixels from `point` to the segment from `from` to `to`.
double apart(Point point, Point from, Point to) {
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    const double length = dx * dx + dy * dy;
    const double along = length > 0 ? std::clamp(((point.x - from.x) * dx + (point.y - from.y) * dy) / length, 0.0, 1.0) : 0.0;

    return std::hypot(point.x - (from.x + along * dx), point.y - (from.y + along * dy));
}

// The points of `run` (finite, in pixels) that a line through them needs to stay within
// `tolerance` of every one: a point is left out where the segment from the last one kept to the
// one after it passes within the tolerance of it and of every point left out since. No more than
// `reach` are left out in a row, so that the work stays linear in the points. A line of thousands
// of points across a few hundred pixels is so drawn with the segments it shows.
std::vector<Point> thinned(const std::vector<Point>& run, double tolerance) {
    constexpr std::size_t reach = 64;

    if (run.size() <= 2) return run;

    std::vector<Point> made{run.front()};
    std::size_t anchor = 0;

    for (std::size_t at = 2; at < run.size(); ++at) {
        bool covers = at - anchor <= reach;

        for (std::size_t middle = anchor + 1; covers && middle < at; ++middle) covers = apart(run[middle], run[anchor], run[at]) <= tolerance;

        if (!covers) {
            made.push_back(run[at - 1]);
            anchor = at - 1;
        }
    }

    made.push_back(run.back());

    return made;
}

// The mark `mark` of `size` around `at` in `ink`.
void marked(Pen& pen, Mark mark, double size, Point at, const Color& ink) {
    const double half = size / 2;
    Path shape;

    switch (mark) {
    case Mark::none: return;
    case Mark::circle:
        shape.arc(at, half, 0, 2 * std::numbers::pi);
        shape.close();
        pen.fill(shape, ink);
        return;
    case Mark::square:
        shape.rectangle(Rect{Point{at.x - half, at.y - half}, size, size});
        pen.fill(shape, ink);
        return;
    case Mark::triangle:
        shape.move(Point{at.x, at.y - half});
        shape.line(Point{at.x + half, at.y + half});
        shape.line(Point{at.x - half, at.y + half});
        shape.close();
        pen.fill(shape, ink);
        return;
    case Mark::cross:
        ruled(pen, Point{at.x - half, at.y - half}, Point{at.x + half, at.y + half}, std::max(1.0, size / 5), ink);
        ruled(pen, Point{at.x - half, at.y + half}, Point{at.x + half, at.y - half}, std::max(1.0, size / 5), ink);
        return;
    }
}

// What the layout found: the scales, their ticks and labels, the margins and the rectangle of the
// data.
struct Laid {
    Layout layout;
    std::vector<double> x_ticks;
    std::vector<double> y_ticks;
    std::vector<double> x_minor;
    std::vector<double> y_minor;
    Labels x_labels;
    Labels y_labels;
    Rect box;
};

// The least distance in pixels between neighbouring `ticks` of `scale` across `room`, endless for
// fewer than two.
double closest(const Scale& scale, const std::vector<double>& ticks, double room) {
    double made = std::numeric_limits<double>::infinity();

    for (std::size_t at = 1; at < ticks.size(); ++at) made = std::min(made, std::abs(scale.placed(ticks[at], 0, room) - scale.placed(ticks[at - 1], 0, room)));

    return made;
}

// The major and minor ticks of an axis.
struct Spread {
    std::vector<double> ticks;
    std::vector<double> minor;
};

// The ticks of `scale` across `room` pixels, the major ones at least `least` pixels apart: on a
// straight scale the least round step that keeps them so; on a logarithmic one, or where that
// leaves fewer than two, as many as the room holds by `ticks`, and as few more as give two, so
// that an axis always shows its measure (then, where they stand too close, only the first and the
// last of them); where no round step gives two, as on a scale only a few numbers wide, its ends.
// However long the axis, no more than `crowd` major ticks.
Spread spread(const Scale& scale, double room, double least) {
    constexpr double crowd = 200;
    Spread made;

    if (scale.spacing() == Spacing::linear && room > 0 && least > 0) {
        double step = std::max(stepped(scale.high() / room * least - scale.low() / room * least), stepped(finest(scale.low(), scale.high())));

        while (std::floor(scale.high() / step + 1e-9) - std::ceil(scale.low() / step - 1e-9) + 1 > crowd) step = after(step);

        if (std::isfinite(step)) {
            made.ticks = multiples(scale.low(), scale.high(), step);

            if (made.ticks.size() >= 2) {
                made.minor = between(scale.low(), scale.high(), step);

                return made;
            }
        }
    }

    const double fit = std::floor(room / least) + 1;
    auto most = static_cast<std::size_t>(fit >= 2 ? std::min(fit, 64.0) : 2.0);

    made.ticks = scale.ticks(most);

    for (const std::size_t limit = most + 8; made.ticks.size() < 2 && most < limit;) made.ticks = scale.ticks(++most);

    made.minor = scale.minor(most);

    if (made.ticks.size() > 2 && closest(scale, made.ticks, room) < least) made.ticks = {made.ticks.front(), made.ticks.back()};

    if (made.ticks.size() < 2) {
        made.ticks = {scale.low(), scale.high()};
        made.minor.clear();
    }

    return made;
}

// The plot `plot` laid out in `area` with `font`: margins from the labels as they measure, the
// ticks of x as far apart as their widest label and a letter, of y two and a half letters, at
// least two each. The labels of x depend on its ticks and the room for them on the margins, so the
// layout is sought again until a round changes nothing, the least distance only growing and the
// room the ticks are spread over only shrinking, so that it cannot swing between two.
Laid laying(const Plot& plot, const Font& font, Rect area) {
    const double letters = plot.letters;
    const double gap = letters * 0.4;
    const double tick = letters * 0.4;
    const text::Language language = plot.language.value_or(text::current());
    Laid made;

    made.layout.x = plot.x.scale.value_or(Scale::fitted(gathered(plot.series, true, plot.x.spacing), plot.x.spacing));
    made.layout.y = plot.y.scale.value_or(Scale::fitted(gathered(plot.series, false, plot.y.spacing), plot.y.spacing));

    const Scale& xs = made.layout.x;
    const Scale& ys = made.layout.y;
    double left = gap * 2;
    double bottom = gap * 2;
    double top = gap + letters * 0.5;
    double right = gap * 2;
    double width = std::max(0.0, area.wide - left - right);
    double room = width;
    double least = letters * 2;
    std::vector<double> x_before;
    std::vector<double> y_before;
    double width_before = -1;

    for (int round = 0; round < 6; ++round) {
        const Spread down = spread(ys, std::max(0.0, area.high - top - bottom), 2.5 * letters);

        made.y_ticks = down.ticks;
        made.y_minor = down.minor;
        made.y_labels = labelled(ys, made.y_ticks, language);

        double widest = 0;

        for (const std::string& one : made.y_labels.said) widest = std::max(widest, wide(font, one, letters));

        left = gap + (plot.y.title.empty() ? 0 : letters * 1.2 + gap) + widest + gap + tick;
        top = gap + (plot.title.empty() ? letters * 0.5 : letters * 1.3 + gap) + (made.y_labels.power != 0 ? letters + gap : 0);

        const Spread across = spread(xs, room, least);

        made.x_ticks = across.ticks;
        made.x_minor = across.minor;
        made.x_labels = labelled(xs, made.x_ticks, language);

        double longest = 0;

        for (const std::string& one : made.x_labels.said) longest = std::max(longest, wide(font, one, letters));

        const std::string power = made.x_labels.power != 0 ? "×10" + raised(made.x_labels.power) : std::string{};
        const double last = made.x_labels.said.empty() ? 0 : wide(font, made.x_labels.said.back(), letters) / 2;

        right = std::max(gap * 2, last + gap) + (power.empty() ? 0 : wide(font, power, letters) + gap);
        bottom = tick + gap + letters + (plot.x.title.empty() ? 0 : gap + letters * 1.2) + gap;
        width = std::max(0.0, area.wide - left - right);

        if (made.x_ticks == x_before && made.y_ticks == y_before && width == width_before) break;

        x_before = made.x_ticks;
        y_before = made.y_ticks;
        width_before = width;
        room = std::min(room, width);
        least = std::max(least, longest + letters);
    }

    made.layout.data = Rect{Point{area.at.x + left, area.at.y + top}, width, std::max(0.0, area.high - top - bottom)};
    made.layout.x_ticks = made.x_ticks;
    made.layout.y_ticks = made.y_ticks;

    return made;
}

// Where `value` of `scale` lies across the data, in pixels, x when `across`.
double at(const Scale& scale, double value, const Rect& data, bool across) {
    return across ? scale.placed(value, data.at.x, data.at.x + data.wide) : scale.placed(value, data.at.y + data.high, data.at.y);
}

// The corner of the data for the legend of `size`: the one asked for, or the one with the fewest
// points of the series under it.
Corner cornered(const Plot& plot, const Laid& laid, double wide_, double high_) {
    if (plot.legend != Corner::automatic) return plot.legend;

    const Rect& data = laid.layout.data;
    Corner best = Corner::upper_right;
    std::size_t fewest = std::numeric_limits<std::size_t>::max();

    for (const Corner one : {Corner::upper_right, Corner::upper_left, Corner::lower_right, Corner::lower_left}) {
        const bool rightward = one == Corner::upper_right || one == Corner::lower_right;
        const bool upward = one == Corner::upper_right || one == Corner::upper_left;
        const double x0 = rightward ? data.at.x + data.wide - wide_ : data.at.x;
        const double y0 = upward ? data.at.y : data.at.y + data.high - high_;
        std::size_t under = 0;

        for (const Series& series : plot.series)
            for (const Point& point : series.points) {
                const double x = at(laid.layout.x, point.x, data, true);
                const double y = at(laid.layout.y, point.y, data, false);

                if (x >= x0 && x <= x0 + wide_ && y >= y0 && y <= y0 + high_) ++under;
            }

        if (under < fewest) {
            fewest = under;
            best = one;
        }
    }

    return best;
}

// The legend of `plot` placed in `laid`: its box and its corner, none when no series has a name,
// the legend is not wanted, or the box does not fit in the data.
void legended(const Plot& plot, const Font& font, Laid& laid) {
    const double gap = plot.letters * 0.4;
    double widest = 0;
    std::size_t named = 0;

    for (const Series& one : plot.series)
        if (!one.name.empty()) {
            widest = std::max(widest, wide(font, one.name, plot.letters));
            ++named;
        }

    const Rect& data = laid.layout.data;
    const double box_wide = gap + plot.letters * 2 + gap + widest + gap;
    const double box_high = gap + plot.letters * 1.4 * static_cast<double>(named) + gap / 2;

    laid.layout.legend = Corner::none;

    if (plot.legend == Corner::none || named == 0 || box_wide + 2 * gap > data.wide || box_high + 2 * gap > data.high) return;

    const Corner corner = cornered(plot, laid, box_wide + gap, box_high + gap);
    const bool rightward = corner == Corner::upper_right || corner == Corner::lower_right;
    const bool upward = corner == Corner::upper_right || corner == Corner::upper_left;

    laid.layout.legend = corner;
    laid.box = Rect{Point{rightward ? data.at.x + data.wide - gap - box_wide : data.at.x + gap, upward ? data.at.y + gap : data.at.y + data.high - gap - box_high}, box_wide, box_high};
}

}

// A scale from `low` to `high`, straight or by the logarithm. Throws Trouble malformed for ends
// that are not finite, not in order, or not above zero on a logarithmic scale.
Scale::Scale(double low, double high, Spacing spacing) : low_(low), high_(high), spacing_(spacing) {
    if (!std::isfinite(low) || !std::isfinite(high) || !(low < high)) throw Trouble{tal::Kind::malformed, "A scale whose ends are not finite numbers in order."};

    if (spacing == Spacing::logarithmic && !(low > 0)) throw Trouble{tal::Kind::malformed, "A logarithmic scale that does not start above zero."};
}

// The scale around `values`, widened to the ticks next to them: for a straight one by the step of
// five ticks, for a logarithmic one to whole powers of ten (from the least value itself where its
// power of ten is below the least number). Values that are not finite (or not positive, on a
// logarithmic one) are left out; with none left, 0 to 1 (1 to 10); a single value stands in the
// middle of a tenth of itself each way (of 1, for zero; within the finite numbers, and below it by
// the least step where a tenth is lost). Ends that would overflow, or that no round step keeps
// apart, stay the values.
Scale Scale::fitted(std::span<const double> values, Spacing spacing) {
    double low = std::numeric_limits<double>::infinity();
    double high = -std::numeric_limits<double>::infinity();

    for (const double one : values)
        if (std::isfinite(one) && (spacing == Spacing::linear || one > 0)) {
            low = std::min(low, one);
            high = std::max(high, one);
        }

    if (spacing == Spacing::logarithmic) {
        if (!(low <= high)) return Scale{1, 10, spacing};

        double from = std::pow(10.0, std::floor(std::log10(low)));
        double to = std::pow(10.0, std::ceil(std::log10(high)));

        if (!(from > 0)) from = low;

        if (!(from < to)) to = from * 10;

        if (!std::isfinite(to)) to = std::numeric_limits<double>::max();

        return Scale{from, to, spacing};
    }

    if (!(low <= high)) return Scale{0, 1, spacing};

    if (low == high) {
        const double spread = low == 0 ? 1 : std::abs(low) / 10;

        low = std::max(std::numeric_limits<double>::lowest(), low - spread);
        high = std::min(std::numeric_limits<double>::max(), high + spread);

        if (!(low < high)) low = std::nextafter(low, -std::numeric_limits<double>::infinity());
    }

    const double step = stepped(high / 4 - low / 4);
    const double from = std::floor(low / step + 1e-9) * step;
    const double to = std::ceil(high / step - 1e-9) * step;

    return std::isfinite(from) && std::isfinite(to) && from < to ? Scale{from, to, spacing} : Scale{low, high, spacing};
}

// Where `value` lies between the pixels `from` (the low end) and `to` (the high end), the halves of
// the values subtracted so that the widest scales do not overflow; not a number for a value that is
// not, or not above zero on a logarithmic scale.
double Scale::placed(double value, double from, double to) const {
    if (spacing_ == Spacing::logarithmic) {
        if (!(value > 0)) return std::numeric_limits<double>::quiet_NaN();

        return from + (std::log10(value) - std::log10(low_)) / (std::log10(high_) - std::log10(low_)) * (to - from);
    }

    return from + (value / 2 - low_ / 2) / (high_ / 2 - low_ / 2) * (to - from);
}

// The major ticks, at most `most` (at least two): round values of the steps 1, 2, 5 times a power
// of ten; on a logarithmic scale the powers of ten (every so many, to keep within `most`), or,
// within less than two of them, the values 1, 2 and 5 times a power of ten, or the straight ticks.
std::vector<double> Scale::ticks(std::size_t most) const {
    most = std::max<std::size_t>(most, 2);

    if (spacing_ == Spacing::logarithmic) {
        const double first = std::ceil(std::log10(low_) - 1e-9);
        const double last = std::floor(std::log10(high_) + 1e-9);

        if (last - first + 1 >= 2) {
            const double stride = std::ceil((last - first + 1) / static_cast<double>(most));
            std::vector<double> made;

            for (double power = first; power <= last; power += stride) made.push_back(std::pow(10.0, power));

            return made;
        }

        std::vector<double> made;

        for (double power = std::floor(std::log10(low_)) - 1; power <= std::ceil(std::log10(high_)); ++power)
            for (const double digit : {1.0, 2.0, 5.0}) {
                const double one = digit * std::pow(10.0, power);

                if (one >= low_ * (1 - 1e-9) && one <= high_ * (1 + 1e-9)) made.push_back(one);
            }

        if (made.size() >= 2 && made.size() <= most) return made;

        return multiples(low_, high_, major(low_, high_, most));
    }

    return multiples(low_, high_, major(low_, high_, most));
}

// The minor ticks between the major ones of `ticks(most)`: a fifth of a step of 1 or 5, a quarter
// of a step of 2; on a logarithmic scale of whole powers of ten one by one, 2 to 9 times each.
// The major ticks themselves are not among them.
std::vector<double> Scale::minor(std::size_t most) const {
    std::vector<double> made;

    if (spacing_ == Spacing::logarithmic) {
        const std::vector<double> majors = ticks(most);

        if (majors.size() < 2 || std::abs(majors[1] / majors[0] - 10) > 1e-6) return made;

        for (double power = std::floor(std::log10(low_)); power <= std::ceil(std::log10(high_)); ++power)
            for (int digit = 2; digit <= 9; ++digit) {
                const double one = digit * std::pow(10.0, power);

                if (one > low_ * (1 + 1e-9) && one < high_ * (1 - 1e-9)) made.push_back(one);
            }

        return made;
    }

    return between(low_, high_, major(low_, high_, std::max<std::size_t>(most, 2)));
}

// Colors for a dark ground: light ink, and a palette after Okabe and Ito, lightened where its dark
// colors would sink into the ground.
Theme dark() {
    Theme made;

    made.palette = {Color{0.90F, 0.62F, 0.00F, 1}, Color{0.34F, 0.71F, 0.91F, 1}, Color{0.00F, 0.70F, 0.50F, 1}, Color{0.94F, 0.89F, 0.26F, 1},
                    Color{0.40F, 0.60F, 1.00F, 1}, Color{0.95F, 0.45F, 0.20F, 1}, Color{0.85F, 0.55F, 0.75F, 1}, Color{0.75F, 0.75F, 0.75F, 1}};

    return made;
}

// Colors for a light ground: dark ink, and the palette of Okabe and Ito as it is.
Theme light() {
    Theme made;

    made.ink = Color{0.12F, 0.13F, 0.15F, 1};
    made.grid = Color{0.12F, 0.13F, 0.15F, 0.15F};
    made.backing = Color{1, 1, 1, 0.85F};
    made.palette = {Color{0.90F, 0.62F, 0.00F, 1}, Color{0.34F, 0.71F, 0.91F, 1}, Color{0.00F, 0.62F, 0.45F, 1}, Color{0.80F, 0.70F, 0.00F, 1},
                    Color{0.00F, 0.45F, 0.70F, 1}, Color{0.84F, 0.37F, 0.00F, 1}, Color{0.80F, 0.47F, 0.65F, 1}, Color{0.40F, 0.40F, 0.40F, 1}};

    return made;
}

// The plot laid out in `area` with `font`: the rectangle of its data and its scales.
Layout Plot::laid(const Font& font, Rect area) const {
    Laid made = laying(*this, font, area);

    legended(*this, font, made);

    return made.layout;
}

// The plot drawn with `pen` in `area`, in `font`: the paper, the grid, the series clipped to the
// data (their lines thinned to the points they need within `thinning`, their lines and marks no
// wider than twice the diagonal of the data, past which they only bury the plot, so that a stroke
// of any width costs no more than that), the frame with its ticks and labels, the common powers,
// the titles and the legend (its samples no wider than half a letter, their marks than a letter).
// Lines of one pixel stand on the middles of pixels, so that they come out sharp. Returns the
// layout.
Layout Plot::drawn(Pen& pen, const Font& font, Rect area) const {
    Laid laid = laying(*this, font, area);

    legended(*this, font, laid);

    const Rect& data = laid.layout.data;
    const double gap = letters * 0.4;
    const double tick = letters * 0.4;
    const Color& ink = theme.ink;

    if (theme.paper) {
        Path ground;

        ground.rectangle(area);
        pen.fill(ground, *theme.paper);
    }

    if (data.wide <= 0 || data.high <= 0) return laid.layout;

    const double left = std::round(data.at.x) + 0.5;
    const double right = std::round(data.at.x + data.wide) - 0.5;
    const double top = std::round(data.at.y) + 0.5;
    const double bottom = std::round(data.at.y + data.high) - 0.5;
    const auto sharp = [](double pixel) { return std::round(pixel - 0.5) + 0.5; };

    if (x.grid)
        for (const double one : laid.x_ticks) ruled(pen, Point{sharp(at(laid.layout.x, one, data, true)), top}, Point{sharp(at(laid.layout.x, one, data, true)), bottom}, 1, theme.grid);

    if (y.grid)
        for (const double one : laid.y_ticks) ruled(pen, Point{left, sharp(at(laid.layout.y, one, data, false))}, Point{right, sharp(at(laid.layout.y, one, data, false))}, 1, theme.grid);

    const double reach = 2 * std::hypot(data.wide, data.high);

    pen.save();

    Path inside;

    inside.rectangle(data);
    pen.clip(inside);

    for (std::size_t index = 0; index < series.size(); ++index) {
        const Series& one = series[index];
        const Color color = one.color.value_or(theme.palette.empty() ? ink : theme.palette[index % theme.palette.size()]);
        std::vector<Point> placed;

        for (const Point& point : one.points) placed.push_back(Point{at(laid.layout.x, point.x, data, true), at(laid.layout.y, point.y, data, false)});

        if (one.line) {
            Path path;
            std::vector<Point> run;
            const auto ended = [&] {
                const std::vector<Point> kept = thinned(run, thinning);

                for (std::size_t at = 0; at < kept.size(); ++at) {
                    if (at == 0) path.move(kept[at]);
                    else path.line(kept[at]);
                }

                run.clear();
            };

            for (const Point& point : placed) {
                if (std::isfinite(point.x) && std::isfinite(point.y)) run.push_back(point);
                else ended();
            }

            ended();

            Stroke style;

            style.width = std::min(one.width, reach);
            style.join = drawing::Join::round;
            style.cap = drawing::Cap::round;
            pen.stroke(path, color, style);
        }

        for (const Point& point : placed)
            if (std::isfinite(point.x) && std::isfinite(point.y)) marked(pen, one.mark, std::min(one.size, reach), point, color);
    }

    pen.restore();

    Path frame;

    frame.rectangle(Rect{Point{left, top}, right - left, bottom - top});

    Stroke thin;

    thin.width = 1;
    pen.stroke(frame, ink, thin);

    for (const double one : laid.x_minor) ruled(pen, Point{sharp(at(laid.layout.x, one, data, true)), bottom}, Point{sharp(at(laid.layout.x, one, data, true)), bottom + tick / 2}, 1, ink);

    for (const double one : laid.y_minor) ruled(pen, Point{left - tick / 2, sharp(at(laid.layout.y, one, data, false))}, Point{left, sharp(at(laid.layout.y, one, data, false))}, 1, ink);

    for (std::size_t index = 0; index < laid.x_ticks.size(); ++index) {
        const double where = sharp(at(laid.layout.x, laid.x_ticks[index], data, true));

        ruled(pen, Point{where, bottom}, Point{where, bottom + tick}, 1, ink);
        written(pen, font, laid.x_labels.said[index], letters, Point{where, bottom + tick + gap}, Align::center, Baseline::top, ink);
    }

    for (std::size_t index = 0; index < laid.y_ticks.size(); ++index) {
        const double where = sharp(at(laid.layout.y, laid.y_ticks[index], data, false));

        ruled(pen, Point{left - tick, where}, Point{left, where}, 1, ink);
        written(pen, font, laid.y_labels.said[index], letters, Point{left - tick - gap, where}, Align::right, Baseline::middle, ink);
    }

    if (laid.x_labels.power != 0)
        written(pen, font, "×10" + raised(laid.x_labels.power), letters, Point{right + gap + wide(font, laid.x_labels.said.empty() ? std::string{} : laid.x_labels.said.back(), letters) / 2, bottom + tick + gap}, Align::left, Baseline::top, ink);

    if (laid.y_labels.power != 0) written(pen, font, "×10" + raised(laid.y_labels.power), letters, Point{left, top - gap}, Align::left, Baseline::bottom, ink);

    if (!x.title.empty()) written(pen, font, x.title, letters, Point{(left + right) / 2, area.at.y + area.high - gap}, Align::center, Baseline::bottom, ink);

    if (!y.title.empty()) {
        pen.save();
        pen.place(Matrix::turned(-std::numbers::pi / 2).then(Matrix::moved(area.at.x + gap, (top + bottom) / 2)).then(pen.placed()));
        written(pen, font, y.title, letters, Point{0, 0}, Align::center, Baseline::top, ink);
        pen.restore();
    }

    if (!title.empty()) written(pen, font, title, letters * 1.1, Point{(left + right) / 2, area.at.y + gap}, Align::center, Baseline::top, ink);

    if (laid.layout.legend == Corner::none) return laid.layout;

    const Point origin = laid.box.at;
    const double sample = letters * 2;
    const double row = letters * 1.4;
    Path box;

    box.rectangle(laid.box);
    pen.fill(box, theme.backing);
    pen.stroke(box, theme.grid, thin);

    std::size_t index = 0;

    for (std::size_t number = 0; number < series.size(); ++number) {
        const Series& one = series[number];

        if (one.name.empty()) continue;

        const Color color = one.color.value_or(theme.palette.empty() ? ink : theme.palette[number % theme.palette.size()]);
        const double middle = origin.y + gap + row * (static_cast<double>(index) + 0.5);

        if (one.line) ruled(pen, Point{origin.x + gap, middle}, Point{origin.x + gap + sample, middle}, std::min(one.width, letters / 2), color);

        marked(pen, one.mark, std::min(one.size, letters), Point{origin.x + gap + sample / 2, middle}, color);
        written(pen, font, one.name, letters, Point{origin.x + gap + sample + gap, middle}, Align::left, Baseline::middle, ink);
        ++index;
    }

    return laid.layout;
}

// A trace keeping the samples of the last `window` (in the units of their time), and no more than
// `most` of them. Throws Trouble malformed for a window that is not a positive finite number or
// room for fewer than two samples.
Trace::Trace(double window, std::size_t most) : window_(window), most_(most) {
    if (!std::isfinite(window) || !(window > 0) || most < 2) throw Trouble{tal::Kind::malformed, "A trace of a window that is not positive or of room for fewer than two samples."};
}

// The sample `value` at `time`, from any thread. Samples older than the window behind it go, and
// the oldest beyond the room; a time before the last one starts the trace anew (the simulation
// went back), and a time that is not finite is left out.
void Trace::add(double time, double value) {
    if (!std::isfinite(time)) return;

    const std::lock_guard<std::mutex> kept{guard_};

    if (!kept_.empty() && time < kept_.back().x) kept_.clear();

    kept_.push_back(Point{time, value});

    while (!kept_.empty() && (kept_.front().x < time - window_ || kept_.size() > most_)) kept_.pop_front();
}

// No samples kept.
void Trace::clear() {
    const std::lock_guard<std::mutex> kept{guard_};

    kept_.clear();
}

// A copy of the samples kept, oldest first: time across, value up.
std::vector<Point> Trace::points() const {
    const std::lock_guard<std::mutex> kept{guard_};

    return {kept_.begin(), kept_.end()};
}

// The scale of time the window covers, ending at the last sample; none before the first. Where the
// window is lost against a time so large, the least step of the numbers below it, and where it
// reaches beyond the least number, from there.
std::optional<Scale> Trace::span() const {
    const std::lock_guard<std::mutex> kept{guard_};

    if (kept_.empty()) return std::nullopt;

    const double last = kept_.back().x;
    const double first = std::max(std::numeric_limits<double>::lowest(), last - window_);

    if (first < last) return Scale{first, last};

    if (last > std::numeric_limits<double>::lowest()) return Scale{std::nextafter(last, -std::numeric_limits<double>::infinity()), last};

    return Scale{last, std::nextafter(last, std::numeric_limits<double>::infinity())};
}

}
