#include "tal/drawing.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "tal/trouble.hpp"

#include "flattening.hpp"
#include "lighting.hpp"
#include "rasterizing.hpp"
#include "recording.hpp"
#include "stroking.hpp"

// The raster and the pen (E7a). A raster keeps its pixels premultiplied by their alpha, 16 bits a
// channel, in the space it blends in: linear light by default, or sRGB as Canvas and the browsers
// do. Eight bits of linear light would band the darks, and sixteen keep every sRGB byte apart; the
// pixels come out as straight sRGB bytes, as everywhere in tal. Colors are given in sRGB, between 0
// and 1 (beyond is clamped), and a pen turns them into the raster's space; so do gradients, whose
// stops blend premultiplied (no dark fringe between a color and transparency), and images.
//
// A pen draws on one raster through its state: a matrix, a clip, an opacity and whether images are
// smoothed (bilinear) or sharp (nearest); save and restore keep and bring back the whole state. A
// fill flattens its path in the raster's pixels within a tenth of a pixel, covers it by the rule
// and lays the paint over (source over), each pixel by its coverage, the clip and the opacity. A
// stroke is outlined in the path's coordinates within the same tenth of a pixel once mapped, and
// filled by nonzero. Paints live in the path's coordinates too: a gradient or a pattern moves and
// turns with the matrix. A clip is a mask of coverage, each new clip multiplied into the last.
//
// Every number that is not finite, in a matrix, a color, a style or a point, is Trouble malformed,
// and so is a path that the matrix carries beyond the range of numbers; nothing of that operation
// is drawn. Restoring with nothing saved is Trouble other: it is a mistake of the program.
namespace tal::drawing {

namespace {

constexpr double tolerance = 0.1;

using rasterizing::Box;

// A color premultiplied by its alpha in the space of a raster.
struct Sample {
    float r = 0;
    float g = 0;
    float b = 0;
    float a = 0;
};

// The coverage of a clip over its box, 0 to 255 a pixel; nothing outside the box.
struct Mask {
    Box box;
    std::vector<std::uint8_t> cover;

    // The coverage of the pixel (`x`, `y`) of the raster, 0 to 1.
    [[nodiscard]] float at(int x, int y) const {
        if (x < box.x0 || x >= box.x1 || y < box.y0 || y >= box.y1) return 0;

        return static_cast<float>(cover[static_cast<std::size_t>(y - box.y0) * static_cast<std::size_t>(box.wide()) + static_cast<std::size_t>(x - box.x0)]) / 255.0F;
    }
};

// Throws Trouble malformed when any of `numbers` is not finite.
void finite(std::initializer_list<double> numbers) {
    for (const double one : numbers)
        if (!std::isfinite(one)) throw Trouble{Kind::malformed, "A point, size or matrix that is not a finite number."};
}

// Throws Trouble malformed when any part of `color` is not finite.
void finite(const Color& color) {
    if (!std::isfinite(color.r) || !std::isfinite(color.g) || !std::isfinite(color.b) || !std::isfinite(color.a)) throw Trouble{Kind::malformed, "A color that is not a finite number."};
}

// Throws Trouble malformed when `style` holds a number that is not finite, a negative width or a
// miter limit that is not positive.
void checked(const Stroke& style) {
    finite({style.width, style.miter, style.offset});

    for (const double one : style.dashes) finite({one});

    if (style.width < 0) throw Trouble{Kind::malformed, "A stroke of a negative width."};

    if (!(style.miter > 0)) throw Trouble{Kind::malformed, "A miter limit that is not positive."};
}

// Throws Trouble malformed when a point of `lines` lies beyond the range of numbers.
void finite(const std::vector<flattening::Line>& lines) {
    for (const flattening::Line& line : lines)
        for (const Point& one : line.points)
            if (!std::isfinite(one.x) || !std::isfinite(one.y)) throw Trouble{Kind::malformed, "A path the matrix carries beyond the range of numbers."};
}

// The sRGB value `one` (0 to 1) in linear light.
float decoded(float one) { return one <= 0.04045F ? one / 12.92F : static_cast<float>(std::pow((static_cast<double>(one) + 0.055) / 1.055, 2.4)); }

// `color` (sRGB, clamped to 0 to 1) premultiplied in the space of `blending`.
Sample sampled(const Color& color, Blending blending) {
    const float a = std::clamp(color.a, 0.0F, 1.0F);
    float r = std::clamp(color.r, 0.0F, 1.0F);
    float g = std::clamp(color.g, 0.0F, 1.0F);
    float b = std::clamp(color.b, 0.0F, 1.0F);

    if (blending == Blending::linear) {
        r = decoded(r);
        g = decoded(g);
        b = decoded(b);
    }

    return Sample{r * a, g * a, b * a, a};
}

// The texels of the still `still` of `image` within [x0, x1) × [y0, y1), premultiplied in the
// space of `blending`, row by row.
std::vector<Sample> textured(const media::Pixels& image, std::size_t still, Blending blending, int x0, int y0, int x1, int y1) {
    const std::array<double, 256>& light = media::lighting::straight();
    const Bytes& rgba = image.stills()[still].rgba;
    const std::size_t wide = image.size().width;
    std::vector<Sample> made;

    made.reserve(static_cast<std::size_t>(x1 - x0) * static_cast<std::size_t>(y1 - y0));

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * wide + static_cast<std::size_t>(x)) * 4;
            const float a = static_cast<float>(std::to_integer<int>(rgba[at + 3])) / 255.0F;
            float channel[3];

