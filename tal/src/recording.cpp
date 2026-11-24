#include "recording.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>

#include "tal/crypto.hpp"
#include "tal/trouble.hpp"

#include "flattening.hpp"
#include "stroking.hpp"

// Recordings of drawing (E13): what a pen is asked to draw kept as steps, each with the state it is
// drawn in, so that it can be drawn again on any pen, at any scale, or written as SVG. A path stays
// as it was given, its curves and arcs whole, so that the SVG holds curves where the raster has
// pixels. The SVG keeps what the pen means: the matrix of each step as its transform, clips as
// groups clipped by their paths, paints as SVG paints (images inside as PNG); text, which fonts
// give as paths, comes out as curves and needs no font.
namespace tal::drawing {

namespace {

// The number `value` written as SVG takes it, the shortest that reads back the same. Throws Trouble
// malformed for a number that is not finite.
std::string number(double value) {
    if (!std::isfinite(value)) throw Trouble{Kind::malformed, "A drawing that goes beyond the numbers."};

    char made[32];
    const auto [end, failed] = std::to_chars(std::begin(made), std::end(made), value == 0 ? 0.0 : value);

    return std::string(made, end);
}

// The share `value` (an opacity) written as SVG takes it: clamped to 0 to 1, the shortest that
// reads back as the same float, as it was given.
std::string share(float value) {
    char made[32];
    const auto [end, failed] = std::to_chars(std::begin(made), std::end(made), std::clamp(value, 0.0F, 1.0F));

    return std::string(made, end);
}

// `matrix` as the transform of SVG.
std::string transformed(const Matrix& matrix) {
    return "matrix(" + number(matrix.a) + " " + number(matrix.b) + " " + number(matrix.c) + " " + number(matrix.d) + " " + number(matrix.e) + " " + number(matrix.f) + ")";
}

// A share (0 to 1, clamped) as a byte.
int byte(float one) { return static_cast<int>(std::lround(std::clamp(static_cast<double>(one), 0.0, 1.0) * 255)); }

// `color` without its alpha, as #rrggbb.
std::string hexed(const Color& color) {
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string made = "#";

    for (const float one : {color.r, color.g, color.b}) {
        const int value = byte(one);

        made += digits[static_cast<std::size_t>(value / 16)];
        made += digits[static_cast<std::size_t>(value % 16)];
    }

    return made;
}

// The still `still` of `image` as a data URL of PNG.
std::string png_url(const media::Pixels& image, std::size_t still) {
    const Bytes encoded = media::encoded(image, media::Format::png, still);

    return "data:image/png;base64," + crypto::base64(encoded);
}

// The writer of one SVG: the document so far and the number of the next definition.
struct Writer {
    std::string out;
    std::size_t next = 0;

    // The next identifier, `prefix` and a number.
    std::string named(std::string_view prefix) { return std::string(prefix) + std::to_string(next++); }

    // `path` as the data of an SVG path: lines, curves and arcs as they are, an arc of a whole turn
    // or more as two halves, since SVG draws none whose ends meet.
    static std::string data(const Path& path) {
        std::string made;
        const auto point = [&made](Point one) { made += number(one.x) + " " + number(one.y); };

        for (const Path::Piece& piece : path.pieces()) {
            if (!made.empty()) made += ' ';

            switch (piece.kind) {
            case Path::Kind::move:
                made += "M";
                point(piece.points[0]);
                break;
            case Path::Kind::line:
                made += "L";
                point(piece.points[0]);
                break;
            case Path::Kind::quad:
                made += "Q";
                point(piece.points[0]);
                made += " ";
                point(piece.points[1]);
                break;
            case Path::Kind::cubic:
                made += "C";
                point(piece.points[0]);
                made += " ";
                point(piece.points[1]);
                made += " ";
                point(piece.points[2]);
                break;
            case Path::Kind::arc: {
                const double whole = 2 * std::numbers::pi;
                const bool halved = std::abs(piece.sweep) >= whole - 1e-9;
                const std::string head = "A" + number(piece.rx) + " " + number(piece.ry) + " " + number(std::remainder(piece.turn, whole) * 180 / std::numbers::pi) + " ";
                const auto arc = [&](double sweep, Point to) {
                    made += head + (std::abs(sweep) > std::numbers::pi ? "1 " : "0 ") + (sweep > 0 ? "1 " : "0 ");
                    point(to);
                };

                if (halved) {
                    arc(piece.sweep / 2, flattening::ellipse_point(piece.points[0], piece.rx, piece.ry, piece.turn, piece.from + piece.sweep / 2));
                    made += " ";
                }

                arc(halved ? piece.sweep / 2 : piece.sweep, piece.points[1]);
                break;
            }
            case Path::Kind::close:
                made += "Z";
                break;
            }
        }

        return made;
    }

