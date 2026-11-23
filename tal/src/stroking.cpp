#include "stroking.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include "tal/trouble.hpp"

// Stroking (E7a): a stroke as pieces, closed polygons filled together by the nonzero rule. The
// lines come flattened, in the coordinates of the path, and so the pieces are made there too and
// mapped afterwards, as in Canvas: a matrix that stretches one way more than the other flattens
// the pen with the path.
//
// Every segment is a band of the stroke's width; every vertex between two segments a wedge on the
// outer side of its turn, from the vertex out to the bands' corners (and to the miter's point, or
// round); the ends of an open run its caps. Each piece is turned to wind the same way, so where
// pieces overlap (at every join, where a line crosses itself, where a thick stroke folds over a
// tight turn) their windings add and never cancel, and nonzero fills their union; the rasterizer
// cuts each pixel's row into sixteen, so that the union is covered to within a few hundredths of
// a pixel. (One outline of the whole stroke, the way of Skia and cairo, is fewer edges, but its
// inner side folds back through the vertices and, where segments are short against the width,
// winds the other way and cuts holes.)
//
// Joins come where pieces of the path meet; points within a flattened curve are joined round,
// since the true outline of a curve is smooth. A miter is cut to a bevel where it would reach
// further than `Stroke::miter` half widths from the vertex. Dashes are laid along each subpath
// anew from `Stroke::offset`, an odd list counting twice, as in Canvas; a dash of no length still
// takes its caps (round dots of a dotted line), and on a closed subpath the dash running over its
// start is one dash. Repeated points are dropped; a subpath of one point draws nothing.
namespace tal::drawing::stroking {

namespace {

using flattening::Line;

// `one` scaled by `by`.
Point times(Point one, double by) { return Point{one.x * by, one.y * by}; }

// The sum of `one` and `other`.
Point plus(Point one, Point other) { return Point{one.x + other.x, one.y + other.y}; }

// `one` less `other`.
Point minus(Point one, Point other) { return Point{one.x - other.x, one.y - other.y}; }

// The normal of the direction `d`, turned a quarter to its left in the coordinates of the path.
Point normal(Point d) { return Point{-d.y, d.x}; }

// `d` made one long; `d` must not be of no length.
Point unit(Point d) { return times(d, 1 / std::hypot(d.x, d.y)); }

// A run of a stroke: the points of a subpath or of one dash, whether it closes, the corners among
// its points, the lengths of its lines where they stand for curves (to be dashed by), and its
// directions at the start and the end (for a dash of one point).
struct Run {
    std::vector<Point> points;
    std::vector<bool> corners;
    std::vector<double> lengths;
    bool closed = false;
    bool dot = false;
    Point head{1, 0};
    Point tail{1, 0};
};

// Whether `one` and `other` are one point, up to the last bits of their size.
bool same(Point one, Point other) {
    const double scale = 1 + std::max({std::abs(one.x), std::abs(one.y), std::abs(other.x), std::abs(other.y)});

    return std::hypot(one.x - other.x, one.y - other.y) <= 1e-12 * scale;
}

// `run` without repeated points (also the last of a closed run where it repeats the first). The
// length of a line dropped goes to the next line kept; the length of the line into a last point
// dropped, to the line closing the run.
void deduplicated(Run& run) {
    std::vector<Point> points;
    std::vector<bool> corners;
    std::vector<double> lengths;
    const bool measured = !run.lengths.empty();
    double extra = 0;

    for (std::size_t at = 0; at < run.points.size(); ++at) {
        const double length = measured && at > 0 && at - 1 < run.lengths.size() ? run.lengths[at - 1] : 0;

        if (!points.empty() && same(points.back(), run.points[at])) {
            corners.back() = corners.back() || run.corners[at];
            extra += length;

            continue;
        }

        if (measured && !points.empty()) lengths.push_back(length + extra);

        extra = 0;
        points.push_back(run.points[at]);
        corners.push_back(run.corners[at]);
    }

    if (run.closed && points.size() > 1 && same(points.back(), points.front())) {
        corners.front() = corners.front() || corners.back();
        points.pop_back();
        corners.pop_back();
    }

    run.points = std::move(points);
    run.corners = std::move(corners);
    run.lengths = std::move(lengths);
}

// The making of the pieces of one stroke: its style, the tolerance of its round parts, the view
// they are seen in (in pixels, through the matrix) and the polygons made.
class Outlining {
public:
    // The step of a round part's angle is the arc whose chord strays `tolerance` from it at the
    // middle, 4·asin(√(tolerance/2r)) for a radius r of half the width (the form by the cosine,
    // 2·acos(1 - tolerance/r), loses the step to rounding once r is some 10¹⁵ tolerances).
    Outlining(const Stroke& style, double tolerance, const std::optional<flattening::Bounds>& view, const Matrix& matrix) : style_(style), half_(style.width / 2), tolerance_(tolerance), view_(view), matrix_(matrix) {
        const double step = half_ > tolerance_ ? 4 * std::asin(std::sqrt(tolerance_ / (2 * half_))) : std::numbers::pi / 2;

        step_ = step > 0 ? step : std::numbers::pi / 2;
    }