            for (int one = 0; one < 3; ++one) {
                const int byte = std::to_integer<int>(rgba[at + static_cast<std::size_t>(one)]);

                channel[one] = blending == Blending::linear ? static_cast<float>(light[static_cast<std::size_t>(byte)]) : static_cast<float>(byte) / 255.0F;
            }

            made.push_back(Sample{channel[0] * a, channel[1] * a, channel[2] * a, a});
        }
    }

    return made;
}

// A paint of one color.
struct Plain {
    Sample color;

    // The color at any point.
    [[nodiscard]] Sample operator()(double, double) const { return color; }
};

// A gradient seen from the raster: its geometry in the path's coordinates, the matrix from the
// raster's pixels there, and its stops in the raster's space.
struct Graded {
    Gradient::Shape shape;
    Point first;
    Point second;
    double inner;
    double outer;
    Matrix back;
    std::vector<std::pair<double, Sample>> stops;

    // The color at the point (`x`, `y`) of the raster.
    [[nodiscard]] Sample operator()(double x, double y) const {
        const Point at = back.applied(Point{x, y});
        double place = 0;

        if (stops.empty()) return Sample{};

        if (shape == Gradient::Shape::linear) {
            const double dx = second.x - first.x;
            const double dy = second.y - first.y;
            const double length = dx * dx + dy * dy;

            if (length == 0) return Sample{};

            place = ((at.x - first.x) * dx + (at.y - first.y) * dy) / length;
        } else {
            const std::optional<double> found = circled(at);

            if (!found) return Sample{};

            place = *found;
        }

        return colored(place);
    }

    // The largest ω of the circles through `at` whose radius is not negative, if any (the two
    // circles of Canvas: center first + ω (second - first), radius inner + ω (outer - inner)).
    [[nodiscard]] std::optional<double> circled(Point at) const {
        const double cx = second.x - first.x;
        const double cy = second.y - first.y;
        const double dr = outer - inner;
        const double px = at.x - first.x;
        const double py = at.y - first.y;

        if (cx == 0 && cy == 0 && dr == 0) return std::nullopt;

        const double a = cx * cx + cy * cy - dr * dr;
        const double b = px * cx + py * cy + inner * dr;
        const double c = px * px + py * py - inner * inner;
        const auto fits = [&](double omega) { return inner + omega * dr >= 0; };

        if (std::abs(a) <= 1e-12 * (cx * cx + cy * cy + dr * dr)) {
            if (b == 0) return std::nullopt;

            const double omega = c / (2 * b);

            return fits(omega) ? std::optional<double>{omega} : std::nullopt;
        }

        const double discriminant = b * b - a * c;

        if (discriminant < 0) return std::nullopt;

        const double root = std::sqrt(discriminant);
        const double one = (b + root) / a;
        const double other = (b - root) / a;
        const double high = std::max(one, other);
        const double low = std::min(one, other);

        if (fits(high)) return high;

        if (fits(low)) return low;

        return std::nullopt;
    }

    // The color at `place` along the stops, the end colors beyond them; nothing where the place is
    // not a number (a matrix or gradient so extreme that the arithmetic gave out).
    [[nodiscard]] Sample colored(double place) const {
        if (std::isnan(place)) return Sample{};

        if (place <= stops.front().first) return stops.front().second;

        if (place >= stops.back().first) return stops.back().second;

        const auto after = std::upper_bound(stops.begin(), stops.end(), place, [](double where, const std::pair<double, Sample>& one) { return where < one.first; });
        const auto before = after - 1;
        const double span = after->first - before->first;
        const auto t = static_cast<float>(span > 0 ? (place - before->first) / span : 1.0);
        const Sample& p = before->second;
        const Sample& q = after->second;

        return Sample{p.r + (q.r - p.r) * t, p.g + (q.g - p.g) * t, p.b + (q.b - p.b) * t, p.a + (q.a - p.a) * t};
    }
};