    // The attributes that paint with `paint` as `role` (fill or stroke), its definition (a
    // gradient, or a pattern of an image as PNG) written first, in the colors' space of `blending`.
    std::string painted(const Paint& paint, std::string_view role, Blending blending) {
        const std::string name{role};

        if (const Color* color = std::get_if<Color>(&paint)) return " " + name + "=\"" + hexed(*color) + "\" " + name + "-opacity=\"" + share(color->a) + "\"";

        const std::string id = named("p");

        if (const Gradient* gradient = std::get_if<Gradient>(&paint)) {
            const bool linear = gradient->shape() == Gradient::Shape::linear;

            out += linear ? "<linearGradient" : "<radialGradient";
            out += " id=\"" + id + "\" gradientUnits=\"userSpaceOnUse\"";

            if (blending == Blending::linear) out += " color-interpolation=\"linearRGB\"";

            if (linear) {
                out += " x1=\"" + number(gradient->first().x) + "\" y1=\"" + number(gradient->first().y) + "\" x2=\"" + number(gradient->second().x) + "\" y2=\"" + number(gradient->second().y) + "\">";
            } else {
                out += " fx=\"" + number(gradient->first().x) + "\" fy=\"" + number(gradient->first().y) + "\" fr=\"" + number(gradient->inner()) + "\" cx=\"" + number(gradient->second().x) + "\" cy=\"" + number(gradient->second().y) + "\" r=\"" +
                       number(gradient->outer()) + "\">";
            }

            for (const auto& [at, color] : gradient->stops())
                out += "<stop offset=\"" + number(std::clamp(at, 0.0, 1.0)) + "\" stop-color=\"" + hexed(color) + "\" stop-opacity=\"" + share(color.a) + "\"/>";

            out += linear ? "</linearGradient>\n" : "</radialGradient>\n";
        } else {
            const Pattern& pattern = std::get<Pattern>(paint);
            const media::Size size = pattern.image().size();

            // A direction that does not repeat holds one image in a tile far wider than any
            // drawing, so that the tiles next to it lie out of sight.
            const double wide = pattern.across() ? size.width : 1e7;
            const double high = pattern.down() ? size.height : 1e7;

            out += "<pattern id=\"" + id + "\" patternUnits=\"userSpaceOnUse\" x=\"0\" y=\"0\" width=\"" + number(wide) + "\" height=\"" + number(high) + "\" patternTransform=\"" + transformed(pattern.placed()) + "\">";

            if (pattern.image().held())
                out += "<image width=\"" + number(size.width) + "\" height=\"" + number(size.height) + "\" preserveAspectRatio=\"none\" xlink:href=\"" + png_url(pattern.image(), pattern.still()) + "\"/>";

            out += "</pattern>\n";
        }

        return " " + name + "=\"url(#" + id + ")\"";
    }
};

// A rectangle with its negative sizes turned round, as `Pen::draw` takes it.
Rect normalized(Rect one) {
    if (one.wide < 0) {
        one.at.x += one.wide;
        one.wide = -one.wide;
    }

    if (one.high < 0) {
        one.at.y += one.high;
        one.high = -one.high;
    }

    return one;
}

}

// An empty recording of a drawing of `size` pixels, its colors to be mixed in the space of
// `blending`. Throws Trouble other for a size of no pixels or over `media::ceiling`, as a raster.
Recording::Recording(media::Size size, Blending blending) : size_(size), blending_(blending), steps_(std::make_unique<Steps>()) {
    const std::uint64_t pixels = std::uint64_t{size.width} * size.height;

    if (pixels == 0 || pixels > media::ceiling) throw Trouble{Kind::other, "A recording of no pixels or over the ceiling of pixels."};
}

// Lets go of the steps.
Recording::~Recording() = default;

// The steps of `other` taken over; `other` is left with none.
Recording::Recording(Recording&& other) noexcept = default;

// The steps of `other` taken over in place of these.
Recording& Recording::operator=(Recording&& other) noexcept = default;

// How many steps are kept: drawings, clips, and the saving and restoring of the pen.
std::size_t Recording::steps() const { return steps_ ? steps_->all.size() : 0; }

// The steps drawn again with `onto`, after its matrix (a pen scaled by two draws them twice as
// large), each with the opacity and smoothing it was recorded with; on a raster of the recording's
// size and blending as it would have been drawn there. The pen's state is as before afterwards,
// also when a step throws, as a raster refuses a path the matrix carries beyond the numbers.
void Recording::played(Pen& onto) const {
    if (!steps_) return;

    const Matrix base = onto.placed();
    std::size_t depth = 0;
    const auto unwound = [&] {
        for (; depth > 0; --depth) onto.restore();

        onto.restore();
    };

    onto.save();

    try {
        for (const Step& one : steps_->all) {
            if (one.kind == Step::Kind::save) {
                onto.save();
                ++depth;

                continue;
            }

            if (one.kind == Step::Kind::restore) {
                onto.restore();
                --depth;

                continue;
            }

            onto.place(one.matrix.then(base));
            onto.opacity(one.opacity);
            onto.smoothing(one.smooth);

            switch (one.kind) {
            case Step::Kind::clip: onto.clip(one.path, one.rule); break;
            case Step::Kind::fill: onto.fill(one.path, one.paint, one.rule); break;
            case Step::Kind::stroke: onto.stroke(one.path, one.paint, one.style); break;
            case Step::Kind::draw: onto.draw(*one.image, one.from, one.into, one.still); break;
            case Step::Kind::save:
            case Step::Kind::restore: break;
            }
        }
    } catch (...) {
        unwound();

        throw;
    }

    unwound();
}

// The recording as an SVG 1.1 document of its size in pixels: each step as an element with the
// matrix it was drawn with as its transform; a clip as a group clipped by its path, closed when the
// pen that clipped is restored; fills and strokes as paths (even-odd as the fill rule; the width,
// joins, caps, miter limit (one at least, as SVG takes it; below one tal bevels every join, as SVG
// does at one) and dashes of a stroke, none where tal draws it whole), colors with their opacity,
// gradients and patterns as definitions (in linear light where the recording mixes there, which
// viewers may not all do), images as PNG inside, unsmoothed where the pen drew them sharp; the
// pen's opacity on each element. Raster and viewer differ in one thing: tal lays translucent colors
// over each other in linear light, a viewer in sRGB, so overlaps of them may come out a little
// apart. Throws Trouble malformed for a step whose numbers go beyond the finite ones, and Trouble
// full for a dash list past its limit.
std::string svg(const Recording& recording) {
    const media::Size size = recording.size();
    Writer writer;
    std::vector<std::size_t> groups{0};

    writer.out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" version=\"1.1\" width=\"" + number(size.width) + "\" height=\"" + number(size.height) +
                 "\" viewBox=\"0 0 " + number(size.width) + " " + number(size.height) + "\">\n";

    const auto closed = [&writer](std::size_t many) {
        for (std::size_t one = 0; one < many; ++one) writer.out += "</g>\n";
    };

    if (recording.steps_)
        for (const Recording::Step& one : recording.steps_->all) {
            const std::string opacity = one.opacity < 1 ? " opacity=\"" + share(one.opacity) + "\"" : std::string{};

            switch (one.kind) {
            case Recording::Step::Kind::save: groups.push_back(0); break;
            case Recording::Step::Kind::restore:
                closed(groups.back());
                groups.pop_back();
                break;
            case Recording::Step::Kind::clip: {
                const std::string id = writer.named("c");

                writer.out += "<clipPath id=\"" + id + "\" clipPathUnits=\"userSpaceOnUse\"><path d=\"" + Writer::data(one.path) + "\" transform=\"" + transformed(one.matrix) + "\"" + (one.rule == Rule::evenodd ? " clip-rule=\"evenodd\"" : "") +
                              "/></clipPath>\n<g clip-path=\"url(#" + id + ")\">\n";
                ++groups.back();
                break;
            }
            case Recording::Step::Kind::fill: {
                const std::string paint = writer.painted(one.paint, "fill", recording.blending());

                writer.out += "<path d=\"" + Writer::data(one.path) + "\" transform=\"" + transformed(one.matrix) + "\"" + paint + (one.rule == Rule::evenodd ? " fill-rule=\"evenodd\"" : "") + opacity + "/>\n";
                break;
            }
            case Recording::Step::Kind::stroke: {
                static constexpr std::string_view joins[] = {"miter", "round", "bevel"};
                static constexpr std::string_view caps[] = {"butt", "round", "square"};
                const std::string paint = writer.painted(one.paint, "stroke", recording.blending());
                std::string dashes;

                if (!stroking::pattern(one.style).empty())
                    for (const double length : one.style.dashes) dashes += (dashes.empty() ? "" : ",") + number(length);

                writer.out += "<path d=\"" + Writer::data(one.path) + "\" transform=\"" + transformed(one.matrix) + "\" fill=\"none\"" + paint + " stroke-width=\"" + number(one.style.width) + "\" stroke-linejoin=\"" +
                              std::string(joins[static_cast<std::size_t>(one.style.join)]) + "\" stroke-linecap=\"" + std::string(caps[static_cast<std::size_t>(one.style.cap)]) + "\" stroke-miterlimit=\"" + number(std::max(1.0, one.style.miter)) + "\"" +
                              (dashes.empty() ? std::string{} : " stroke-dasharray=\"" + dashes + "\" stroke-dashoffset=\"" + number(one.style.offset) + "\"") + opacity + "/>\n";
                break;
            }
            case Recording::Step::Kind::draw: {
                const Rect from = normalized(one.from);
                const Rect into = normalized(one.into);

                if (from.wide == 0 || from.high == 0 || into.wide == 0 || into.high == 0) break;

                writer.out += "<g transform=\"" + transformed(one.matrix) + "\"" + opacity + "><svg x=\"" + number(into.at.x) + "\" y=\"" + number(into.at.y) + "\" width=\"" + number(into.wide) + "\" height=\"" + number(into.high) + "\" viewBox=\"" +
                              number(from.at.x) + " " + number(from.at.y) + " " + number(from.wide) + " " + number(from.high) + "\" preserveAspectRatio=\"none\" overflow=\"hidden\"><image width=\"" + number(one.image->size().width) + "\" height=\"" +
                              number(one.image->size().height) + "\" preserveAspectRatio=\"none\"" + (one.smooth ? "" : " image-rendering=\"optimizeSpeed\"") + " xlink:href=\"" + png_url(*one.image, one.still) + "\"/></svg></g>\n";
                break;
            }
            }
        }

    for (const std::size_t many : groups) closed(many);

    writer.out += "</svg>\n";

    return std::move(writer.out);
}

}