    // The pieces of `run` added: a band for every segment, a join at every vertex between two (at
    // every vertex of a closed run), caps at the ends of an open one, or a dot for a dash of no
    // length.
    void outlined(Run run) {
        deduplicated(run);

        if (run.points.size() == 1) {
            if (run.dot && style_.cap != Cap::butt) dotted(run.points.front(), run.head);

            return;
        }

        if (run.points.empty()) return;

        const std::vector<Point>& points = run.points;
        const std::size_t count = points.size();
        const std::size_t segments = run.closed ? count : count - 1;

        for (std::size_t at = 0; at < segments; ++at) {
            const Point from = points[at];
            const Point to = points[(at + 1) % count];
            const Point side = times(normal(unit(minus(to, from))), half_);

            kept({plus(from, side), plus(to, side), minus(to, side), minus(from, side)});
        }

        for (std::size_t at = run.closed ? 0 : 1; at < (run.closed ? count : count - 1); ++at) {
            const Point before = unit(minus(points[at], points[(at + count - 1) % count]));
            const Point after = unit(minus(points[(at + 1) % count], points[at]));

            joined(points[at], before, after, run.corners[at]);
        }

        if (!run.closed) {
            capped(points[count - 1], unit(minus(points[count - 1], points[count - 2])));
            capped(points[0], unit(minus(points[0], points[1])));
        }
    }

    // The polygons made.
    std::vector<Line> made() && { return std::move(made_); }

private:
    // The join at `vertex` from the direction `before` to `after`, on the outer side of the turn:
    // by the style there (round within a curve), a wedge from the vertex; nothing where the path
    // goes straight on. The inner side is covered by the bands themselves.
    void joined(Point vertex, Point before, Point after, bool corner) {
        const double cross = before.x * after.y - before.y * after.x;
        const double dot = before.x * after.x + before.y * after.y;

        if (std::abs(cross) <= 1e-12 && dot > 0) return;

        const double outer = cross > 1e-12 ? -1 : 1;
        const Point from = times(normal(before), outer);
        const Point to = times(normal(after), outer);
        const Join join = corner ? style_.join : Join::round;
        std::vector<Point> wedge{vertex, plus(vertex, times(from, half_))};

        if (join == Join::round) {
            const double angle = std::abs(cross) <= 1e-12 ? -std::numbers::pi : std::atan2(cross, dot);

            arced(vertex, from, angle, wedge);
        } else if (join == Join::miter && 1 + dot > 1e-12 && std::sqrt(2 / (1 + dot)) <= style_.miter) {
            wedge.push_back(plus(vertex, times(plus(from, to), half_ / (1 + dot))));
        }

        wedge.push_back(plus(vertex, times(to, half_)));
        kept(std::move(wedge));
    }

    // The cap at `end` of a run that came along `along`: a half disc, or a half square, beyond it.
    void capped(Point end, Point along) {
        const Point side = normal(along);

        if (style_.cap == Cap::round) {
            std::vector<Point> disc{plus(end, times(side, half_))};

            arced(end, side, -std::numbers::pi, disc);
            disc.push_back(minus(end, times(side, half_)));
            kept(std::move(disc));
        } else if (style_.cap == Cap::square) {
            kept({plus(end, times(side, half_)), plus(plus(end, times(side, half_)), times(along, half_)), plus(minus(end, times(side, half_)), times(along, half_)), minus(end, times(side, half_))});
        }
    }

    // A dash of no length at `at` going along `along`: its two caps, a dot or a square.
    void dotted(Point at, Point along) {
        capped(at, along);
        capped(at, times(along, -1));
    }

    // The point of the arc of radius half the width around `center`, from the direction `from`
    // turned by `turn`.
    [[nodiscard]] Point turned(Point center, Point from, double turn) const {
        const double cosine = std::cos(turn);
        const double sine = std::sin(turn);

        return plus(center, times(Point{from.x * cosine - from.y * sine, from.x * sine + from.y * cosine}, half_));
    }