// How a texture is read beyond its texels along one axis.
enum class Edge : std::uint8_t { repeat, none, clamp };

// Texels seen from the raster: the texels and their size, the matrix from the raster's pixels to
// them, how each axis ends, and whether they are smoothed.
struct Textured {
    std::vector<Sample> texels;
    int wide = 0;
    int high = 0;
    Matrix back;
    Edge across = Edge::none;
    Edge down = Edge::none;
    bool smooth = true;

    // The color at the point (`x`, `y`) of the raster; nothing where it maps to texels further than
    // the numbers of texels go (or to no number at all).
    [[nodiscard]] Sample operator()(double x, double y) const {
        const Point at = back.applied(Point{x, y});

        if (!(std::abs(at.x) <= 1e15) || !(std::abs(at.y) <= 1e15)) return Sample{};

        if (!smooth) return texel(static_cast<long long>(std::floor(at.x)), static_cast<long long>(std::floor(at.y)));

        const double u = at.x - 0.5;
        const double v = at.y - 0.5;
        const double u0 = std::floor(u);
        const double v0 = std::floor(v);
        const auto i = static_cast<long long>(u0);
        const auto j = static_cast<long long>(v0);
        const auto fu = static_cast<float>(u - u0);
        const auto fv = static_cast<float>(v - v0);
        const Sample s00 = texel(i, j);
        const Sample s10 = texel(i + 1, j);
        const Sample s01 = texel(i, j + 1);
        const Sample s11 = texel(i + 1, j + 1);
        const auto mixed = [&](float p00, float p10, float p01, float p11) { return (p00 * (1 - fu) + p10 * fu) * (1 - fv) + (p01 * (1 - fu) + p11 * fu) * fv; };

        return Sample{mixed(s00.r, s10.r, s01.r, s11.r), mixed(s00.g, s10.g, s01.g, s11.g), mixed(s00.b, s10.b, s01.b, s11.b), mixed(s00.a, s10.a, s01.a, s11.a)};
    }

    // The texel (`i`, `j`), by the edges beyond the texture.
    [[nodiscard]] Sample texel(long long i, long long j) const {
        const std::optional<long long> x = placed(i, wide, across);
        const std::optional<long long> y = placed(j, high, down);

        if (!x || !y) return Sample{};

        return texels[static_cast<std::size_t>(*y) * static_cast<std::size_t>(wide) + static_cast<std::size_t>(*x)];
    }

    // The index `at` on an axis of `count` texels ending by `edge`, none where it reads nothing.
    [[nodiscard]] static std::optional<long long> placed(long long at, int count, Edge edge) {
        if (at >= 0 && at < count) return at;

        if (edge == Edge::none) return std::nullopt;

        if (edge == Edge::clamp) return std::clamp<long long>(at, 0, count - 1);

        const long long folded = at % count;

        return folded < 0 ? folded + count : folded;
    }
};

// Whether `at` lies on the segment from `from` to `to`, up to the last bits of their size.
bool touches(Point at, Point from, Point to) {
    const double scale = 1 + std::max({std::abs(at.x), std::abs(at.y), std::abs(from.x), std::abs(from.y), std::abs(to.x), std::abs(to.y)});
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    const double length = dx * dx + dy * dy;
    double t = length > 0 ? ((at.x - from.x) * dx + (at.y - from.y) * dy) / length : 0;

    t = std::clamp(t, 0.0, 1.0);

    return std::hypot(at.x - (from.x + t * dx), at.y - (from.y + t * dy)) <= 1e-9 * scale;
}

// Whether `at` is inside the polygons of `lines` by `rule`; a point on an edge is inside.
bool within(const std::vector<flattening::Line>& lines, Point at, Rule rule) {
    long long winding = 0;

    for (const flattening::Line& line : lines) {
        const std::size_t count = line.points.size();

        if (count < 2) continue;

        for (std::size_t one = 0; one < count; ++one) {
            const Point from = line.points[one];
            const Point to = line.points[(one + 1) % count];

            if (touches(at, from, to)) return true;

            const double side = (to.x - from.x) * (at.y - from.y) - (at.x - from.x) * (to.y - from.y);

            if (from.y <= at.y && to.y > at.y && side > 0) ++winding;

            if (to.y <= at.y && from.y > at.y && side < 0) --winding;
        }
    }

    return rule == Rule::nonzero ? winding != 0 : winding % 2 != 0;
}

}

