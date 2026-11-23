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
// size of its letters and its rectangle, drawn on a raster of 64 by 48 pixels, and the samples of a
// trace. Numbers are small (a byte) or whole (eight bytes, any double, NaN and the infinities among
// them). Troubles may come; nothing may crash, hang or touch memory out of bounds. Ticks are no
// more than asked (two at least) and lie within their scale; labels are as many as ticks; the
// layout keeps the data within its rectangle; a trace's span ends at its last sample. The input is
// read in a fixed order, so that it means the same to every compiler.
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
            switch (reading.byte() % 10) {
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
            default: values.clear(); break;
            }
        } catch (const tal::Trouble&) {
        }
    }

    (void)raster.pixels();

    return 0;
}