    // The points within the arc of radius half the width around `center`, from the direction
    // `from` turning by `angle` (no more than a half turn): at even steps without a view, else in
    // halves, a part that lies wholly beyond the view going as its chord, so that a stroke far
    // wider than the view costs the steps of the arc it shows; the coverage within the view stays
    // within the tolerance.
    void arced(Point center, Point from, double angle, std::vector<Point>& into) const {
        if (!view_) {
            evened(center, from, 0, angle, into);

            return;
        }

        halved(center, from, 0, angle, into);
    }

    // The points within the arc around `center` from the turn `first` to `last`, at even steps.
    void evened(Point center, Point from, double first, double last, std::vector<Point>& into) const {
        const auto steps = static_cast<std::size_t>(std::ceil(std::abs(last - first) / step_));

        for (std::size_t one = 1; one < steps; ++one) into.push_back(turned(center, from, first + (last - first) * static_cast<double>(one) / static_cast<double>(steps)));
    }

    // The points within the arc around `center` from the turn `first` to `last`: at even steps
    // where it takes `reach` steps or fewer, else in halves, the points of each half with the point
    // between them, and none for a half that lies beyond the view (the box of its ends and of where
    // the tangents at them meet, which holds the part and its chord, misses the view once the
    // matrix has carried them to pixels; for a half turn they meet beyond the numbers' reach, and
    // the part is halved again). Throws Trouble malformed where the matrix carries an end past the
    // range of numbers, as it would the stroke's points.
    void halved(Point center, Point from, double first, double last, std::vector<Point>& into) const {
        const double angle = std::abs(last - first);

        if (std::ceil(angle / step_) <= reach) {
            evened(center, from, first, last, into);

            return;
        }

        const Point start = matrix_.applied(turned(center, from, first));
        const Point end = matrix_.applied(turned(center, from, last));
        const Point apex = matrix_.applied(plus(center, times(minus(turned(center, from, (first + last) / 2), center), 1 / std::cos(angle / 2))));

        if (!std::isfinite(start.x) || !std::isfinite(start.y) || !std::isfinite(end.x) || !std::isfinite(end.y)) throw Trouble{Kind::malformed, "A round part of a stroke carried past the range of numbers."};

        const double x0 = std::min({start.x, end.x, apex.x});
        const double x1 = std::max({start.x, end.x, apex.x});
        const double y0 = std::min({start.y, end.y, apex.y});
        const double y1 = std::max({start.y, end.y, apex.y});

        if (x1 < view_->x0 || x0 > view_->x1 || y1 < view_->y0 || y0 > view_->y1) return;

        const double middle = (first + last) / 2;

        halved(center, from, first, middle, into);
        into.push_back(turned(center, from, middle));
        halved(center, from, middle, last, into);
    }

    // `polygon` kept as a closed line, turned so that it winds as every other piece does (the
    // nonzero rule then fills their union, overlaps never cancelling). Throws Trouble full past
    // `most_pieces` points in all, each polygon counting eight more for what it costs besides its
    // points.
    void kept(std::vector<Point> polygon) {
        double twice = 0;

        for (std::size_t at = 0; at < polygon.size(); ++at) {
            const Point& p = polygon[at];
            const Point& q = polygon[(at + 1) % polygon.size()];

            twice += p.x * q.y - q.x * p.y;
        }

        if (twice < 0) std::reverse(polygon.begin(), polygon.end());

        points_ += polygon.size() + 8;

        if (points_ > most_pieces) throw Trouble{Kind::full, "A stroke of more than " + std::to_string(most_pieces) + " points."};

        Line line;

        line.corners.assign(polygon.size(), true);
        line.points = std::move(polygon);
        line.closed = true;
        made_.push_back(std::move(line));
    }

    static constexpr double reach = 64;