// The whole state of a pen.
struct Pen::State {
    Matrix matrix;
    std::shared_ptr<const Mask> clip;
    float opacity = 1;
    bool smooth = true;
};

// The working memory of a pen, kept from one operation to the next.
struct Pen::Work {
    rasterizing::Coverage coverage;
};

// The polygons of one operation, in the raster's pixels.
struct Pen::Lines {
    std::vector<flattening::Line> all;
};

// A raster of `size`, all transparent, blending in the space `blending`. Throws Trouble other for a
// size of no pixels or over `media::ceiling`.
Raster::Raster(media::Size size, Blending blending) : size_(size), blending_(blending) {
    const std::uint64_t pixels = std::uint64_t{size.width} * size.height;

    if (pixels == 0 || pixels > media::ceiling) throw Trouble{Kind::other, "A raster of no pixels or over the ceiling of pixels."};

    samples_.assign(static_cast<std::size_t>(pixels) * 4, 0);
}

// Every pixel set to `with`, laid over nothing. Throws Trouble malformed for a color that is not
// finite.
void Raster::clear(Color with) {
    finite(with);

    const Sample one = sampled(with, blending_);
    const auto quantized = [](float value) { return static_cast<std::uint16_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 65535.0F)); };
    const std::uint16_t four[4] = {quantized(one.r), quantized(one.g), quantized(one.b), quantized(one.a)};

    for (std::size_t at = 0; at < samples_.size(); ++at) samples_[at] = four[at % 4];
}

// The pixels as straight sRGB bytes, one still: each sample divided by its alpha in integers (to
// the nearest of 65 535 steps; an opaque pixel's samples as they are, which is what the division
// gives) and, in linear light, encoded by a table of those steps.
media::Pixels Raster::pixels() const {
    static const std::vector<unsigned char> bent = [] {
        std::vector<unsigned char> made(65536);

        for (std::size_t at = 0; at < made.size(); ++at) made[at] = media::lighting::bent(static_cast<double>(at) / 65535.0);

        return made;
    }();
    const std::size_t count = samples_.size() / 4;
    Bytes rgba(samples_.size());

    for (std::size_t at = 0; at < count; ++at) {
        const std::uint32_t alpha = samples_[at * 4 + 3];

        if (alpha == 0) continue;

        for (std::size_t one = 0; one < 3; ++one) {
            const std::uint32_t sample = std::min<std::uint32_t>(samples_[at * 4 + one], alpha);
            const auto straight = alpha == 65535 ? std::size_t{sample} : static_cast<std::size_t>((static_cast<std::uint64_t>(sample) * 65535 + alpha / 2) / alpha);
            const unsigned char byte = blending_ == Blending::linear ? bent[straight] : static_cast<unsigned char>((straight * 255 + 32767) / 65535);

            rgba[at * 4 + one] = std::byte{byte};
        }

        rgba[at * 4 + 3] = std::byte{static_cast<unsigned char>((alpha * 255 + 32767) / 65535)};
    }

    std::vector<media::Still> stills;

    stills.push_back(media::Still{std::move(rgba), std::chrono::milliseconds{0}});

    return media::Pixels{size_, std::move(stills)};
}

// A pen on `onto`: no matrix, no clip, fully opaque, images smoothed.
Pen::Pen(Raster& onto) : onto_(&onto), state_(std::make_unique<State>()), work_(std::make_unique<Work>()) {}

// A pen recording into `onto` what it is asked to draw, each step with the matrix, the opacity and
// the smoothing it is drawn with, its clips and the saving and restoring around them, for
// `Recording::played` and `svg`; otherwise as a pen on a raster of the recording's size, with the
// same checks (what a raster would refuse only at its pixels, as a matrix carrying a path beyond
// the numbers, is refused when the recording is played or written).
Pen::Pen(Recording& onto) : recording_(&onto), state_(std::make_unique<State>()), work_(std::make_unique<Work>()) {}

// Lets go of the state kept.
Pen::~Pen() = default;

// The state kept, for `restore`.
void Pen::save() {
    saved_.push_back(*state_);

    if (recording_ != nullptr) recording_->steps_->all.push_back(Recording::Step{.kind = Recording::Step::Kind::save});
}

// The state last kept brought back. Throws Trouble other when nothing is kept.
void Pen::restore() {
    if (saved_.empty()) throw Trouble{Kind::other, "A pen restored with nothing saved."};

    *state_ = std::move(saved_.back());
    saved_.pop_back();

    if (recording_ != nullptr) recording_->steps_->all.push_back(Recording::Step{.kind = Recording::Step::Kind::restore});
}

