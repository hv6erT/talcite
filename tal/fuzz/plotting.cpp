#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <optional>
#include <vector>

#include "tal/drawing.hpp"
#include "tal/fonts.hpp"
#include "tal/plotting.hpp"
#include "tal/text.hpp"
#include "tal/trouble.hpp"

// One input of libFuzzer as scales, data and a plot: the ends of a scale and its kind, how many
// ticks are asked for, the values a scale is fitted to, the series of a plot with their marks, the
// size of its letters and its rectangle, a cursor over it and how it reads (E9), drawn on a raster
// of 64 by 48 pixels, the samples of a trace, scales placed and read back, zoomed and shifted, and
// values binned into histograms, series as bars or steps grouped every way (E10); the bars of more
// than 4096 bins are neither made nor plotted, since the plots of an input would otherwise hold
// millions of points and cost the time of their drawing, not of a fault; and fields of up to 17 by
// 17 nodes read at points, cut into contours and plotted as heat maps and contours with palettes of
// their own, ranges, middles and levels (E11). Numbers are small (a byte) or whole (eight bytes,
// any double, NaN and the infinities among them). Troubles may come; nothing may crash, hang or
// touch memory out of bounds. Ticks are no more than asked (two at least) and lie within their
// scale; labels are as many as ticks; the layout keeps the data within its rectangle; a trace's
// span ends at its last sample; the bins of values hold each finite one once, and a histogram has a
// bar for each bin; a contour has two points at least. The input is read in a fixed order, so that
// it means the same to every compiler.
namespace {

using namespace tal::drawing;
using namespace tal::plotting;

// The input read from the start.
class Reading {
public:
    Reading(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    // Whether anything is left.
    [[nodiscard]] bool left() const { return at_ < size_; }

    // The next byte, 0 when none is left.
    std::uint8_t byte() { return at_ < size_ ? data_[at_++] : 0; }

    // The next number: small after an even byte, whole after an odd one.
    double number() {
        if ((byte() & 1) == 0) return (static_cast<double>(byte()) - 64) * 0.75;

        double made = 0;
        std::uint8_t bytes[8] = {};

        for (std::uint8_t& one : bytes) one = byte();

        std::memcpy(&made, bytes, sizeof made);

        return made;
    }

    // The next point.
    Point point() {
        const double x = number();

        return Point{x, number()};
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t at_ = 0;
};

// The next scale of `reading`, none where its ends are malformed.
std::optional<Scale> scale(Reading& reading) {
    const double low = reading.number();
    const double high = reading.number();
    const Spacing spacing = reading.byte() % 2 == 0 ? Spacing::linear : Spacing::logarithmic;

    try {
        return Scale{low, high, spacing};
    } catch (const tal::Trouble&) {
        return std::nullopt;
    }
}

// The next field of `reading`: its scales (0 to 1 where one is malformed), from 2 to 17 columns
// and rows (or, now and then, one column or none, which is malformed), and a value for each node,
// the last few left out now and then.
Field field(Reading& reading) {
    Field made;

    made.x = scale(reading).value_or(Scale{});
    made.y = scale(reading).value_or(Scale{});

    const std::uint8_t shape = reading.byte();

    made.columns = shape % 32 == 0 ? shape % 64 / 32 : 2 + shape % 16;
    made.rows = 2 + reading.byte() % 16;

    std::size_t many = made.columns * made.rows;

    if (shape & 0x80) many -= std::min<std::size_t>(many, reading.byte() % 3);

    for (std::size_t at = 0; at < many && reading.left(); ++at) made.values.push_back(reading.byte() & 1 ? reading.number() : static_cast<double>(at % 7));

    while (made.values.size() < many) made.values.push_back(static_cast<double>(made.values.size() % 5));

    return made;
}

// The ticks of `one` checked: no more than `most` (two at least), within the scale, labelled one
// by one; then its minor ticks within it too.
void ticked(const Scale& one, std::size_t most) {
    const std::vector<double> ticks = one.ticks(most);
    const double slack = 1e-6 * std::max(std::abs(one.low()), std::abs(one.high()));

    if (ticks.size() > std::max<std::size_t>(most, 2)) std::terminate();

    for (const double tick : ticks)
        if (!std::isfinite(tick) || tick < one.low() - slack || tick > one.high() + slack) std::terminate();

    if (labelled(one, ticks, tal::text::Language{"pl-PL"}).said.size() != ticks.size()) std::terminate();

    for (const double tick : one.minor(most))
        if (!std::isfinite(tick) || tick < one.low() - slack || tick > one.high() + slack) std::terminate();
}

}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const Font font = Font::builtin();
    Reading reading{data, size};
    Raster raster{tal::media::Size{64, 48}};
    Pen pen{raster};
    Plot plot;
    std::vector<double> values;
    std::optional<Trace> trace;