    const Stroke& style_;
    double half_;
    double tolerance_;
    const std::optional<flattening::Bounds>& view_;
    const Matrix& matrix_;
    double step_ = 0;
    std::vector<Line> made_;
    std::size_t points_ = 0;
};

// The share of the segment from `from` to `to` within `view` (none: all of it): the parameters,
// 0 to 1, where it enters and leaves; an empty range (the first past the second) when it misses.
std::pair<double, double> within(Point from, Point to, const std::optional<flattening::Bounds>& view) {
    if (!view) return {0, 1};

    double enter = 0;
    double leave = 1;
    const Point d = minus(to, from);

    for (const auto& [low, high, start, step] : {std::tuple{view->x0, view->x1, from.x, d.x}, std::tuple{view->y0, view->y1, from.y, d.y}}) {
        if (step == 0) {
            if (start < low || start > high) return {1, 0};

            continue;
        }

        const double one = (low - start) / step;
        const double other = (high - start) / step;

        enter = std::max(enter, std::min(one, other));
        leave = std::min(leave, std::max(one, other));
    }

    return {enter, leave};
}

// The dashing of one line: the pattern, where in it the line is, and where its dashes go. Each
// dash is outlined as soon as it ends, but for the first one of a closed line, kept until the last
// one shows whether the two meet over the start. Outside the view (the part of the path that can
// reach a pixel, a stroke's reach included) whole rounds of the pattern are passed over at once,
// wherever the next change of the pattern lies outside the part of a segment within the view. Each
// round leaves the pattern as it was; a dash going on over them goes on, longer, where nothing is
// seen (a dash begun within the view and leaving it ends past the view, not at its edge). So a
// line far longer than the view costs the dashes within it.
class Dashing {
public:
    Dashing(const std::vector<double>& dashes, double offset, Outlining& into, const std::optional<flattening::Bounds>& view) : dashes_(dashes), into_(into), view_(view) {
        double sum = 0;

        for (const double one : dashes_) sum += one;

        sum_ = sum;

        double phase = std::fmod(offset, sum);

        if (phase < 0) phase += sum;

        while (phase >= dashes_[index_] && phase > 0) {
            phase -= dashes_[index_];
            index_ = (index_ + 1) % dashes_.size();
        }

        left_ = dashes_[index_] - phase;
        on_ = index_ % 2 == 0;
    }

    // The dashes along `line` (without repeated points), its corners kept, outlined; each line
    // counts by its length (that of the part of a curve it stands for) and its ends are laid on
    // it by the same share.
    void dashed(const Line& line) {
        const std::size_t count = line.points.size();
        const std::size_t edges = line.closed ? count : count - 1;
        bool whole = true;

        opened_ = on_;
        keeping_ = line.closed && on_;

        if (on_) started(line.points[0], line.corners[0], unit(minus(line.points[1 % count], line.points[0])));

        for (std::size_t edge = 0; edge < edges; ++edge) {
            const Point from = line.points[edge];
            const Point to = line.points[(edge + 1) % count];
            const double length = edge < line.lengths.size() ? line.lengths[edge] : std::hypot(to.x - from.x, to.y - from.y);
            const Point along = unit(minus(to, from));
            const auto [enter, leave] = within(from, to, view_);
            const double seen = enter <= leave ? enter * length : length;
            const double gone = enter <= leave ? leave * length : length;
            double done = 0;

            while (left_ <= length - done) {
                const double next = done + left_;
                const double rounds = next < seen ? std::floor((seen - next) / sum_) : next > gone ? std::floor((length - next) / sum_) : 0;

                if (rounds >= 1) {
                    done += rounds * sum_;
                    whole = false;

                    continue;
                }

                done += left_;

                const Point here = plus(from, times(minus(to, from), length > 0 ? done / length : 0));

                if (on_) {
                    current_.points.push_back(here);
                    current_.corners.push_back(false);
                    current_.tail = along;
                    finished();
                } else {
                    started(here, false, along);
                }

                whole = false;
                on_ = !on_;
                index_ = (index_ + 1) % dashes_.size();
                left_ = dashes_[index_];
            }

            left_ -= length - done;

            if (on_) {
                current_.points.push_back(to);
                current_.corners.push_back(line.corners[(edge + 1) % count]);
                current_.tail = along;
            }
        }

        if (whole && opened_) {
            into_.outlined(Run{.points = line.points, .corners = line.corners, .closed = line.closed});

            return;
        }

        if (on_ && !current_.points.empty()) {
            if (first_) {
                current_.points.insert(current_.points.end(), first_->points.begin() + 1, first_->points.end());
                current_.corners.insert(current_.corners.end(), first_->corners.begin() + 1, first_->corners.end());
                current_.tail = first_->tail;
                first_.reset();
            }

            into_.outlined(std::move(current_));
        }

        if (first_) into_.outlined(std::move(*first_));
    }