// `more` applied before the matrix so far, as Canvas's transform: what is drawn next is mapped by
// `more` first.
void Pen::transform(const Matrix& more) {
    finite({more.a, more.b, more.c, more.d, more.e, more.f});
    state_->matrix = more.then(state_->matrix);
}

// `step` kept in the recording, drawn with the matrix, the opacity and the smoothing of now.
void Pen::recorded(Recording::Step step) {
    step.matrix = state_->matrix;
    step.opacity = state_->opacity;
    step.smooth = state_->smooth;
    recording_->steps_->all.push_back(std::move(step));
}

// The matrix replaced by `whole`.
void Pen::place(const Matrix& whole) {
    finite({whole.a, whole.b, whole.c, whole.d, whole.e, whole.f});
    state_->matrix = whole;
}

// The matrix from the path's coordinates to the raster's pixels.
const Matrix& Pen::placed() const { return state_->matrix; }

// The opacity of everything drawn next, clamped to 0 to 1.
void Pen::opacity(float alpha) {
    finite({alpha});
    state_->opacity = std::clamp(alpha, 0.0F, 1.0F);
}

// Whether images and patterns are smoothed (bilinear) or sharp (the nearest texel).
void Pen::smoothing(bool on) { state_->smooth = on; }

namespace {

// The pixels of `onto` the state's clip leaves open.
Box opened(const Raster& onto, const Mask* clip) {
    const Box whole{0, 0, static_cast<int>(onto.size().width), static_cast<int>(onto.size().height)};

    return clip == nullptr ? whole : clip->box.common(whole);
}

// The view of the flattening for `box`.
flattening::Bounds viewed(const Box& box) { return flattening::Bounds{static_cast<double>(box.x0), static_cast<double>(box.y0), static_cast<double>(box.x1), static_cast<double>(box.y1)}; }

// The farthest a stroke in `style` reaches from its line: half its width, times the miter limit
// for miter joins and the root of two for square caps.
double reach(const Stroke& style) { return style.width / 2 * std::max({1.0, style.join == Join::miter ? style.miter : 1.0, style.cap == Cap::square ? std::numbers::sqrt2 : 1.0}); }

// The view, in the space of the path, of a stroke in `style` drawn through `matrix` onto the
// pixels of `view`: the bounds of all that the matrix carries into it, widened by the `reach` of
// the stroke. A dashed stroke has it too: the curves beyond it count their true length, and the
// dashes are laid within it only. None for a matrix with no inverse or a view the inverse carries
// beyond the range of numbers.
std::optional<flattening::Bounds> reached(const Matrix& matrix, const flattening::Bounds& view, const Stroke& style) {
    const std::optional<Matrix> back = matrix.inverted();

    if (!back) return std::nullopt;

    const double far = reach(style);
    flattening::Bounds made{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};

    for (const Point corner : {Point{view.x0, view.y0}, Point{view.x1, view.y0}, Point{view.x0, view.y1}, Point{view.x1, view.y1}}) {
        const Point one = back->applied(corner);

        made = flattening::Bounds{std::min(made.x0, one.x), std::min(made.y0, one.y), std::max(made.x1, one.x), std::max(made.y1, one.y)};
    }

    made = flattening::Bounds{made.x0 - far, made.y0 - far, made.x1 + far, made.y1 + far};

    if (!std::isfinite(made.x0) || !std::isfinite(made.y0) || !std::isfinite(made.x1) || !std::isfinite(made.y1)) return std::nullopt;

    return made;
}

// The angle within which the lines of a stroke in `style` keep to the direction of its curves at
// their ends, for flattening within `near`: as far as moves the farthest point of a cap or a join
// (its `reach`) by no more than `near`, between a thousandth and a tenth of a radian; for miter
// joins no more than a ten-thousandth, since whether a miter stands or is cut to a bevel turns on
// the angle (a miter near the limit, ten times as long as it is wide, changes its length by a
// tenth for every hundredth of a radian), and the lines' angle should decide it as the curve's
// would.
double turning(const Stroke& style, double near) {
    const double made = std::clamp(near / reach(style), 1e-3, 0.1);

    return style.join == Join::miter ? std::min(made, 1e-4) : made;
}

// The dashes of a stroke in `style` for flattening within `near`: its pattern, its offset, the
// reach of its caps (half the width, or the root of two of it for square caps), and the angle
// within which the lines keep to the direction of a curve near an end of a dash, as far as moves
// the farthest point of a cap by no more than `near`, between five thousandths and a tenth of a
// radian (so that a stroke far wider than the raster still costs a few points a turn). No pattern
// for a stroke without dashes.
flattening::Dashes dashes(const Stroke& style, double near) {
    std::vector<double> lengths = stroking::pattern(style);

    if (lengths.empty()) return {};

    const double far = style.width / 2 * (style.cap == Cap::square ? std::numbers::sqrt2 : 1.0);

    return flattening::Dashes{std::move(lengths), style.offset, std::clamp(near / far, 5e-3, 0.1), far};
}

// The view of a question about the point `one`: the square of a pixel around it each way.
flattening::Bounds around(Point one) { return flattening::Bounds{one.x - 1, one.y - 1, one.x + 1, one.y + 1}; }

// `paint` laid over `samples` (a raster of `wide` pixels a row) by the coverage, the clip and the
// opacity; where an opaque color covers a pixel whole, the pixel is the color (what blending over
// it gives), a whole run of them at once for a plain color without a clip.
template <typename Painting>
void laid(std::vector<std::uint16_t>& samples, std::size_t wide, const rasterizing::Coverage& coverage, const Mask* clip, float opacity, const Painting& paint) {
    const Box& box = coverage.box();

    for (int y = box.y0; y < box.y1; ++y) {
        const std::span<const rasterizing::Step> steps = coverage.steps(y);

        for (std::size_t step = 0; step < steps.size(); ++step) {
            const float base = steps[step].cover * opacity;

            if (base <= 0) continue;

            const int end = step + 1 < steps.size() ? steps[step + 1].x : box.x1;
            std::uint16_t* pixel = samples.data() + (static_cast<std::size_t>(y) * wide + static_cast<std::size_t>(steps[step].x)) * 4;

            if constexpr (std::is_same_v<Painting, Plain>) {
                if (clip == nullptr && base >= 1 && paint.color.a >= 1) {
                    const auto whole = [](float source) { return static_cast<std::uint16_t>(std::clamp(source, 0.0F, 1.0F) * 65535.0F + 0.5F); };
                    const std::array<std::uint16_t, 4> four{whole(paint.color.r), whole(paint.color.g), whole(paint.color.b), 65535};

                    for (int x = steps[step].x; x < end; ++x, pixel += 4) std::copy(four.begin(), four.end(), pixel);

                    continue;
                }
            }

            for (int x = steps[step].x; x < end; ++x, pixel += 4) {
                const float cover = clip != nullptr ? base * clip->at(x, y) : base;

                if (cover <= 0) continue;

                const Sample color = paint(x + 0.5, y + 0.5);

                if (cover >= 1 && color.a >= 1) {
                    const auto whole = [](float source) { return static_cast<std::uint16_t>(std::clamp(source, 0.0F, 1.0F) * 65535.0F + 0.5F); };

                    pixel[0] = whole(color.r);
                    pixel[1] = whole(color.g);
                    pixel[2] = whole(color.b);
                    pixel[3] = 65535;

                    continue;
                }

                const float a = color.a * cover;
                const float rest = (1 - a) / 65535.0F;
                const auto blended = [rest](float source, std::uint16_t under) { return static_cast<std::uint16_t>(std::clamp(source + static_cast<float>(under) * rest, 0.0F, 1.0F) * 65535.0F + 0.5F); };

                pixel[0] = blended(color.r * cover, pixel[0]);
                pixel[1] = blended(color.g * cover, pixel[1]);
                pixel[2] = blended(color.b * cover, pixel[2]);
                pixel[3] = blended(a, pixel[3]);
            }
        }
    }
}

}

