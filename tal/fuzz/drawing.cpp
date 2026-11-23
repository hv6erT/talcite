#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <vector>

#include "tal/drawing.hpp"
#include "tal/pixels.hpp"
#include "tal/trouble.hpp"

// One input of libFuzzer as a program of drawing on a raster of 48 by 32 pixels: each byte picks
// an operation (a piece of the path, a fill, a stroke, a clip, a save or restore, a matrix, an
// opacity, an image), and the bytes after it give its numbers, either small (a byte, as a pixel
// and a half of it) or whole (eight bytes, any double, NaN, infinities and the largest among them).
// Troubles may come (malformed, full, other); nothing may crash, hang, read or write out of
// bounds, or leak. Paint of no alpha must leave the raster as it was. The input is read in a
// fixed order (never two reads among the arguments of one call, whose order C++ leaves open), so
// that an input means the same program to every compiler.
namespace {

using namespace tal::drawing;

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

    // The next color, its alpha forced to `alpha` when given.
    Color color(float alpha = -1) {
        Color made{static_cast<float>(byte()) / 255.0F, static_cast<float>(byte()) / 255.0F, static_cast<float>(byte()) / 255.0F, static_cast<float>(byte()) / 255.0F};

        if (alpha >= 0) made.a = alpha;

        return made;
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t at_ = 0;
};

// A small image of four by three pixels from the input, of no alpha when `clear`.
tal::media::Pixels image(Reading& reading, bool clear = false) {
    tal::Bytes rgba(4 * 3 * 4);

    for (std::size_t at = 0; at < rgba.size(); ++at) rgba[at] = std::byte{clear && at % 4 == 3 ? std::uint8_t{0} : reading.byte()};

    std::vector<tal::media::Still> stills;

    stills.push_back(tal::media::Still{std::move(rgba), std::chrono::milliseconds{0}});

    return tal::media::Pixels{tal::media::Size{4, 3}, std::move(stills)};
}

// The next paint: a color, a gradient or a pattern, of no alpha when `clear`.
Paint paint(Reading& reading, bool clear) {
    const std::uint8_t kind = reading.byte() % 3;

    if (kind == 0) return reading.color(clear ? 0.0F : -1.0F);

    if (kind == 1) {
        const bool linear = reading.byte() % 2 == 0;
        const std::array<double, 6> v{reading.number(), reading.number(), reading.number(), reading.number(), reading.number(), reading.number()};
        Gradient made = linear ? Gradient::linear(Point{v[0], v[1]}, Point{v[2], v[3]}) : Gradient::radial(Point{v[0], v[1]}, v[2], Point{v[3], v[4]}, v[5]);
        const std::uint8_t stops = reading.byte() % 4;

        for (std::uint8_t one = 0; one < stops; ++one) {
            const double at = static_cast<double>(reading.byte()) / 255.0;

            made.stop(at, reading.color(clear ? 0.0F : -1.0F));
        }

        return made;
    }

    tal::media::Pixels picture = image(reading, clear);
    const bool across = reading.byte() % 2 == 0;
    const bool down = reading.byte() % 2 == 0;
    Pattern made{std::move(picture), across, down};

    if (reading.byte() % 2 == 0) made.place(Matrix{reading.number(), reading.number(), reading.number(), reading.number(), reading.number(), reading.number()});

    return made;
}

// The next style of a stroke.
Stroke style(Reading& reading) {
    Stroke made;

    made.width = reading.number();
    made.join = static_cast<Join>(reading.byte() % 3);
    made.cap = static_cast<Cap>(reading.byte() % 3);
    made.miter = reading.number();

    const std::uint8_t dashes = reading.byte() % 5;

    for (std::uint8_t one = 0; one < dashes; ++one) made.dashes.push_back(reading.number());

    made.offset = reading.number();

    return made;
}

}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    Reading reading{data, size};
    Raster raster{tal::media::Size{48, 32}, reading.byte() % 2 == 0 ? Blending::linear : Blending::srgb};
    Pen pen{raster};
    Path path;

    for (int operations = 0; reading.left() && operations < 64; ++operations) {
        try {
            switch (reading.byte() % 20) {
            case 0: path.move(reading.point()); break;
            case 1: path.line(reading.point()); break;
            case 2: {
                const std::array<Point, 2> p{reading.point(), reading.point()};

                path.quad(p[0], p[1]);
                break;
            }
            case 3: {
                const std::array<Point, 3> p{reading.point(), reading.point(), reading.point()};

                path.cubic(p[0], p[1], p[2]);
                break;
            }
            case 4: {
                const Point center = reading.point();
                const std::array<double, 3> v{reading.number(), reading.number(), reading.number()};

                path.arc(center, v[0], v[1], v[2], reading.byte() % 2 == 0);
                break;
            }
            case 5: {
                const Point center = reading.point();
                const std::array<double, 5> v{reading.number(), reading.number(), reading.number(), reading.number(), reading.number()};

                path.ellipse(center, v[0], v[1], v[2], v[3], v[4], reading.byte() % 2 == 0);
                break;
            }
            case 6: path.rectangle(Rect{reading.point(), reading.number(), reading.number()}); break;
            case 7: path.close(); break;
            case 8: {
                const Paint used = paint(reading, false);

                pen.fill(path, used, reading.byte() % 2 == 0 ? Rule::nonzero : Rule::evenodd);
                break;
            }
            case 9: {
                const Paint used = paint(reading, false);

                pen.stroke(path, used, style(reading));
                break;
            }
            case 10: pen.clip(path, reading.byte() % 2 == 0 ? Rule::nonzero : Rule::evenodd); break;
            case 11: pen.save(); break;
            case 12: pen.restore(); break;
            case 13: pen.transform(Matrix{reading.number(), reading.number(), reading.number(), reading.number(), reading.number(), reading.number()}); break;
            case 14: pen.opacity(static_cast<float>(reading.number())); break;
            case 15: pen.smoothing(reading.byte() % 2 == 0); break;
            case 16: {
                const tal::media::Pixels picture = image(reading);
                const std::array<Rect, 2> boxes{Rect{reading.point(), reading.number(), reading.number()}, Rect{reading.point(), reading.number(), reading.number()}};

                pen.draw(picture, boxes[0], boxes[1]);
                break;
            }
            case 17: {
                const Point one = reading.point();

                (void)pen.inside(path, one, reading.byte() % 2 == 0 ? Rule::nonzero : Rule::evenodd);

                const Point other = reading.point();

                (void)pen.on(path, other, style(reading));
                break;
            }
            case 18: {
                const tal::media::Pixels before = raster.pixels();

                pen.fill(path, paint(reading, true));

                const Paint used = paint(reading, true);

                pen.stroke(path, used, style(reading));

                if (raster.pixels().stills().front().rgba != before.stills().front().rgba) std::terminate();

                break;
            }
            default: path = Path{}; break;
            }
        } catch (const tal::Trouble&) {
        }
    }

    (void)raster.pixels();

    return 0;
}