    // How many dashes `line` lays at most: as many as the length it counts, where a segment the
    // view leaves partly unseen counts the part seen and two rounds of the pattern besides (one
    // before it, one after it), and a few dashes more.
    [[nodiscard]] double counted(const Line& line) const {
        const std::size_t count = line.points.size();
        const std::size_t edges = line.closed ? count : count - 1;
        const double many = static_cast<double>(dashes_.size());
        double length = 0;

        for (std::size_t edge = 0; edge < edges; ++edge) {
            const Point from = line.points[edge];
            const Point to = line.points[(edge + 1) % count];
            const double one = edge < line.lengths.size() ? line.lengths[edge] : std::hypot(to.x - from.x, to.y - from.y);
            const auto [enter, leave] = within(from, to, view_);
            const double seen = enter <= leave ? (leave - enter) * one : 0;

            length += std::min(one, seen + 2 * sum_ + 4 * sum_ / many);
        }

        return (length / sum_ + 1) * many;
    }

private:
    // A new dash at `at`, a corner or not, going along `along`.
    void started(Point at, bool corner, Point along) {
        current_ = Run{.points = {at}, .corners = {corner}, .dot = true, .head = along, .tail = along};
    }

    // The dash so far outlined, or kept when it is the first of a closed line. Throws Trouble full
    // when the dashes of the line come to more than `most_dashes` (which the estimate before
    // dashing should already have caught).
    void finished() {
        if (++made_ > most_dashes) throw Trouble{Kind::full, "A stroke of more than " + std::to_string(most_dashes) + " dashes."};

        if (keeping_ && !first_ && opened_ && made_ == 1) {
            first_ = std::move(current_);
        } else {
            into_.outlined(std::move(current_));
        }

        current_ = Run{};
    }

    const std::vector<double>& dashes_;
    Outlining& into_;
    const std::optional<flattening::Bounds>& view_;
    double sum_ = 0;
    std::size_t index_ = 0;
    double left_ = 0;
    bool on_ = true;
    bool opened_ = false;
    bool keeping_ = false;
    Run current_;
    std::optional<Run> first_;
    std::size_t made_ = 0;
};

}

// The dashes of the style, doubled when odd; none when the list is empty, holds a negative length
// or only lengths of zero. Throws Trouble full past `most_dashes`.
std::vector<double> pattern(const Stroke& style) {
    if (style.dashes.empty()) return {};

    if (style.dashes.size() > most_dashes) throw Trouble{Kind::full, "A dash list of more than " + std::to_string(most_dashes) + " lengths."};

    double sum = 0;

    for (const double one : style.dashes) {
        if (one < 0) return {};

        sum += one;
    }

    if (!(sum > 0) || !std::isfinite(sum)) return {};

    std::vector<double> made = style.dashes;

    if (made.size() % 2 == 1) made.insert(made.end(), style.dashes.begin(), style.dashes.end());

    return made;
}

// The pieces of `lines` (flattened, in the path's coordinates) stroked with `style`, its round
// parts within `tolerance`, the dashes laid only where `view` (in the same coordinates, the
// stroke's reach included; none: everywhere) may see them, the round parts of caps and joins cut to
// what `seen` (the view itself in pixels, without the reach, where `matrix` carries the path; none:
// all of them) shows: closed polygons that all wind one way, for the nonzero rule. Throws Trouble
// full, before any dash is made, when the dashes would come to more than `most_dashes`: dashes of
// no length with butt caps leave nothing, so the points alone would not stop them. A width of zero
// draws nothing.
std::vector<Line> outlined(const std::vector<Line>& lines, const Stroke& style, double tolerance, const std::optional<flattening::Bounds>& view, const std::optional<flattening::Bounds>& seen,
                           const Matrix& matrix) {
    if (!(style.width > 0)) return {};

    const std::vector<double> dashes = pattern(style);
    double expected = 0;
    Outlining outlining{style, tolerance, seen, matrix};

    for (const Line& line : lines) {
        Run run{.points = line.points, .corners = line.corners, .lengths = line.lengths, .closed = line.closed};

        deduplicated(run);

        if (run.points.size() < 2) continue;

        if (dashes.empty()) {
            outlining.outlined(std::move(run));

            continue;
        }

        Dashing dashing{dashes, style.offset, outlining, view};
        const Line kept{.points = std::move(run.points), .corners = std::move(run.corners), .lengths = std::move(run.lengths), .closed = run.closed};

        expected += dashing.counted(kept);

        if (!(expected <= static_cast<double>(most_dashes))) throw Trouble{Kind::full, "A stroke of more than " + std::to_string(most_dashes) + " dashes."};

        dashing.dashed(kept);
    }

    return std::move(outlining).made();
}

}