// `path` filled with `paint` by `rule`.
void Pen::fill(const Path& path, const Paint& paint, Rule rule) {
    if (const Color* color = std::get_if<Color>(&paint)) finite(*color);

    if (recording_ != nullptr) {
        recorded(Recording::Step{.kind = Recording::Step::Kind::fill, .path = path, .rule = rule, .paint = paint});

        return;
    }

    const Box open = opened(*onto_, state_->clip.get());
    const Lines lines{flattening::flattened(path, state_->matrix, tolerance, viewed(open))};

    finite(lines.all);
    painted(lines, rule, paint);
}

// `path` stroked with `paint` in `style`. Throws Trouble malformed for a style of numbers that are
// not finite, a negative width or a miter limit that is not positive.
void Pen::stroke(const Path& path, const Paint& paint, const Stroke& style) {
    checked(style);

    if (const Color* color = std::get_if<Color>(&paint)) finite(*color);

    const double stretch = flattening::stretch(state_->matrix);

    if (style.width == 0 || !(stretch > 0)) return;

    if (recording_ != nullptr) {
        recorded(Recording::Step{.kind = Recording::Step::Kind::stroke, .path = path, .paint = paint, .style = style});

        return;
    }

    const double near = tolerance / stretch;
    const Box open = opened(*onto_, state_->clip.get());
    const std::optional<flattening::Bounds> view = reached(state_->matrix, viewed(open), style);
    Lines outline{stroking::outlined(flattening::flattened(path, Matrix{}, near, view, turning(style, near), dashes(style, near)), style, near, view, viewed(open), state_->matrix)};

    for (flattening::Line& line : outline.all)
        for (Point& one : line.points) one = state_->matrix.applied(one);

    finite(outline.all);
    painted(outline, Rule::nonzero, paint);
}

