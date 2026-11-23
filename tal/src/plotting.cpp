#include "tal/plotting.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <string>
#include <string_view>
#include <unordered_map>
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

namespace {

// How the numbers of a straight axis are written: the power of ten common to them (0 for none)
// and the decimals after it.
struct Written {
    int power = 0;
    int decimals = 0;
};

// How the numbers of a straight axis with `ticks` are written: divided by a power of ten common
// to them where the largest is 10⁵ or more or below 10⁻³ (a multiple of three, as engineers write
// it), with the decimals of the step between ticks (none where it is not a number).
Written written_as(std::span<const double> ticks) {
    Written made;
    double largest = 0;

    for (const double one : ticks) largest = std::max(largest, std::abs(one));

    if (largest >= 1e5 || (largest > 0 && largest < 1e-3)) made.power = 3 * static_cast<int>(std::floor(std::log10(largest) / 3));

    const double step = ticks.size() > 1 ? (ticks[1] - ticks[0]) / std::pow(10.0, made.power) : 1;
    const double shown = -std::floor(std::log10(std::abs(step)) + 1e-9);

    made.decimals = std::isfinite(shown) ? static_cast<int>(std::clamp(shown, 0.0, 17.0)) : 0;

    return made;
}

}

// The labels of `ticks` on `scale` in `language`, and the power of ten common to them (0 for
// none): on a logarithmic scale a power of ten as 10 with its exponent raised, anything else as a
// number with as many decimals as the scale shows; on a straight one as `written_as` tells.
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

    const Written form = written_as(ticks);
    const double unit = std::pow(10.0, form.power);

    made.power = form.power;

    for (const double one : ticks) made.said.push_back(signed_(text::number(one / unit, form.decimals, language)));

    return made;
}

namespace {

// The most bins a histogram may have.
constexpr std::size_t most_bins = std::size_t{1} << 20;

// The value at the share `share` (0 to 1) of the sorted `values`, between the two nearest as
// they lie (the seventh definition of Hyndman and Fan), their halves subtracted so that the
// widest values do not overflow.
double quantile(const std::vector<double>& values, double share) {
    const double place = share * static_cast<double>(values.size() - 1);
    const auto below = static_cast<std::size_t>(std::floor(place));
    const std::size_t above = std::min(below + 1, values.size() - 1);
    const double part = place - static_cast<double>(below);

    return values[below] + (values[above] / 2 - values[below] / 2) * 2 * part;
}

}

// The bins of the finite `values` (the rest left out) and how many fall in each, a value on an
// edge counting in the bin above it, the greatest in the last. Their width as `how` asks, or else
// by the rule of Freedman and Diaconis (twice the interquartile range over the cube root of the
// count), by the rule of Sturges (the range over one more than the binary logarithm of the count)
// where the quartiles meet, a tenth of the value (1, for zero) where all are one, raised to the
// next 1, 2 or 5 times a power of ten (no finer than `finest`, so that the bins stay apart) and
// widened, twice at least, while there would be more than `most_bins`; the first bin then starts
// at a multiple of the width. A width asked for starts there too; a count
// asked for divides the range evenly from its least value (around the value, where all are one).
// Throws Trouble malformed for a width that is not a positive finite number or a count of none,
// and Trouble full past `most_bins` bins asked for by the width. None without a finite value.
Bins binned(std::span<const double> values, Binning how) {
    if (how.width && !(std::isfinite(*how.width) && *how.width > 0)) throw Trouble{tal::Kind::malformed, "A bin width that is not a positive finite number."};

    if (how.count && *how.count == 0) throw Trouble{tal::Kind::malformed, "A histogram of no bins."};

    if (how.count && *how.count > most_bins) throw Trouble{tal::Kind::full, "A histogram of more than " + std::to_string(most_bins) + " bins."};

    std::vector<double> kept;

    for (const double one : values)
        if (std::isfinite(one)) kept.push_back(one);

    Bins made;

    if (kept.empty()) return made;

    std::sort(kept.begin(), kept.end());

    const double least = kept.front();
    const double most = kept.back();
    const double one_value = least == 0 ? 1 : stepped(std::abs(least) / 10);
    std::size_t count = 0;

    if (how.count) {
        count = *how.count;
        made.width = most > least ? std::max((most / 2 - least / 2) / static_cast<double>(count) * 2, std::numeric_limits<double>::denorm_min()) : one_value;
        made.low = most > least ? least : least - made.width * static_cast<double>(count) / 2;
    } else {
        double width = how.width.value_or(0);

        if (!how.width) {
            const double n = static_cast<double>(kept.size());

            width = (quantile(kept, 0.75) / 2 - quantile(kept, 0.25) / 2) / std::cbrt(n) * 4;

            if (!(width > 0)) width = (most / 2 - least / 2) / (std::ceil(std::log2(n)) + 1) * 2;

            if (!(width > 0)) width = one_value;

            width = std::max(std::isfinite(width) ? stepped(width) : 1e308, stepped(finest(least, most)));
        }

        for (;;) {
            const double low = std::floor(least / width) * width;

            made.width = width;
            made.low = std::isfinite(low) ? low : least;

            const double many = std::floor(most / width - made.low / width) + 1;

            if (many <= static_cast<double>(most_bins)) {
                count = static_cast<std::size_t>(many);

                break;
            }

            if (how.width) throw Trouble{tal::Kind::full, "A histogram of more than " + std::to_string(most_bins) + " bins."};

            width = std::max(after(width), width * 2);
        }
    }

    made.counts.assign(count, 0);

    for (const double one : kept) {
        const double at = std::floor(one / made.width - made.low / made.width);

        ++made.counts[static_cast<std::size_t>(std::clamp(at, 0.0, static_cast<double>(count - 1)))];
    }

    return made;
}

// The bars of `bins`, one at the middle of each, as wide as a bin so that they meet: how many fall
// in it, their share of all, or that share over the width (a density, whose bars have an area of
// one); shares of nothing are naught.
Series histogram(const Bins& bins, Tally tally) {
    Series made;
    std::size_t all = 0;

    for (const std::size_t one : bins.counts) all += one;

    made.shape = Shape::bars;
    made.bar = bins.width;

    for (std::size_t at = 0; at < bins.counts.size(); ++at) {
        const double many = static_cast<double>(bins.counts[at]);
        const double share = all > 0 ? many / static_cast<double>(all) : 0;
        const double value = tally == Tally::count ? many : tally == Tally::share ? share : share / bins.width;

        made.points.push_back(Point{bins.low + (static_cast<double>(at) + 0.5) * bins.width, value});
    }

    return made;
}