    for (int operations = 0; reading.left() && operations < 64; ++operations) {
        try {
            switch (reading.byte() % 16) {
            case 0: {
                const std::optional<Scale> one = scale(reading);

                if (one) ticked(*one, reading.byte() % 40);

                break;
            }
            case 1: values.push_back(reading.number()); break;
            case 2: {
                const Spacing spacing = reading.byte() % 2 == 0 ? Spacing::linear : Spacing::logarithmic;

                ticked(Scale::fitted(values, spacing), reading.byte() % 40);
                break;
            }
            case 3: {
                Series series;

                series.name = reading.byte() % 2 == 0 ? "" : "a";
                series.line = reading.byte() % 2 == 0;
                series.mark = static_cast<Mark>(reading.byte() % 5);
                series.width = reading.number();
                series.size = reading.number();

                for (int many = reading.byte() % 16; many > 0; --many) series.points.push_back(reading.point());

                plot.series.push_back(series);
                break;
            }
            case 4: {
                const std::uint8_t which = reading.byte();
                Axis& axis = which % 2 == 0 ? plot.x : plot.y;

                axis.spacing = which & 2 ? Spacing::logarithmic : Spacing::linear;
                axis.grid = (which & 4) != 0;
                axis.title = which & 8 ? "t [s]" : "";
                axis.scale = which & 16 ? scale(reading) : std::nullopt;
                break;
            }
            case 5:
                plot.letters = reading.number();
                plot.legend = static_cast<Corner>(reading.byte() % 6);
                plot.title = reading.byte() % 2 == 0 ? "" : "Plot";
                break;
            case 6: {
                const Rect area{reading.point(), reading.number(), reading.number()};
                const Layout laid = reading.byte() % 2 == 0 ? plot.laid(font, area) : plot.drawn(pen, font, area);

                if (std::isfinite(area.at.x) && std::isfinite(area.wide) && laid.data.wide > std::max(0.0, area.wide) + 1e-6 * std::abs(area.wide)) std::terminate();

                const Point asked = reading.point();
                const std::optional<Point> under = laid.at(asked);

                if (under && (!std::isfinite(asked.x) || !std::isfinite(asked.y))) std::terminate();

                const double within = reading.number();

                (void)laid.nearest(plot, asked, within);

                break;
            }
            case 7: trace.emplace(reading.number(), reading.byte()); break;
            case 8:
                if (trace) {
                    const double time = reading.number();

                    trace->add(time, reading.number());

                    const std::optional<Scale> span = trace->span();

                    if (span && span->high() != trace->points().back().x) std::terminate();
                }

                break;
            case 10: {
                const std::optional<Scale> one = scale(reading);

                if (one) {
                    const double from = reading.number();
                    const double to = reading.number();
                    const double pixel = reading.number();
                    const double back = one->valued(one->placed(pixel, from, to), from, to);

                    (void)back;
                    (void)one->zoomed(reading.number(), reading.number());
                    (void)one->shifted(reading.number(), from, to);
                }

                break;
            }
            case 11:
                plot.cursor = reading.point();
                plot.readout = static_cast<Readout>(reading.byte() % 3);
                plot.cursor_line = reading.byte() % 2 == 0;
                plot.readout_digits = static_cast<int>(reading.byte()) - 128;
                break;
            case 12: {
                tal::plotting::Binning how;
                const std::uint8_t which = reading.byte();

                if (which & 1) how.count = reading.byte();

                if (which & 2) how.width = reading.number();

                const tal::plotting::Bins bins = tal::plotting::binned(values, how);
                std::size_t all = 0;
                std::size_t finite = 0;

                for (const std::size_t one : bins.counts) all += one;

                for (const double one : values) finite += std::isfinite(one) ? 1 : 0;

                if (all != finite) std::terminate();

                const auto tally = static_cast<tal::plotting::Tally>(reading.byte() % 3);

                if (bins.counts.size() > 4096) break;

                Series counted = tal::plotting::histogram(bins, tally);

                if (counted.points.size() != bins.counts.size()) std::terminate();

                plot.series.push_back(std::move(counted));
                break;
            }
            case 13:
                if (!plot.series.empty()) {
                    Series& last = plot.series.back();

                    last.shape = static_cast<tal::plotting::Shape>(reading.byte() % 3);
                    last.bar = reading.byte() % 2 == 0 ? std::optional<double>{} : std::optional<double>{reading.number()};
                }

                plot.bars = static_cast<tal::plotting::Grouping>(reading.byte() % 3);
                break;
            case 14: {
                Heat heat;
                const std::uint8_t which = reading.byte();

                heat.field = field(reading);
                heat.palette = which % 4 == 0 ? viridis() : which % 4 == 1 ? diverging() : which % 4 == 2 ? Palette{{Color{1, 0, 0, 1}}} : Palette{};
                heat.smooth = (which & 4) != 0;
                heat.bar = (which & 8) != 0;
                heat.title = which & 16 ? "V [J]" : "";

                if (which & 32) heat.range = scale(reading);

                if (which & 64) heat.middle = reading.number();

                (void)heat.palette.at(reading.number());
                (void)heat.field.value(reading.point(), (which & 128) != 0);
                plot.heat = std::move(heat);
                break;
            }
            case 15: {
                Contours contours;
                const std::uint8_t which = reading.byte();

                contours.field = field(reading);

                for (int many = which % 4; many > 0; --many) contours.levels.push_back(reading.number());

                contours.labels = (which & 4) != 0;
                contours.width = reading.number();

                if (which & 8) contours.color = Color{0.2F, 0.4F, 0.6F, 1};

                for (const std::vector<Point>& line : contoured(contours.field, reading.number()))
                    if (line.size() < 2) std::terminate();

                if (which & 16) plot.contours.clear();

                plot.contours.push_back(std::move(contours));
                break;
            }
            default: values.clear(); break;
            }
        } catch (const tal::Trouble&) {
        }
    }

    (void)raster.pixels();

    return 0;
}