// The clip narrowed to `path` by `rule`: from now on (until a restore) only its coverage is drawn.
void Pen::clip(const Path& path, Rule rule) {
    if (recording_ != nullptr) {
        recorded(Recording::Step{.kind = Recording::Step::Kind::clip, .path = path, .rule = rule});

        return;
    }

    const Box open = opened(*onto_, state_->clip.get());
    const std::vector<flattening::Line> lines = flattening::flattened(path, state_->matrix, tolerance, viewed(open));

    finite(lines);

    rasterizing::Coverage& coverage = work_->coverage;
    const Box box = rasterizing::bounded(lines, open);
    auto made = std::make_shared<Mask>();

    made->box = box;
    made->cover.assign(static_cast<std::size_t>(std::max(0, box.wide())) * static_cast<std::size_t>(std::max(0, box.high())), 0);

    if (!box.empty()) {
        coverage.reset(box);
        coverage.covered(lines, rule);

        for (int y = box.y0; y < box.y1; ++y) {
            const std::span<const rasterizing::Step> steps = coverage.steps(y);

            for (std::size_t step = 0; step < steps.size(); ++step) {
                const int end = step + 1 < steps.size() ? steps[step + 1].x : box.x1;

                for (int x = steps[step].x; x < end; ++x) {
                    const float before = state_->clip ? state_->clip->at(x, y) : 1.0F;
                    const float cover = steps[step].cover * before;

                    made->cover[static_cast<std::size_t>(y - box.y0) * static_cast<std::size_t>(box.wide()) + static_cast<std::size_t>(x - box.x0)] = static_cast<std::uint8_t>(std::clamp(cover, 0.0F, 1.0F) * 255.0F + 0.5F);
                }
            }
        }
    }

    state_->clip = std::move(made);
}

// The whole still `still` of `image` drawn into `into`.
void Pen::draw(const media::Pixels& image, Rect into, std::size_t still) {
    if (!image.held()) return;

    draw(image, Rect{Point{0, 0}, static_cast<double>(image.size().width), static_cast<double>(image.size().height)}, into, still);
}

// The part `from` (in the image's pixels) of the still `still` of `image` drawn into `into`, as
// Canvas's drawImage: negative sizes are turned round, a part reaching beyond the image is cut to
// it (and `into` with it), and the image is read only within that part. Throws Trouble other for a
// still the image does not have.
void Pen::draw(const media::Pixels& image, Rect from, Rect into, std::size_t still) {
    finite({from.at.x, from.at.y, from.wide, from.high, into.at.x, into.at.y, into.wide, into.high});

    if (!image.held()) return;

    if (still >= image.stills().size()) throw Trouble{Kind::other, "A still the image does not have."};

    if (recording_ != nullptr) {
        recorded(Recording::Step{.kind = Recording::Step::Kind::draw, .image = std::make_shared<const media::Pixels>(image), .from = from, .into = into, .still = still});

        return;
    }

    const auto normalized = [](Rect& one) {
        if (one.wide < 0) {
            one.at.x += one.wide;
            one.wide = -one.wide;
        }

        if (one.high < 0) {
            one.at.y += one.high;
            one.high = -one.high;
        }
    };

    normalized(from);
    normalized(into);

    if (from.wide == 0 || from.high == 0 || into.wide == 0 || into.high == 0) return;

    const double sx = into.wide / from.wide;
    const double sy = into.high / from.high;
    const double width = image.size().width;
    const double height = image.size().height;
    const double left = std::max(from.at.x, 0.0);
    const double top = std::max(from.at.y, 0.0);
    const double right = std::min(from.at.x + from.wide, width);
    const double bottom = std::min(from.at.y + from.high, height);

    if (right <= left || bottom <= top) return;

    const Rect cut{Point{into.at.x + (left - from.at.x) * sx, into.at.y + (top - from.at.y) * sy}, (right - left) * sx, (bottom - top) * sy};
    const int x0 = static_cast<int>(std::floor(left));
    const int y0 = static_cast<int>(std::floor(top));
    const int x1 = static_cast<int>(std::ceil(right));
    const int y1 = static_cast<int>(std::ceil(bottom));
    const std::optional<Matrix> back = state_->matrix.inverted();

    if (!back) return;

    const Matrix toward{1 / sx, 0, 0, 1 / sy, left - x0 - cut.at.x / sx, top - y0 - cut.at.y / sy};
    Path box;

    box.rectangle(cut);

    const Box open = opened(*onto_, state_->clip.get());
    const std::vector<flattening::Line> lines = flattening::flattened(box, state_->matrix, tolerance, viewed(open));

    finite(lines);

    rasterizing::Coverage& coverage = work_->coverage;
    const Box reach = rasterizing::bounded(lines, open);

    if (reach.empty()) return;

    coverage.reset(reach);
    coverage.covered(lines, Rule::nonzero);

    const Textured texture{textured(image, still, onto_->blending_, x0, y0, x1, y1), x1 - x0, y1 - y0, back->then(toward), Edge::clamp, Edge::clamp, state_->smooth};

    laid(onto_->samples_, onto_->size_.width, coverage, state_->clip.get(), state_->opacity, texture);
}