namespace {

// The most nodes a field may have: 2²⁶, half a gigabyte of values.
constexpr std::size_t most_nodes = std::size_t{1} << 26;

// Throws Trouble malformed unless `field` has two columns and two rows of nodes at least and a
// value for each, and Trouble full past `most_nodes` nodes.
void shaped(const Field& field) {
    if (field.columns < 2 || field.rows < 2) throw Trouble{tal::Kind::malformed, "A field of fewer than two columns or two rows."};

    if (field.columns > most_nodes / field.rows) throw Trouble{tal::Kind::full, "A field of more than " + std::to_string(most_nodes) + " nodes."};

    if (field.values.size() != field.columns * field.rows) throw Trouble{tal::Kind::malformed, "A field without a value for each of its nodes."};
}

// The value of `field` (shaped) at the column `across` and the row `up` counted in nodes, both
// within the field: that of the nearest node (not a number where it has no finite value), or,
// `smooth`, weighed between the four nodes around them (those with finite values, their weights
// made whole again), within the least and the greatest of them.
double sample(const Field& field, double across, double up, bool smooth) {
    const auto column = static_cast<std::size_t>(std::round(across));
    const auto row = static_cast<std::size_t>(std::round(up));
    const double nearest = field.values[row * field.columns + column];

    if (!std::isfinite(nearest)) return std::numeric_limits<double>::quiet_NaN();

    if (!smooth) return nearest;

    const std::size_t left = std::min(static_cast<std::size_t>(across), field.columns - 2);
    const std::size_t low = std::min(static_cast<std::size_t>(up), field.rows - 2);
    const double right_part = across - static_cast<double>(left);
    const double up_part = up - static_cast<double>(low);
    double sum = 0;
    double weight = 0;
    double least = nearest;
    double most = nearest;

    for (std::size_t over = 0; over < 2; ++over)
        for (std::size_t along = 0; along < 2; ++along) {
            const double one = field.values[(low + over) * field.columns + left + along];
            const double share = (along == 1 ? right_part : 1 - right_part) * (over == 1 ? up_part : 1 - up_part);

            if (!std::isfinite(one) || !(share > 0)) continue;

            sum += share * one;
            weight += share;
            least = std::min(least, one);
            most = std::max(most, one);
        }

    return std::clamp(sum / weight, least, most);
}

// A color in Oklab, Ottosson's space of perceived lightness and hue.
struct Lab {
    double l = 0;
    double a = 0;
    double b = 0;
};

// `color` (sRGB, clamped to 0 to 1) in Oklab.
Lab oklab(const Color& color) {
    const auto linear = [](float one) {
        const double c = std::clamp(static_cast<double>(one), 0.0, 1.0);

        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    const double r = linear(color.r);
    const double g = linear(color.g);
    const double b = linear(color.b);
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);

    return Lab{0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
               0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

// The color `lab` in sRGB (clamped to 0 to 1) with `alpha`.
Color srgb(const Lab& lab, float alpha) {
    const double l = std::pow(lab.l + 0.3963377774 * lab.a + 0.2158037573 * lab.b, 3);
    const double m = std::pow(lab.l - 0.1055613458 * lab.a - 0.0638541728 * lab.b, 3);
    const double s = std::pow(lab.l - 0.0894841775 * lab.a - 1.2914855480 * lab.b, 3);
    const auto encoded = [](double one) {
        const double c = std::clamp(one, 0.0, 1.0);

        return static_cast<float>(c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055);
    };

    return Color{encoded(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s), encoded(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s),
                 encoded(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s), alpha};
}

// The color of the bytes `red`, `green` and `blue` (sRGB).
Color hex(int red, int green, int blue) { return Color{static_cast<float>(red) / 255.0F, static_cast<float>(green) / 255.0F, static_cast<float>(blue) / 255.0F, 1}; }

// The point between `low` and `high` at the share `part` of the way, the ends weighed by it, so
// that no difference of them overflows.
double between_ends(double low, double high, double part) { return low * (1 - part) + high * part; }

}

// The place of the node in `column` and `row`: the columns spread over x from its low end to its
// high end, the rows over y from its low end up, evenly (by the logarithm, on a logarithmic scale);
// the last of each exactly at the high end. Throws Trouble malformed for a field of fewer than two
// columns or rows, or a node it does not have.
Point Field::node(std::size_t column, std::size_t row) const {
    if (columns < 2 || rows < 2 || column >= columns || row >= rows) throw Trouble{tal::Kind::malformed, "A node the field does not have."};

    const double last_column = static_cast<double>(columns - 1);
    const double last_row = static_cast<double>(rows - 1);

    return Point{column + 1 == columns ? x.high() : x.valued(static_cast<double>(column), 0, last_column), row + 1 == rows ? y.high() : y.valued(static_cast<double>(row), 0, last_row)};
}

// The value of the field at `at` (x and y): that of the nearest node, or, `smooth`, weighed
// between the four around it (bilinearly, as their places lie along each scale); not a number
// outside the field or where the nearest node has no finite value. Throws as `shaped`.
double Field::value(Point at, bool smooth) const {
    shaped(*this);

    const double across = x.placed(at.x, 0, static_cast<double>(columns - 1));
    const double up = y.placed(at.y, 0, static_cast<double>(rows - 1));

    if (!(across >= 0 && across <= static_cast<double>(columns - 1) && up >= 0 && up <= static_cast<double>(rows - 1))) return std::numeric_limits<double>::quiet_NaN();

    return sample(*this, across, up, smooth);
}

// The lines of `field` at `level`, in its values, by marching squares: a cell of four nodes with
// finite values is crossed on each side whose nodes lie one at or above the level and one below,
// at the place between them where the level falls (weighed by the values, as the nodes lie along
// each scale); a cell crossed on all four sides (a saddle) joins its sides so as to cut off the
// corners on the other side of the level from the mean of its four values. The pieces are joined
// where they meet into lines: open ones from an edge of the field or a node without a value, then
// closed ones, whose last point is their first. None for a level that is not a number or is
// endless, since every node then lies on one side of it. Throws as `shaped`.
std::vector<std::vector<Point>> contoured(const Field& field, double level) {
    shaped(field);

    std::vector<std::vector<Point>> made;
    const std::size_t columns = field.columns;
    const double last_column = static_cast<double>(columns - 1);
    const double last_row = static_cast<double>(field.rows - 1);
    const auto value = [&](std::size_t column, std::size_t row) { return field.values[row * columns + column]; };
    std::unordered_map<std::size_t, Point> crossed;
    std::vector<std::array<std::size_t, 2>> pieces;

    // The side from the node (column, row) to the right is numbered 2(row·columns + column), the
    // one up from it one more; its crossing is found once.
    const auto side = [&](std::size_t column, std::size_t row, bool upward) {
        const std::size_t number = 2 * (row * columns + column) + (upward ? 1 : 0);

        if (!crossed.contains(number)) {
            const std::size_t to_column = upward ? column : column + 1;
            const std::size_t to_row = upward ? row + 1 : row;
            const double from = value(column, row);
            const double to = value(to_column, to_row);
            const double span = to - from;
            double part = std::isfinite(span) ? (level - from) / span : (level / 2 - from / 2) / (to / 2 - from / 2);

            part = std::isfinite(part) ? std::clamp(part, 0.0, 1.0) : 0.5;
            const double across = upward ? static_cast<double>(column) : static_cast<double>(column) + part;
            const double up = upward ? static_cast<double>(row) + part : static_cast<double>(row);

            crossed.emplace(number, Point{field.x.valued(across, 0, last_column), field.y.valued(up, 0, last_row)});
        }

        return number;
    };

    for (std::size_t row = 0; row + 1 < field.rows; ++row)
        for (std::size_t column = 0; column + 1 < columns; ++column) {
            const std::array<double, 4> corners{value(column, row), value(column + 1, row), value(column + 1, row + 1), value(column, row + 1)};

            if (!std::all_of(corners.begin(), corners.end(), [](double one) { return std::isfinite(one); })) continue;

            std::array<bool, 4> above{};

            for (std::size_t at = 0; at < 4; ++at) above[at] = corners[at] >= level;

            // The sides of the cell, each between the corners `at` and `at + 1`: below, right, top
            // and left.
            const std::array<bool, 4> cut{above[0] != above[1], above[1] != above[2], above[2] != above[3], above[3] != above[0]};
            const auto numbered = [&](std::size_t at) {
                switch (at) {
                case 0: return side(column, row, false);
                case 1: return side(column + 1, row, true);
                case 2: return side(column, row + 1, false);
                default: return side(column, row, true);
                }
            };
            const auto count = static_cast<std::size_t>(std::count(cut.begin(), cut.end(), true));

            if (count == 2) {
                std::array<std::size_t, 2> ends{};
                std::size_t found = 0;

                for (std::size_t at = 0; at < 4; ++at)
                    if (cut[at]) ends[found++] = numbered(at);

                pieces.push_back(ends);
            } else if (count == 4) {
                const bool middle = corners[0] / 4 + corners[1] / 4 + corners[2] / 4 + corners[3] / 4 >= level;

                // A corner on the other side from the middle is cut off by the sides meeting at it:
                // the corner `at` lies between the sides `at - 1` and `at`.
                for (std::size_t at = 0; at < 4; ++at)
                    if (above[at] != middle) pieces.push_back({numbered((at + 3) % 4), numbered(at)});
            }
        }

    std::unordered_map<std::size_t, std::array<std::size_t, 2>> meeting;
    std::unordered_map<std::size_t, std::size_t> meetings;

    for (std::size_t at = 0; at < pieces.size(); ++at)
        for (const std::size_t end : pieces[at]) meeting[end][meetings[end]++ % 2] = at;

    std::vector<bool> taken(pieces.size(), false);

    // The line from the side `start` along the piece `first`, piece by piece while one not taken
    // meets the side it reached.
    const auto walked = [&](std::size_t start, std::size_t first) {
        std::vector<Point> line{crossed.at(start)};
        std::size_t at = start;
        std::optional<std::size_t> piece = first;

        while (piece) {
            taken[*piece] = true;
            at = pieces[*piece][0] == at ? pieces[*piece][1] : pieces[*piece][0];
            line.push_back(crossed.at(at));
            piece.reset();

            for (std::size_t one = 0; one < std::min<std::size_t>(meetings.at(at), 2); ++one)
                if (!taken[meeting.at(at)[one]]) piece = meeting.at(at)[one];
        }

        return line;
    };

    for (std::size_t at = 0; at < pieces.size(); ++at)
        for (const std::size_t end : pieces[at])
            if (!taken[at] && meetings.at(end) == 1) made.push_back(walked(end, at));

    for (std::size_t at = 0; at < pieces.size(); ++at)
        if (!taken[at]) made.push_back(walked(pieces[at][0], at));

    return made;
}

// The color at `share` (0 to 1, clamped) of the way through the palette, its colors spread evenly
// and mixed in Oklab, so that equal steps look equally far; clear for a share that is not a
// number. Throws Trouble malformed for a palette of no colors.
Color Palette::at(double share) const {
    if (colors.empty()) throw Trouble{tal::Kind::malformed, "A palette of no colors."};

    if (std::isnan(share)) return Color{0, 0, 0, 0};

    if (colors.size() == 1) return colors.front();

    const double place = std::clamp(share, 0.0, 1.0) * static_cast<double>(colors.size() - 1);
    const std::size_t below = std::min(static_cast<std::size_t>(place), colors.size() - 2);
    const double part = place - static_cast<double>(below);
    const Lab from = oklab(colors[below]);
    const Lab to = oklab(colors[below + 1]);

    return srgb(Lab{between_ends(from.l, to.l, part), between_ends(from.a, to.a, part), between_ends(from.b, to.b, part)},
                static_cast<float>(between_ends(colors[below].a, colors[below + 1].a, part)));
}

// Viridis (van der Walt and Smith), dark violet to yellow, its lightness rising all the way: read
// alike in colour, in grey and by most who see colours otherwise; nine of its colors.
Palette viridis() {
    return Palette{{hex(0x44, 0x01, 0x54), hex(0x47, 0x2d, 0x7b), hex(0x3b, 0x52, 0x8b), hex(0x2c, 0x72, 0x8e), hex(0x21, 0x90, 0x8c), hex(0x27, 0xad, 0x81),
                    hex(0x5d, 0xc8, 0x63), hex(0xaa, 0xdc, 0x32), hex(0xfd, 0xe7, 0x25)}};
}

// Blue through nearly white to red (Brewer's RdBu turned round), for values on either side of a
// middle.
Palette diverging() {
    return Palette{{hex(0x21, 0x66, 0xac), hex(0x67, 0xa9, 0xcf), hex(0xd1, 0xe5, 0xf0), hex(0xf7, 0xf7, 0xf7), hex(0xfd, 0xdb, 0xc7), hex(0xef, 0x8a, 0x62),
                    hex(0xb2, 0x18, 0x2b)}};
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

// A bar of a series: the series and its point, its sides along x, its foot and its top along y.
struct Block {
    std::size_t series = 0;
    std::size_t point = 0;
    double left = 0;
    double right = 0;
    double foot = 0;
    double top = 0;
};

// The bars of the series of `plot` shaped so, standing on `floor`: each as wide as its series
// asks, or eight tenths of the least distance between neighbouring x of all bars (of 1, without
// two); side by side, each a share of that width (`Grouping::side`), one on another at the same x
// in the order of the series (`Grouping::stacked`, the first on the floor), or over one another
// (`Grouping::over`). Points that are not numbers stand for no bar.
std::vector<Block> blocked(const Plot& plot, double floor) {
    std::vector<double> across;
    std::size_t many = 0;

    for (const Series& one : plot.series)
        if (one.shape == Shape::bars) {
            ++many;

            for (const Point& point : one.points)
                if (std::isfinite(point.x) && std::isfinite(point.y)) across.push_back(point.x);
        }

    std::sort(across.begin(), across.end());

    double gap = std::numeric_limits<double>::infinity();

    for (std::size_t at = 1; at < across.size(); ++at)
        if (across[at] > across[at - 1]) gap = std::min(gap, across[at] / 2 - across[at - 1] / 2);

    const double shared = std::isfinite(gap) ? gap * 1.6 : 0.8;
    std::map<double, double> tops;
    std::vector<Block> made;
    std::size_t order = 0;

    for (std::size_t index = 0; index < plot.series.size(); ++index) {
        const Series& one = plot.series[index];

        if (one.shape != Shape::bars) continue;

        const double wide = one.bar.value_or(shared);
        const bool side = plot.bars == Grouping::side;
        const double part = side ? wide / static_cast<double>(many) : wide;
        const double shift = side ? (static_cast<double>(order) - static_cast<double>(many - 1) / 2) * part : 0;

        for (std::size_t point = 0; point < one.points.size(); ++point) {
            const Point& at = one.points[point];

            if (!std::isfinite(at.x) || !std::isfinite(at.y)) continue;

            Block block{index, point, at.x + shift - part / 2, at.x + shift + part / 2, floor, at.y};

            if (plot.bars == Grouping::stacked) {
                double& reached = tops[at.x];

                block.foot = reached == 0 ? floor : reached;
                block.top = reached + at.y;
                reached = block.top;
            }

            made.push_back(block);
        }

        ++order;
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

// The field under `plot`: that of its heat, or else of its first contours; none without either.
const Field* ground(const Plot& plot) {
    if (plot.heat) return &plot.heat->field;

    return plot.contours.empty() ? nullptr : &plot.contours.front().field;
}

// The values of `field` that are finite numbers.
std::vector<double> finite_of(const Field& field) {
    std::vector<double> made;

    for (const double one : field.values)
        if (std::isfinite(one)) made.push_back(one);

    return made;
}

// The scale the values of the field of `plot` are read on, for colors and readouts: the range of
// its heat, or from the least of the field's finite values to the greatest, so that the map spans
// the whole palette; about the middle of the heat where one is asked, as far each way as the
// farthest value (halved, so that the distances do not overflow). Values all one, or none, take a
// scale fitted around them.
Scale ranged(const Plot& plot) {
    if (plot.heat && plot.heat->range) return *plot.heat->range;

    const std::vector<double> kept = finite_of(*ground(plot));
    double low = std::numeric_limits<double>::infinity();
    double high = -std::numeric_limits<double>::infinity();

    for (const double one : kept) {
        low = std::min(low, one);
        high = std::max(high, one);
    }

    if (plot.heat && plot.heat->middle && std::isfinite(*plot.heat->middle) && !kept.empty()) {
        const double middle = *plot.heat->middle;
        const double reach = std::max(std::abs(low / 2 - middle / 2), std::abs(high / 2 - middle / 2));

        low = std::max(std::numeric_limits<double>::lowest(), (middle / 2 - reach) * 2);
        high = std::min(std::numeric_limits<double>::max(), (middle / 2 + reach) * 2);
    }

    return low < high ? Scale{low, high} : Scale::fitted(kept);
}

// The levels of `contours`: those asked for, or else the major ticks (ten at most) strictly within
// a scale fitted to the finite values of its field.
std::vector<double> leveled(const Contours& contours) {
    if (!contours.levels.empty()) return contours.levels;

    const Scale fitted = Scale::fitted(finite_of(contours.field));
    std::vector<double> made;

    for (const double one : fitted.ticks(10))
        if (one > fitted.low() && one < fitted.high()) made.push_back(one);

    return made;
}

// What the layout found: the scales, their ticks and labels, the margins and the rectangle of the
// data; for a plot over a field the scale its values are read on, and the ticks and labels of that
// scale (those of the bar of colors, or of six at most without one).
struct Laid {
    Layout layout;
    std::vector<double> x_ticks;
    std::vector<double> y_ticks;
    std::vector<double> x_minor;
    std::vector<double> y_minor;
    Labels x_labels;
    Labels y_labels;
    Rect box;
    Scale range;
    std::vector<double> value_ticks;
    Labels value_labels;
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
// room the ticks are spread over only shrinking, so that it cannot swing between two. Over a field
// the axes not given take its scales as they are, so that a heat map fills the data; the bar of
// colors of a heat stands right of the data, as high, its ticks as far apart as those of y, the
// right margin wide enough for it, its labels and the title of the heat. Throws as `shaped` for a
// field of the heat or of contours.
Laid laying(const Plot& plot, const Font& font, Rect area) {
    const double letters = plot.letters;
    const double gap = letters * 0.4;
    const double tick = letters * 0.4;
    const text::Language language = plot.language.value_or(text::current());
    Laid made;

    std::vector<double> x_values = gathered(plot.series, true, plot.x.spacing);
    std::vector<double> y_values = gathered(plot.series, false, plot.y.spacing);

    for (const Block& block : blocked(plot, plot.y.spacing == Spacing::linear ? 0 : std::numeric_limits<double>::quiet_NaN())) {
        for (const double one : {block.left, block.right})
            if (std::isfinite(one) && (plot.x.spacing == Spacing::linear || one > 0)) x_values.push_back(one);

        for (const double one : {block.foot, block.top})
            if (std::isfinite(one) && (plot.y.spacing == Spacing::linear || one > 0)) y_values.push_back(one);
    }

    const Field* field = ground(plot);

    if (plot.heat) shaped(plot.heat->field);

    for (const Contours& one : plot.contours) shaped(one.field);

    const bool bar = plot.heat && plot.heat->bar;

    if (field) {
        made.range = ranged(plot);
        made.value_ticks = made.range.ticks(6);
        made.value_labels = labelled(made.range, made.value_ticks, language);
    }

    made.layout.x = plot.x.scale.value_or(field ? field->x : Scale::fitted(x_values, plot.x.spacing));
    made.layout.y = plot.y.scale.value_or(field ? field->y : Scale::fitted(y_values, plot.y.spacing));

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
    std::vector<double> values_before;
    double width_before = -1;

    for (int round = 0; round < 6; ++round) {
        const Spread down = spread(ys, std::max(0.0, area.high - top - bottom), 2.5 * letters);

        made.y_ticks = down.ticks;
        made.y_minor = down.minor;
        made.y_labels = labelled(ys, made.y_ticks, language);

        double widest = 0;

        for (const std::string& one : made.y_labels.said) widest = std::max(widest, wide(font, one, letters));

        double beside = 0;

        if (bar) {
            const Spread colors = spread(made.range, std::max(0.0, area.high - top - bottom), 2.5 * letters);
            double longest_value = 0;

            made.value_ticks = colors.ticks;
            made.value_labels = labelled(made.range, made.value_ticks, language);

            for (const std::string& one : made.value_labels.said) longest_value = std::max(longest_value, wide(font, one, letters));

            beside = gap * 2 + letters + tick + gap + longest_value + gap + (plot.heat->title.empty() ? 0 : letters * 1.2 + gap);
        }

        const bool powered = made.y_labels.power != 0 || (bar && made.value_labels.power != 0);

        left = gap + (plot.y.title.empty() ? 0 : letters * 1.2 + gap) + widest + gap + tick;
        top = gap + (plot.title.empty() ? letters * 0.5 : letters * 1.3 + gap) + (powered ? letters + gap : 0);

        const Spread across = spread(xs, room, least);

        made.x_ticks = across.ticks;
        made.x_minor = across.minor;
        made.x_labels = labelled(xs, made.x_ticks, language);

        double longest = 0;

        for (const std::string& one : made.x_labels.said) longest = std::max(longest, wide(font, one, letters));

        const std::string power = made.x_labels.power != 0 ? "×10" + raised(made.x_labels.power) : std::string{};
        const double last = made.x_labels.said.empty() ? 0 : wide(font, made.x_labels.said.back(), letters) / 2;

        right = std::max(std::max(gap * 2, last + gap) + (power.empty() ? 0 : wide(font, power, letters) + gap), beside);
        bottom = tick + gap + letters + (plot.x.title.empty() ? 0 : gap + letters * 1.2) + gap;
        width = std::max(0.0, area.wide - left - right);

        if (made.x_ticks == x_before && made.y_ticks == y_before && made.value_ticks == values_before && width == width_before) break;

        x_before = made.x_ticks;
        y_before = made.y_ticks;
        values_before = made.value_ticks;
        width_before = width;
        room = std::min(room, width);
        least = std::max(least, longest + letters);
    }

    made.layout.data = Rect{Point{area.at.x + left, area.at.y + top}, width, std::max(0.0, area.high - top - bottom)};
    made.layout.x_ticks = made.x_ticks;
    made.layout.y_ticks = made.y_ticks;

    if (bar) made.layout.bar = Rect{Point{made.layout.data.at.x + made.layout.data.wide + gap * 2, made.layout.data.at.y}, letters, made.layout.data.high};

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

// The color of the series `index` of `plot`: its own, or the theme's palette in turn.
Color colored(const Plot& plot, std::size_t index) {
    return plot.series[index].color.value_or(plot.theme.palette.empty() ? plot.theme.ink : plot.theme.palette[index % plot.theme.palette.size()]);
}

// `value` on the axis of `scale` with `ticks` in `language` for a readout, `more` decimals beyond
// its labels: on a straight axis by its common power, written after the number; on a logarithmic
// one with three digits that count, and `more` beyond them.
std::string read_out(const Scale& scale, std::span<const double> ticks, double value, int more, const text::Language& language) {
    if (scale.spacing() == Spacing::logarithmic) {
        const double shown = 2 + more - std::floor(std::log10(std::abs(value)));

        return signed_(text::number(value, std::isfinite(shown) ? static_cast<int>(std::clamp(shown, 0.0, 17.0)) : 0, language));
    }

    const Written form = written_as(ticks);
    const std::string number = signed_(text::number(value / std::pow(10.0, form.power), std::min(form.decimals + more, 17), language));

    return form.power != 0 ? number + "×10" + raised(form.power) : number;
}

// Whether the finite points of `series` go along x without turning back, as samples in time do.
bool ordered(const Series& series) {
    std::optional<double> last;

    for (const Point& one : series.points) {
        if (!std::isfinite(one.x) || !std::isfinite(one.y)) continue;

        if (last && one.x < *last) return false;

        last = one.x;
    }

    return true;
}

// The finite point of `series` whose x is nearest `x`, the first of equals; none without one.
std::optional<std::size_t> along(const Series& series, double x) {
    std::optional<std::size_t> made;
    double least = std::numeric_limits<double>::infinity();

    for (std::size_t at = 0; at < series.points.size(); ++at) {
        const Point& one = series.points[at];
        const double away = std::abs(one.x - x);

        if (std::isfinite(one.x) && std::isfinite(one.y) && (!made || away < least)) {
            made = at;
            least = away;
        }
    }

    return made;
}

// The finite point of `series` nearest the pixel `pixel` as `layout` places it, and how far it is
// in pixels; the first of equals; none without a finite point.
std::optional<std::pair<std::size_t, double>> nearest_of(const Layout& layout, const Series& series, Point pixel) {
    std::optional<std::pair<std::size_t, double>> made;

    for (std::size_t point = 0; point < series.points.size(); ++point) {
        const Point& one = series.points[point];
        const double away = std::hypot(at(layout.x, one.x, layout.data, true) - pixel.x, at(layout.y, one.y, layout.data, false) - pixel.y);

        if (std::isfinite(away) && (!made || away < made->second)) made = std::pair{point, away};
    }

    return made;
}

// The most pixels a side of the image of a heat map has; a larger area takes it stretched.
constexpr double most_side = 1024;

// The steps the palette of a heat map is read through.
constexpr std::size_t shades = 1024;

// The heat `heat` drawn with `pen` over the data of `layout`, its values read on `range`: an image
// of the pixels of the data (on whole pixels, so that it goes onto them one for one, where a side
// measures no more than `most_side`; else of `most_side` stretched over it), each the color of the
// field's value at its middle, from the nearest node or smoothed as the heat asks; clear outside
// the field and where it has no value. Values beyond the range take the color of its end.
void heated(Pen& pen, const Heat& heat, const Scale& range, const Layout& layout) {
    const Rect& data = layout.data;
    const Field& field = heat.field;
    double left = std::floor(data.at.x);
    double top = std::floor(data.at.y);
    double wide_ = std::ceil(data.at.x + data.wide) - left;
    double high_ = std::ceil(data.at.y + data.high) - top;

    if (!(wide_ <= most_side)) {
        left = data.at.x;
        wide_ = data.wide;
    }

    if (!(high_ <= most_side)) {
        top = data.at.y;
        high_ = data.high;
    }

    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(wide_) || !std::isfinite(high_) || !(wide_ > 0) || !(high_ > 0)) return;

    const auto columns = static_cast<std::uint32_t>(std::clamp(std::ceil(wide_), 1.0, most_side));
    const auto rows = static_cast<std::uint32_t>(std::clamp(std::ceil(high_), 1.0, most_side));
    const double last_column = static_cast<double>(field.columns - 1);
    const double last_row = static_cast<double>(field.rows - 1);
    std::vector<double> across(columns);
    std::vector<double> up(rows);

    for (std::uint32_t at = 0; at < columns; ++at) {
        const double pixel = left + (at + 0.5) * wide_ / columns;

        across[at] = field.x.placed(layout.x.valued(pixel, data.at.x, data.at.x + data.wide), 0, last_column);
    }

    for (std::uint32_t at = 0; at < rows; ++at) {
        const double pixel = top + (at + 0.5) * high_ / rows;

        up[at] = field.y.placed(layout.y.valued(pixel, data.at.y + data.high, data.at.y), 0, last_row);
    }

    std::vector<std::array<std::byte, 4>> colors(shades);

    for (std::size_t at = 0; at < shades; ++at) {
        const Color one = heat.palette.at(static_cast<double>(at) / static_cast<double>(shades - 1));
        const auto byte = [](float channel) { return static_cast<std::byte>(std::lround(std::clamp(static_cast<double>(channel), 0.0, 1.0) * 255)); };

        colors[at] = {byte(one.r), byte(one.g), byte(one.b), byte(one.a)};
    }

    Bytes rgba(static_cast<std::size_t>(columns) * rows * 4, std::byte{0});

    for (std::uint32_t row = 0; row < rows; ++row) {
        if (!(up[row] >= 0 && up[row] <= last_row)) continue;

        for (std::uint32_t column = 0; column < columns; ++column) {
            if (!(across[column] >= 0 && across[column] <= last_column)) continue;

            const double share = range.placed(sample(field, across[column], up[row], heat.smooth), 0, 1);

            if (std::isnan(share)) continue;

            const auto shade = static_cast<std::size_t>(std::lround(std::clamp(share, 0.0, 1.0) * static_cast<double>(shades - 1)));

            std::copy(colors[shade].begin(), colors[shade].end(), rgba.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(row) * columns + column) * 4));
        }
    }

    pen.draw(media::Pixels{media::Size{columns, rows}, {media::Still{std::move(rgba), {}}}}, Rect{Point{left, top}, wide_, high_});
}

// The label of the contour at `level` among `levels`, in `language`: with the decimals of the
// step between the first two levels and their common power, or, for a level alone, three digits
// that count.
std::string level_text(double level, std::vector<double> levels, const text::Language& language) {
    std::sort(levels.begin(), levels.end());
    levels.erase(std::unique(levels.begin(), levels.end()), levels.end());

    return levels.size() >= 2 ? read_out(Scale{}, levels, level, 0, language) : read_out(Scale{1, 10, Spacing::logarithmic}, levels, level, 0, language);
}

// The contours of `plot` drawn with `pen` over the data of `laid` (the caller clips to it): the
// lines of each set at its levels in its color (the ink, partly seen through, by default), no
// wider than `reach`, thinned as series are; labelled, where asked, with the level in the middle of
// each line long enough to hold three of its label, where the label lies within the data and clear
// of the others (64 at most), the lines broken around the labels.
void contoured_lines(Pen& pen, const Font& font, const Plot& plot, const Laid& laid, double reach) {
    constexpr std::size_t most_labels = 64;
    const Rect& data = laid.layout.data;
    const double size = plot.letters * 0.85;
    const double gap = plot.letters * 0.4;
    const text::Language language = plot.language.value_or(text::current());

    struct Set {
        Path lines;
        Color color;
        double width = 1;
    };

    struct Label {
        Rect box;
        std::string text;
        Color color;
    };

    std::vector<Set> sets;
    std::vector<Label> labels;

    for (const Contours& contours : plot.contours) {
        const std::vector<double> levels = leveled(contours);
        Set set;

        set.color = contours.color.value_or(Color{plot.theme.ink.r, plot.theme.ink.g, plot.theme.ink.b, plot.theme.ink.a * 0.7F});
        set.width = std::min(contours.width, reach);

        for (const double level : levels) {
            const std::string text = contours.labels ? level_text(level, levels, language) : std::string{};
            const double box_wide = text.empty() ? 0 : wide(font, text, size) + gap;
            const double box_high = size * 1.2;

            for (const std::vector<Point>& line : contoured(contours.field, level)) {
                std::vector<Point> run;
                const auto ended = [&] {
                    const std::vector<Point> kept = thinned(run, thinning);
                    double length = 0;

                    for (std::size_t at = 0; at < kept.size(); ++at) {
                        if (at == 0) set.lines.move(kept[at]);
                        else set.lines.line(kept[at]);

                        if (at > 0) length += std::hypot(kept[at].x - kept[at - 1].x, kept[at].y - kept[at - 1].y);
                    }

                    if (!text.empty() && labels.size() < most_labels && length >= 3 * box_wide) {
                        double walked = 0;

                        for (std::size_t at = 1; at < kept.size(); ++at) {
                            const double piece = std::hypot(kept[at].x - kept[at - 1].x, kept[at].y - kept[at - 1].y);

                            if (walked + piece >= length / 2 && piece > 0) {
                                const double part = (length / 2 - walked) / piece;
                                const Point middle{kept[at - 1].x + (kept[at].x - kept[at - 1].x) * part, kept[at - 1].y + (kept[at].y - kept[at - 1].y) * part};
                                const Rect box{Point{middle.x - box_wide / 2, middle.y - box_high / 2}, box_wide, box_high};
                                const bool within = box.at.x >= data.at.x && box.at.y >= data.at.y && box.at.x + box.wide <= data.at.x + data.wide && box.at.y + box.high <= data.at.y + data.high;
                                const bool clear = std::none_of(labels.begin(), labels.end(), [&](const Label& other) {
                                    return box.at.x < other.box.at.x + other.box.wide && other.box.at.x < box.at.x + box.wide && box.at.y < other.box.at.y + other.box.high && other.box.at.y < box.at.y + box.high;
                                });

                                if (within && clear) labels.push_back(Label{box, text, Color{set.color.r, set.color.g, set.color.b, 1}});

                                break;
                            }

                            walked += piece;
                        }
                    }

                    run.clear();
                };

                for (const Point& value : line) {
                    const Point pixel{at(laid.layout.x, value.x, data, true), at(laid.layout.y, value.y, data, false)};

                    if (std::isfinite(pixel.x) && std::isfinite(pixel.y)) run.push_back(pixel);
                    else ended();
                }

                ended();
            }
        }

        sets.push_back(std::move(set));
    }

    pen.save();

    if (!labels.empty()) {
        Path around;

        around.rectangle(data);

        for (const Label& label : labels) around.rectangle(label.box);

        pen.clip(around, drawing::Rule::evenodd);
    }

    for (const Set& set : sets) {
        Stroke style;

        style.width = set.width;
        style.join = drawing::Join::round;
        style.cap = drawing::Cap::round;
        pen.stroke(set.lines, set.color, style);
    }

    pen.restore();

    for (const Label& label : labels)
        written(pen, font, label.text, size, Point{label.box.at.x + label.box.wide / 2, label.box.at.y + label.box.high / 2}, Align::center, Baseline::middle, label.color);
}

// The bar of colors of the heat of `plot` drawn with `pen` at the bar of `laid`: the palette from
// the bottom up, a frame, the ticks of the values on its right with their labels, their common
// power above it, and the title of the heat at the right of `area`, turned to read upward.
void barred(Pen& pen, const Font& font, const Plot& plot, const Laid& laid, Rect area) {
    const Rect& bar = *laid.layout.bar;
    const double letters = plot.letters;
    const double gap = letters * 0.4;
    const double tick = letters * 0.4;
    const Color& ink = plot.theme.ink;
    const auto rows = static_cast<std::uint32_t>(std::clamp(std::ceil(bar.high), 1.0, most_side));
    Bytes rgba(static_cast<std::size_t>(rows) * 4);

    for (std::uint32_t row = 0; row < rows; ++row) {
        const Color one = plot.heat->palette.at(1 - (row + 0.5) / rows);
        const auto byte = [](float channel) { return static_cast<std::byte>(std::lround(std::clamp(static_cast<double>(channel), 0.0, 1.0) * 255)); };

        rgba[row * 4] = byte(one.r);
        rgba[row * 4 + 1] = byte(one.g);
        rgba[row * 4 + 2] = byte(one.b);
        rgba[row * 4 + 3] = byte(one.a);
    }

    pen.draw(media::Pixels{media::Size{1, rows}, {media::Still{std::move(rgba), {}}}}, bar);

    const double left = std::round(bar.at.x) + 0.5;
    const double right = std::round(bar.at.x + bar.wide) - 0.5;
    const double top = std::round(bar.at.y) + 0.5;
    const double bottom = std::round(bar.at.y + bar.high) - 0.5;
    Path frame;
    Stroke thin;

    thin.width = 1;
    frame.rectangle(Rect{Point{left, top}, right - left, bottom - top});
    pen.stroke(frame, ink, thin);

    for (std::size_t index = 0; index < laid.value_ticks.size(); ++index) {
        const double where = std::round(laid.range.placed(laid.value_ticks[index], bar.at.y + bar.high, bar.at.y) - 0.5) + 0.5;

        ruled(pen, Point{right, where}, Point{right + tick, where}, 1, ink);
        written(pen, font, laid.value_labels.said[index], letters, Point{right + tick + gap, where}, Align::left, Baseline::middle, ink);
    }

    if (laid.value_labels.power != 0) written(pen, font, "×10" + raised(laid.value_labels.power), letters, Point{left, top - gap}, Align::left, Baseline::bottom, ink);

    if (!plot.heat->title.empty()) {
        pen.save();
        pen.place(Matrix::turned(-std::numbers::pi / 2).then(Matrix::moved(area.at.x + area.wide - gap, (top + bottom) / 2)).then(pen.placed()));
        written(pen, font, plot.heat->title, letters, Point{0, 0}, Align::center, Baseline::bottom, ink);
        pen.restore();
    }
}

// The readout of `plot` at its cursor, drawn with `pen` over `laid`: a line across the data at the
// cursor (unless not wanted), a ring around each point read and a box of their values beside the
// cursor, on the side where it fits. As `Plot::readout` asks: the point of each series nearest the
// cursor's x with its value (for a series along x, or every series when `Readout::across`), the
// point nearest the cursor of any other series with both its values, or only the one point of all
// nearest the cursor (`Readout::nearest`); a series of bars, unless `Readout::nearest`, the bar
// under the cursor's x with its stretch and its value. A series along x heads the box with the
// cursor's x. Over a field (of the heat, or of the first contours) the value of the field under
// the cursor follows, in the color it has on the map, the box headed by both values of the cursor
// unless a series headed it. A cursor outside the data reads nothing.
void read(const Plot& plot, Pen& pen, const Font& font, const Laid& laid) {
    if (!plot.cursor) return;

    const std::optional<Point> value = laid.layout.at(*plot.cursor);

    if (!value) return;

    const Rect& data = laid.layout.data;
    const double letters = plot.letters;
    const double gap = letters * 0.4;
    const double swatch = letters * 1.2;
    const text::Language language = plot.language.value_or(text::current());
    const int more = std::clamp(plot.readout_digits, 0, 17);
    const auto across_text = [&](double one) { return read_out(laid.layout.x, laid.x_ticks, one, more, language); };
    const auto up_text = [&](double one) { return read_out(laid.layout.y, laid.y_ticks, one, more, language); };

    struct Row {
        std::string text;
        std::optional<Color> color;
    };

    std::vector<Row> rows;
    std::vector<std::pair<Point, Color>> rings;
    bool headed = false;

    const auto shown = [&](std::size_t index, std::size_t point, bool both) {
        const Series& one = plot.series[index];
        const Point& read_point = one.points[point];
        std::string text = one.name.empty() ? std::string{} : one.name + "  ";

        text += both ? across_text(read_point.x) + "; " + up_text(read_point.y) : up_text(read_point.y);
        rows.push_back(Row{std::move(text), colored(plot, index)});
        rings.emplace_back(Point{at(laid.layout.x, read_point.x, data, true), at(laid.layout.y, read_point.y, data, false)}, colored(plot, index));
    };

    if (plot.readout == Readout::nearest) {
        if (const std::optional<Nearest> near = laid.layout.nearest(plot, *plot.cursor, std::numeric_limits<double>::infinity())) shown(near->series, near->point, true);
    } else {
        const std::vector<Block> blocks = blocked(plot, laid.layout.y.spacing() == Spacing::linear ? 0 : laid.layout.y.low());

        for (std::size_t index = 0; index < plot.series.size(); ++index) {
            const Series& one = plot.series[index];
            const bool across = plot.readout == Readout::across || ordered(one);
            std::optional<std::size_t> found;

            if (one.shape == Shape::bars) {
                const auto under = std::find_if(blocks.begin(), blocks.end(), [&](const Block& block) {
                    return block.series == index && std::min(block.left, block.right) <= value->x && value->x < std::max(block.left, block.right);
                });

                if (under == blocks.end()) continue;

                rows.push_back(Row{(one.name.empty() ? std::string{} : one.name + "  ") + "[" + across_text(std::min(under->left, under->right)) + "; " + across_text(std::max(under->left, under->right)) + ")  " + up_text(one.points[under->point].y), colored(plot, index)});
                rings.emplace_back(Point{at(laid.layout.x, (under->left + under->right) / 2, data, true), at(laid.layout.y, under->top, data, false)}, colored(plot, index));

                continue;
            }

            if (across) {
                found = along(one, value->x);
            } else if (const auto near = nearest_of(laid.layout, one, *plot.cursor)) {
                found = near->first;
            }

            if (!found) continue;

            if (across && !headed) {
                rows.insert(rows.begin(), Row{(plot.x.title.empty() ? std::string{} : plot.x.title + "  ") + across_text(value->x), std::nullopt});
                headed = true;
            }

            shown(index, *found, !across);
        }
    }

    if (const Field* field = ground(plot)) {
        const double here = field->value(*value, !plot.heat || plot.heat->smooth);

        if (std::isfinite(here)) {
            if (!headed) rows.insert(rows.begin(), Row{across_text(value->x) + "; " + up_text(value->y), std::nullopt});

            const std::string named = plot.heat && !plot.heat->title.empty() ? plot.heat->title + "  " : std::string{};
            const std::optional<Color> shade = plot.heat ? std::optional<Color>{plot.heat->palette.at(laid.range.placed(here, 0, 1))} : std::nullopt;

            rows.push_back(Row{named + read_out(laid.range, laid.value_ticks, here, more, language), shade});
        }
    }

    const Color& ink = plot.theme.ink;

    if (plot.cursor_line) {
        const double where = std::round(plot.cursor->x - 0.5) + 0.5;

        ruled(pen, Point{where, std::round(data.at.y) + 0.5}, Point{where, std::round(data.at.y + data.high) - 0.5}, 1, Color{ink.r, ink.g, ink.b, ink.a * 0.5F});
    }

    Stroke ring;

    ring.width = 1.5;

    for (const auto& [where, color] : rings) {
        if (!std::isfinite(where.x) || !std::isfinite(where.y)) continue;

        Path circle;

        circle.arc(where, letters * 0.35, 0, 2 * std::numbers::pi);
        circle.close();
        pen.stroke(circle, color, ring);
    }

    if (rows.empty()) return;

    const double row = letters * 1.3;
    double widest = 0;

    for (const Row& one : rows) widest = std::max(widest, wide(font, one.text, letters) + (one.color ? swatch + gap : 0));

    const double box_wide = gap + widest + gap;
    const double box_high = gap + row * static_cast<double>(rows.size()) + gap / 2;
    const bool rightward = plot.cursor->x + gap * 2 + box_wide <= data.at.x + data.wide;
    const Rect box{Point{rightward ? plot.cursor->x + gap * 2 : std::max(data.at.x, plot.cursor->x - gap * 2 - box_wide), data.at.y + gap}, box_wide, box_high};
    Path backing;
    Stroke thin;

    thin.width = 1;
    backing.rectangle(box);
    pen.fill(backing, plot.theme.backing);
    pen.stroke(backing, plot.theme.grid, thin);

    for (std::size_t index = 0; index < rows.size(); ++index) {
        const Row& one = rows[index];
        const double middle = box.at.y + gap + row * (static_cast<double>(index) + 0.5);
        double left = box.at.x + gap;

        if (one.color) {
            ruled(pen, Point{left, middle}, Point{left + swatch, middle}, 2, *one.color);
            left += swatch + gap;
        }

        written(pen, font, one.text, letters, Point{left, middle}, Align::left, Baseline::middle, ink);
    }
}

}

// The values under the pixel `pixel`, x and y, or none where it lies outside the data.
std::optional<Point> Layout::at(Point pixel) const {
    if (!(pixel.x >= data.at.x && pixel.x <= data.at.x + data.wide && pixel.y >= data.at.y && pixel.y <= data.at.y + data.high)) return std::nullopt;

    return Point{x.valued(pixel.x, data.at.x, data.at.x + data.wide), y.valued(pixel.y, data.at.y + data.high, data.at.y)};
}

// The point of the series of `plot` nearest the pixel `pixel` as this layout places it, within
// `within` pixels: the series, the point and the distance; the first of equals; none where no
// finite point lies so near.
std::optional<Nearest> Layout::nearest(const Plot& plot, Point pixel, double within) const {
    std::optional<Nearest> made;

    for (std::size_t index = 0; index < plot.series.size(); ++index) {
        const auto near = nearest_of(*this, plot.series[index], pixel);

        if (near && near->second <= within && (!made || near->second < made->distance)) made = Nearest{index, near->first, near->second};
    }

    return made;
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

// The value at the pixel `pixel` between `from` (the low end) and `to` (the high end), the
// reverse of `placed`: the ends weighed by where the pixel lies, so that no difference of them
// overflows; beyond the ends as far as the numbers go (endless past them).
double Scale::valued(double pixel, double from, double to) const {
    const double share = (pixel - from) / (to - from);

    if (spacing_ == Spacing::logarithmic) return std::pow(10.0, std::log10(low_) * (1 - share) + std::log10(high_) * share);

    return low_ * (1 - share) + high_ * share;
}

// The scale `factor` times nearer around `about`: each end drawn towards it to a `factor`th of its
// distance (away from it, for a factor below one), on a logarithmic scale by the logarithms. Throws
// Trouble malformed where the ends come out as no scale: leaving the numbers, meeting (an endless
// factor), out of order (a factor not above zero) or not numbers (an `about` that is not a finite
// number, or not above zero on a logarithmic scale).
Scale Scale::zoomed(double about, double factor) const {
    if (spacing_ == Spacing::logarithmic) {
        const double middle = std::log10(about);

        return Scale{std::pow(10.0, middle - (middle - std::log10(low_)) / factor), std::pow(10.0, middle + (std::log10(high_) - middle) / factor), spacing_};
    }

    return Scale{about - (about / 2 - low_ / 2) / factor * 2, about + (high_ / 2 - about / 2) / factor * 2, spacing_};
}

// The scale whose values lie `pixels` further along between the pixels `from` and `to` (a drag of
// the plot by so many pixels): its ends the values that were that many pixels before them. Throws
// Trouble malformed where they leave the numbers.
Scale Scale::shifted(double pixels, double from, double to) const {
    return Scale{valued(from - pixels, from, to), valued(to - pixels, from, to), spacing_};
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

// The plot drawn with `pen` in `area`, in `font`: the paper, the grid, clipped to the data the heat
// map and the contours (`heated`, `contoured_lines`), then the series (the bars of each first, as
// one shape so that bars meeting leave no seam between them, half seen through when
// `Grouping::over`; then lines, steps as lines turning square at each point, thinned to the points
// they need within `thinning`, and marks; lines and marks no wider than twice the diagonal of the
// data, past which they only bury the plot, so that a stroke of any width costs no more than that),
// the frame with its ticks and labels, the common powers, the titles, the bar of colors of the
// heat, the legend (its samples no wider than half a letter, their marks than a letter, bars as a
// square) and the readout. Lines of one pixel stand on the middles of pixels, so that they come out
// sharp. Returns the layout.
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

    if (heat) heated(pen, *heat, laid.range, laid.layout);

    if (!contours.empty()) contoured_lines(pen, font, *this, laid, reach);

    const std::vector<Block> blocks = blocked(*this, laid.layout.y.spacing() == Spacing::linear ? 0 : laid.layout.y.low());

    for (std::size_t index = 0; index < series.size(); ++index) {
        Path bars_of;

        for (const Block& block : blocks) {
            if (block.series != index) continue;

            const double x0 = at(laid.layout.x, block.left, data, true);
            const double x1 = at(laid.layout.x, block.right, data, true);
            const double y0 = at(laid.layout.y, block.foot, data, false);
            const double y1 = at(laid.layout.y, block.top, data, false);

            if (std::isfinite(x0) && std::isfinite(x1) && std::isfinite(y0) && std::isfinite(y1)) bars_of.rectangle(Rect{Point{std::min(x0, x1), std::min(y0, y1)}, std::abs(x1 - x0), std::abs(y1 - y0)});
        }

        Color color = colored(*this, index);

        if (bars == Grouping::over) color.a *= 0.6F;

        pen.fill(bars_of, color);
    }

    for (std::size_t index = 0; index < series.size(); ++index) {
        const Series& one = series[index];
        const Color color = colored(*this, index);
        std::vector<Point> placed;

        for (const Point& point : one.points) placed.push_back(Point{at(laid.layout.x, point.x, data, true), at(laid.layout.y, point.y, data, false)});

        const std::vector<Point> points = placed;

        if (one.shape == Shape::steps) {
            std::vector<Point> stairs;

            for (std::size_t at = 0; at < placed.size(); ++at) {
                const bool here = std::isfinite(placed[at].x) && std::isfinite(placed[at].y);
                const bool before = at > 0 && std::isfinite(placed[at - 1].x) && std::isfinite(placed[at - 1].y);

                if (here && before) stairs.push_back(Point{placed[at].x, placed[at - 1].y});

                stairs.push_back(placed[at]);
            }

            placed = std::move(stairs);
        }

        if (one.line && one.shape != Shape::bars) {
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

        for (const Point& point : points)
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

    if (laid.layout.bar) barred(pen, font, *this, laid, area);

    if (laid.layout.legend == Corner::none) {
        read(*this, pen, font, laid);

        return laid.layout;
    }

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

        if (one.shape == Shape::bars) {
            Path swatch;

            swatch.rectangle(Rect{Point{origin.x + gap + sample / 2 - letters * 0.4, middle - letters * 0.4}, letters * 0.8, letters * 0.8});
            pen.fill(swatch, color);
        } else if (one.line) {
            ruled(pen, Point{origin.x + gap, middle}, Point{origin.x + gap + sample, middle}, std::min(one.width, letters / 2), color);
        }

        marked(pen, one.mark, std::min(one.size, letters), Point{origin.x + gap + sample / 2, middle}, color);
        written(pen, font, one.name, letters, Point{origin.x + gap + sample + gap, middle}, Align::left, Baseline::middle, ink);
        ++index;
    }

    read(*this, pen, font, laid);

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