// Whether the point `one` of the raster lies inside `path` (as the matrix maps it) by `rule`; a
// point on its edge is inside.
bool Pen::inside(const Path& path, Point one, Rule rule) const {
    finite({one.x, one.y});

    const std::vector<flattening::Line> lines = flattening::flattened(path, state_->matrix, tolerance, around(one));

    finite(lines);

    return within(lines, one, rule);
}

// Whether the point `one` of the raster lies on the stroke of `path` in `style`.
bool Pen::on(const Path& path, Point one, const Stroke& style) const {
    finite({one.x, one.y});
    checked(style);

    const double stretch = flattening::stretch(state_->matrix);

    if (style.width == 0 || !(stretch > 0)) return false;

    const double near = tolerance / stretch;
    const std::optional<flattening::Bounds> view = reached(state_->matrix, around(one), style);
    std::vector<flattening::Line> outline = stroking::outlined(flattening::flattened(path, Matrix{}, near, view, turning(style, near), dashes(style, near)), style, near, view, around(one), state_->matrix);

    for (flattening::Line& line : outline)
        for (Point& point : line.points) point = state_->matrix.applied(point);

    finite(outline);

    return within(outline, one, Rule::nonzero);
}

// `lines` (in the raster's pixels) covered by `rule` and painted with `paint`.
void Pen::painted(const Lines& lines, Rule rule, const Paint& paint) {
    const Box open = opened(*onto_, state_->clip.get());
    const Box reach = rasterizing::bounded(lines.all, open);

    if (reach.empty()) return;

    const std::optional<Matrix> back = state_->matrix.inverted();

    if (!back) return;

    rasterizing::Coverage& coverage = work_->coverage;

    coverage.reset(reach);
    coverage.covered(lines.all, rule);

    const Mask* clip = state_->clip.get();
    const float opacity = state_->opacity;
    const Blending blending = onto_->blending_;

    if (const Color* color = std::get_if<Color>(&paint)) {
        laid(onto_->samples_, onto_->size_.width, coverage, clip, opacity, Plain{sampled(*color, blending)});
    } else if (const Gradient* gradient = std::get_if<Gradient>(&paint)) {
        Graded graded{gradient->shape(), gradient->first(), gradient->second(), gradient->inner(), gradient->outer(), *back, {}};

        for (const auto& [at, stop] : gradient->stops()) graded.stops.emplace_back(at, sampled(stop, blending));

        laid(onto_->samples_, onto_->size_.width, coverage, clip, opacity, graded);
    } else {
        const Pattern& pattern = std::get<Pattern>(paint);
        const media::Pixels& image = pattern.image();

        if (!image.held()) return;

        const std::optional<Matrix> unplaced = pattern.placed().inverted();

        if (!unplaced) return;

        const int wide = static_cast<int>(image.size().width);
        const int high = static_cast<int>(image.size().height);
        const Textured texture{textured(image, pattern.still(), blending, 0, 0, wide, high), wide, high, back->then(*unplaced), pattern.across() ? Edge::repeat : Edge::none, pattern.down() ? Edge::repeat : Edge::none, state_->smooth};

        laid(onto_->samples_, onto_->size_.width, coverage, clip, opacity, texture);
    }
}

}
