#include "flattening.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string>
#include <vector>

#include "tal/trouble.hpp"

// Flattening (E7a): a path as broken lines in the space of a matrix, no point further than the
// tolerance from the true curve. Curves are mapped first and then cut, so that the tolerance holds
// where the lines are drawn, however the matrix stretches them. A curve of degree n is cut into
// equal steps of its parameter, as many as Wang's bound asks: sqrt(n (n - 1) M / (8 t)) for the
// largest second difference M of its control points and the tolerance t. An arc of an ellipse is
// cut by equal steps of its angle, each the angle whose chord stays within the tolerance of the
// largest circle the mapped ellipse fits in.
//
// Every point says whether it is a corner of the path (where its pieces meet) or lies within a
// curve, since a stroke joins the two differently.
//
// A stroke takes the direction of its caps and joins from the lines at them, so for a stroke the
// lines may be asked to keep within an angle (`turning`) of the curve's own direction at both ends
// of every curve: the step next to the end is halved (up to `finest` times) until its line is
// within the angle of the curve's direction there, so that a miter or a cap at the end of a curve
// stands as it would on the curve itself. A dashed stroke (`Dashes`) ends a dash wherever its
// pattern says, so the flattening follows the pattern along each subpath (from the offset anew, as
// the dashing does, by the same lengths): a line of a curve that comes within the stroke's reach
// of an end of a dash (a band of the stroke turned away from the curve reaches past the end of the
// line by up to that much) is cut in halves (up to `finest` times) until each part turns no more
// than the dashes' angle, so that the cap stands as on the curve and nothing pokes past it. Every
// line of a curve keeps the length of its part of the curve (by Gauss and Legendre), and the
// dashing counts by those, so that dashes lie where the curve's own length puts them.
//
// A curve all of whose control points lie beyond one edge of the view (above, below, left or right)
// becomes the straight line between its ends: beyond an edge it can change no pixel but through the
// winding it carries across rows, and a line between the same ends carries the same (the two make a
// loop within the hull of the points, which winds around no pixel of the view). For a dashed stroke
// the line counts by the length of the curve it stands for (by Gauss and Legendre over halves until
// they agree), so that the dashes within the view lie where they would without it; the dashing lays
// none along such a line. A curve that would need more than `leaf` lines and crosses an edge is cut
// in halves, by de Casteljau, each half taken the same way; an arc likewise, by halves of its
// angle, the hull of a part of at most a quarter turn being the triangle of its ends and the
// meeting of its tangents. So a curve off the screen costs one line, not millions, and one that
// only passes through the view costs what its part within it does; past `most_pieces` points in all
// a path is Trouble full. A halving halves the lines a part needs, or, where rounding (a part's
// size times the precision of a double) is all that bends it, takes a part half as large; `deepest`
// halvings bring any finite curve, even one between the largest numbers, to `leaf` lines a part. So
// a curve that passes the view on its way to 1e69 is cut down to its part within the view, not into
// a million lines most of which lie outside. A part of an arc whose angle halving no longer
// changes, where the angles have no finer steps (an arc of radius 1e150, whose angles near the view
// are 1e-16 apart and whose points 1e134), is the line between its ends: nothing finer can be
// computed, and the halves would only repeat it. The halves wait on a list, not on the stack of
// calls.
namespace tal::drawing::flattening {

namespace {

// The flattening of one path: its lines, the points so far and the limit.
class Flattening {
public:
    Flattening(const Matrix& matrix, double tolerance, std::optional<Bounds> view, double turning, const Dashes& dashes) : matrix_(matrix), tolerance_(tolerance), view_(view), turning_(turning), dashes_(dashes) {
        for (const double one : dashes_.lengths) sum_ += one;
    }

    // The line of a new subpath at `at`; the pattern of dashes from its start.
    void moved(Point at) {
        lines_.push_back(Line{});
        point(matrix_.applied(at), true);
        restarted();
    }

    // A straight line to `to`.
    void lined(Point to) {
        const Point from = lines_.back().points.back();
        const Point mapped = matrix_.applied(to);

        advanced(std::hypot(mapped.x - from.x, mapped.y - from.y));
        point(mapped, true);
    }

    // A curve of the control points `given` (mapped), the first of which is the point so far.
    void curved(const Point* given, std::size_t count) {
        Point mapped[4];

        for (std::size_t at = 0; at < count; ++at) mapped[at] = matrix_.applied(given[at]);

        finite(mapped, count);
        cut(mapped, count, true, true);
    }

    // An arc of the ellipse of `piece`, from the point so far. The angle of a line within the
    // tolerance is 2 acos(1 - tolerance / radius), written as 4 asin(sqrt(tolerance / (2 radius))),
    // which stays above zero for radii past 1e15, where 1 - tolerance / radius is 1.
    void arced(const Path::Piece& piece) {
        const double radius = stretch(matrix_) * std::max(piece.rx, piece.ry);
        const Point center = matrix_.applied(piece.points[0]);

        finite(&center, 1);

        if (!std::isfinite(radius)) throw Trouble{Kind::malformed, beyond_range};

        if (view_ && (center.x + radius < view_->x0 || center.x - radius > view_->x1 || center.y + radius < view_->y0 || center.y - radius > view_->y1)) {
            chorded(matrix_.applied(piece.points[1]), true, arc_length(piece, 0, piece.sweep));

            return;
        }

        const double step = radius > tolerance_ ? 4 * std::asin(std::sqrt(tolerance_ / (2 * radius))) : std::numbers::pi / 2;
        const double quarters = std::max(1.0, std::ceil(std::abs(piece.sweep) / (std::numbers::pi / 2)));
        const std::size_t parts = view_ ? static_cast<std::size_t>(quarters) : 1;

        for (std::size_t part = 0; part < parts; ++part) {
            const double from = piece.sweep * static_cast<double>(part) / static_cast<double>(parts);
            const double to = piece.sweep * static_cast<double>(part + 1) / static_cast<double>(parts);

            bent(piece, step, from, to, part == 0, part + 1 == parts);
        }
    }

    // The subpath so far closed.
    void closed() {
        if (!lines_.empty()) lines_.back().closed = true;
    }

    // The lines made.
    std::vector<Line> made() && { return std::move(lines_); }

private:
    // The curve of the control points `given` (the first being the point so far), cut into lines;
    // its start the start of the whole curve when `first`, its end the end of the whole curve (a
    // corner) when `last`. The halves wait on a list of their own, the first half on top, rather
    // than on the stack of calls: a curve between the largest numbers takes a thousand halvings.
    void cut(const Point* given, std::size_t count, bool first, bool last) {
        struct Part {
            Point points[4];
            int depth = 0;
            bool first = false;
            bool last = false;
        };

        std::vector<Part> waiting(1);

        std::copy(given, given + count, waiting.front().points);
        waiting.front().first = first;
        waiting.front().last = last;

        while (!waiting.empty()) {
            const Part part = waiting.back();
            const Point* mapped = part.points;

            waiting.pop_back();

            if (beyond(mapped, count)) {
                chorded(mapped[count - 1], part.last, curve_length(mapped, count));

                continue;
            }

            double most = 0;

            for (std::size_t at = 0; at + 2 < count; ++at) most = std::max(most, std::hypot(mapped[at].x - 2 * mapped[at + 1].x + mapped[at + 2].x, mapped[at].y - 2 * mapped[at + 1].y + mapped[at + 2].y));

            const double degree = static_cast<double>(count - 1);
            const double steps = std::ceil(std::sqrt(degree * (degree - 1) * most / (8 * tolerance_)));

            if (view_ && steps > leaf && part.depth < deepest && !within(mapped, count)) {
                Part head{.depth = part.depth + 1, .first = part.first, .last = false};
                Part tail{.depth = part.depth + 1, .first = false, .last = part.last};

                halved(mapped, count, head.points, tail.points);

                waiting.push_back(tail);
                waiting.push_back(head);

                continue;
            }

            const auto at = [&](double t) { return count == 3 ? quadratic(mapped, t) : cubical(mapped, t); };
            const auto slope = [&](double t) { return count == 3 ? times(quadratic_slope(mapped, t), 2) : times(cubical_slope(mapped, t), 3); };

            stepped(at, slope, counted(steps), part.first ? heading(mapped, count) : Point{}, part.last ? heading(mapped, count, true) : Point{}, mapped[count - 1], part.last);
        }
    }

    // The part of the arc of `piece` between the angles `from` and `to` (from its start, at most a
    // quarter turn apart), cut into lines of at most `step` of angle; its start the start of the
    // arc when `first`, its end the end of the arc (a corner) when `last`. The halves wait on a
    // list, as those of a curve do.
    void bent(const Path::Piece& piece, double step, double from, double to, bool first, bool last) {
        struct Part {
            double from = 0;
            double to = 0;
            int depth = 0;
            bool first = false;
            bool last = false;
        };

        std::vector<Part> waiting{Part{.from = from, .to = to, .first = first, .last = last}};

        while (!waiting.empty()) {
            const Part part = waiting.back();

            waiting.pop_back();

            const Point end = part.last ? matrix_.applied(piece.points[1]) : spot(piece, piece.from + part.to);
            const double steps = std::ceil(std::abs(part.to - part.from) / step);

            if (view_) {
                const double half = (part.to - part.from) / 2;
                const Point middle = ellipse_point(piece.points[0], piece.rx / std::cos(half), piece.ry / std::cos(half), piece.turn, piece.from + part.from + half);
                const Point hull[3] = {spot(piece, piece.from + part.from), matrix_.applied(middle), end};

                if (beyond(hull, 3)) {
                    chorded(end, part.last, arc_length(piece, part.from, part.to));

                    continue;
                }

                if (steps > leaf && part.depth < deepest && !within(hull, 3)) {
                    const double start = piece.from + part.from;
                    const double split = start + half;

                    if (split == start || split == piece.from + part.to) {
                        chorded(end, part.last, arc_length(piece, part.from, part.to));

                        continue;
                    }

                    waiting.push_back(Part{.from = part.from + half, .to = part.to, .depth = part.depth + 1, .first = false, .last = part.last});
                    waiting.push_back(Part{.from = part.from, .to = part.from + half, .depth = part.depth + 1, .first = part.first, .last = false});

                    continue;
                }
            }

            const auto at = [&](double t) { return spot(piece, piece.from + part.from + (part.to - part.from) * t); };
            const auto slope = [&](double t) { return times(bearing(piece, piece.from + part.from + (part.to - part.from) * t), std::abs(part.to - part.from)); };

            stepped(at, slope, counted(steps), part.first ? bearing(piece, piece.from) : Point{}, part.last ? bearing(piece, piece.from + piece.sweep) : Point{}, end, part.last);
        }
    }

    // The points of a part of a curve whose point at the parameter t (0 to 1) is `at(t)` and whose
    // direction there is `slope(t)`, at the parameters 1 / `all` to 1, the last being `end` (a
    // corner when `last`). Where the curve's direction at its start (`start`) or its end
    // (`finish`) is given, more points go toward that end, at halves of the step next to it, until
    // the line next to it is within `turning_` of that direction. Each line follows the dashes.
    template <typename At, typename Slope>
    void stepped(const At& at, const Slope& slope, std::size_t all, Point start, Point finish, Point end, bool last) {
        const double step = 1 / static_cast<double>(all);
        double nearer[finest];
        std::size_t made = 0;
        double from = 0;
        Point here = lines_.back().points.back();
        const auto next = [&](double t, Point to, bool corner) {
            dashed(at, slope, from, here, t, to, corner, 0);
            from = t;
            here = to;
        };

        if (turning_ > 0 && !(start.x == 0 && start.y == 0)) {
            const Point origin = at(0.0);

            for (double h = step; made < finest && apart(minus(at(h), origin), start) > turning_; ++made) {
                h /= 2;
                nearer[made] = h;
            }

            while (made > 0) {
                const double t = nearer[--made];

                next(t, at(t), false);
            }
        }

        for (std::size_t one = 1; one < all; ++one) next(static_cast<double>(one) * step, at(static_cast<double>(one) * step), false);

        if (turning_ > 0 && !(finish.x == 0 && finish.y == 0)) {
            for (double h = step; made < finest && apart(minus(end, at(1 - h)), finish) > turning_; ++made) {
                h /= 2;
                next(1 - h, at(1 - h), false);
            }
        }

        next(1, end, last);
    }

    // The line of a curve from the parameter `t0` (at `p0`, the point so far) to `t1` (at `p1`, a
    // corner when `corner`), along the dashes: where it comes within the reach of an end of a dash
    // and the curve turns more than the dashes' angle between its two ends, cut at halves (to the
    // `depth` of `finest`); each line then keeps the length of its part of the curve, and the
    // pattern goes on by it.
    template <typename At, typename Slope>
    void dashed(const At& at, const Slope& slope, double t0, Point p0, double t1, Point p1, bool corner, std::size_t depth) {
        if (!(sum_ > 0)) {
            point(p1, corner);

            return;
        }

        const double length = measured(slope, t0, t1);

        const bool near = left_ <= length + dashes_.reach || passed_ < dashes_.reach;

        if (near && ends_ < most_dashes && depth < finest && apart(slope(t0), slope(t1)) > dashes_.turning) {
            const double t = (t0 + t1) / 2;
            const Point middle = at(t);

            dashed(at, slope, t0, p0, t, middle, false, depth + 1);
            dashed(at, slope, t, middle, t1, p1, corner, depth + 1);

            return;
        }

        advanced(length);
        point(p1, corner, length);
    }

    // The length of the curve whose derivative is `slope` between the parameters `t0` and `t1`, by
    // Gauss and Legendre at five points (as near as the lines' tolerance asks, the curve being
    // smooth between two points of its flattening).
    template <typename Slope>
    static double measured(const Slope& slope, double t0, double t1) {
        static constexpr double places[5] = {0, -0.5384693101056831, 0.5384693101056831, -0.9061798459386640, 0.9061798459386640};
        static constexpr double weights[5] = {0.5688888888888889, 0.4786286704993665, 0.4786286704993665, 0.2369268850561891, 0.2369268850561891};
        const double middle = (t0 + t1) / 2;
        const double half = (t1 - t0) / 2;
        double sum = 0;

        for (std::size_t at = 0; at < 5; ++at) {
            const Point d = slope(middle + half * places[at]);

            sum += weights[at] * std::hypot(d.x, d.y);
        }

        return sum * std::abs(half);
    }

    // The straight line to `to` in place of a curve beyond the view, a corner when `corner`. It
    // counts, for the dashes, by `length`, that of the part of the curve it stands for: a dash past
    // the view lies where the curve's own length puts it, as within.
    void chorded(Point to, bool corner, double length) {
        if (!(sum_ > 0)) {
            point(to, corner);

            return;
        }

        advanced(length);
        point(to, corner, length);
    }

    // The length of the curve of the `count` control points `p`, for the dashes only (none
    // without): by Gauss and Legendre over halves until they agree (`lengthy`).
    [[nodiscard]] double curve_length(const Point* p, std::size_t count) const {
        if (!(sum_ > 0)) return 0;

        return lengthy([&](double t) { return count == 3 ? times(quadratic_slope(p, t), 2) : times(cubical_slope(p, t), 3); });
    }

    // The length of the arc of `piece` between the angles `from` and `to` (from its start), for the
    // dashes only (none without), as `curve_length`.
    [[nodiscard]] double arc_length(const Path::Piece& piece, double from, double to) const {
        if (!(sum_ > 0)) return 0;

        return lengthy([&](double t) { return times(bearing(piece, piece.from + from + (to - from) * t), std::abs(to - from)); });
    }

    // The length of the curve whose derivative is `slope` from 0 to 1: Gauss and Legendre at five
    // points (`measured`) on a part, taken when its two halves come to the same within a
    // trillionth, or else the halves taken in turn; at most `finest` halvings deep and 4096 parts
    // in all, so that a part far from the view costs little whatever its shape.
    template <typename Slope>
    static double lengthy(const Slope& slope) {
        struct Part {
            double t0 = 0;
            double t1 = 1;
            double whole = 0;
            std::size_t depth = 0;
        };

        std::vector<Part> waiting{Part{.whole = measured(slope, 0, 1)}};
        double made = 0;
        std::size_t parts = 0;

        while (!waiting.empty()) {
            const Part part = waiting.back();

            waiting.pop_back();

            const double middle = (part.t0 + part.t1) / 2;
            const double head = measured(slope, part.t0, middle);
            const double tail = measured(slope, middle, part.t1);

            if (std::abs(head + tail - part.whole) <= 1e-12 * (head + tail) || part.depth >= finest || ++parts >= 4096) {
                made += head + tail;

                continue;
            }

            waiting.push_back(Part{.t0 = middle, .t1 = part.t1, .whole = tail, .depth = part.depth + 1});
            waiting.push_back(Part{.t0 = part.t0, .t1 = middle, .whole = head, .depth = part.depth + 1});
        }

        return made;
    }

    // The pattern of dashes from the start of a subpath: the offset into it.
    void restarted() {
        if (!(sum_ > 0)) return;

        double phase = std::fmod(dashes_.offset, sum_);

        if (phase < 0) phase += sum_;

        index_ = 0;

        while (phase >= dashes_.lengths[index_] && phase > 0) {
            phase -= dashes_.lengths[index_];
            index_ = (index_ + 1) % dashes_.lengths.size();
        }

        left_ = dashes_.lengths[index_] - phase;
        passed_ = -1;
    }

    // The pattern of dashes advanced by `length`: whole rounds of the pattern at once (by the
    // remainder, which is exact, since a length far past the pattern's would lose the rounds
    // otherwise and never come down), then dash by dash, each end counted, and the length since the
    // last end kept.
    void advanced(double length) {
        if (!(sum_ > 0) || !std::isfinite(length)) return;

        if (length < left_) {
            left_ -= length;
            passed_ = passed_ < 0 ? -1 : passed_ + length;

            return;
        }

        if (length >= left_ + sum_) {
            const double rounds = std::floor((length - left_) / sum_);

            length = left_ + std::fmod(length - left_, sum_);
            ends_ += static_cast<std::size_t>(std::min(rounds * static_cast<double>(dashes_.lengths.size()), static_cast<double>(most_dashes)));
        }

        while (left_ <= length) {
            length -= left_;
            index_ = (index_ + 1) % dashes_.lengths.size();
            left_ = dashes_.lengths[index_];
            ++ends_;
        }

        left_ -= length;
        passed_ = length;
    }

    // The direction of the quadratic curve of `p` at `t` (its derivative, halved).
    static Point quadratic_slope(const Point* p, double t) {
        const double u = 1 - t;

        return Point{u * (p[1].x - p[0].x) + t * (p[2].x - p[1].x), u * (p[1].y - p[0].y) + t * (p[2].y - p[1].y)};
    }

    // The direction of the cubic curve of `p` at `t` (its derivative, a third of it).
    static Point cubical_slope(const Point* p, double t) {
        const double u = 1 - t;

        return Point{u * u * (p[1].x - p[0].x) + 2 * u * t * (p[2].x - p[1].x) + t * t * (p[3].x - p[2].x), u * u * (p[1].y - p[0].y) + 2 * u * t * (p[2].y - p[1].y) + t * t * (p[3].y - p[2].y)};
    }

    // The direction of the curve of the control points `p` at its start, or at its end when `end`:
    // toward the first control point apart from that end (none when all are one point).
    static Point heading(const Point* p, std::size_t count, bool end = false) {
        for (std::size_t at = 1; at < count; ++at) {
            const Point d = end ? minus(p[count - 1], p[count - 1 - at]) : minus(p[at], p[0]);

            if (d.x != 0 || d.y != 0) return d;
        }

        return Point{};
    }

    // The direction of the arc of `piece`, mapped, at `angle`: its derivative, turned the way it
    // sweeps.
    [[nodiscard]] Point bearing(const Path::Piece& piece, double angle) const {
        const double sign = piece.sweep < 0 ? -1 : 1;
        const double x = -piece.rx * std::sin(angle) * sign;
        const double y = piece.ry * std::cos(angle) * sign;
        const double cosine = std::cos(piece.turn);
        const double sine = std::sin(piece.turn);
        const Point d{x * cosine - y * sine, x * sine + y * cosine};

        return Point{matrix_.a * d.x + matrix_.c * d.y, matrix_.b * d.x + matrix_.d * d.y};
    }

    // The angle between the directions `one` and `other`; none when either is of no length.
    static double apart(Point one, Point other) {
        if ((one.x == 0 && one.y == 0) || (other.x == 0 && other.y == 0)) return 0;

        return std::atan2(std::abs(one.x * other.y - one.y * other.x), one.x * other.x + one.y * other.y);
    }

    // `one` less `other`.
    static Point minus(Point one, Point other) { return Point{one.x - other.x, one.y - other.y}; }

    // The point at `angle` on the ellipse of `piece`, mapped.
    [[nodiscard]] Point spot(const Path::Piece& piece, double angle) const { return matrix_.applied(ellipse_point(piece.points[0], piece.rx, piece.ry, piece.turn, angle)); }

    // The curve of the control points `p` cut at its middle, by de Casteljau, into `first` and
    // `second`; the halves of two points are added, not their sum halved, which would overflow
    // between the largest numbers.
    static void halved(const Point* p, std::size_t count, Point* first, Point* second) {
        Point rows[4];

        for (std::size_t at = 0; at < count; ++at) rows[at] = p[at];

        for (std::size_t level = 0; level < count; ++level) {
            first[level] = rows[0];
            second[count - 1 - level] = rows[count - 1 - level];

            for (std::size_t at = 0; at + 1 < count - level; ++at) rows[at] = Point{rows[at].x / 2 + rows[at + 1].x / 2, rows[at].y / 2 + rows[at + 1].y / 2};
        }
    }

    // Throws Trouble malformed when one of the `count` points of `points` lies beyond the range of
    // numbers: the matrix carried the path there, and halving such a part would never end in
    // anything drawable.
    static void finite(const Point* points, std::size_t count) {
        for (std::size_t at = 0; at < count; ++at)
            if (!std::isfinite(points[at].x) || !std::isfinite(points[at].y)) throw Trouble{Kind::malformed, beyond_range};
    }

    // Whether all of `points` lie within the view: a part whose hull does is drawn whole, and
    // halving it would only cost the same lines in more steps (and, for one too long to draw, all
    // of them before the limit stops it, where its count alone would).
    [[nodiscard]] bool within(const Point* points, std::size_t count) const {
        for (std::size_t at = 0; at < count; ++at)
            if (!(points[at].x >= view_->x0 && points[at].x <= view_->x1 && points[at].y >= view_->y0 && points[at].y <= view_->y1)) return false;

        return true;
    }

    // Whether all of `points` lie beyond one edge of the view.
    [[nodiscard]] bool beyond(const Point* points, std::size_t count) const {
        if (!view_) return false;

        bool left = true;
        bool right = true;
        bool above = true;
        bool below = true;

        for (std::size_t at = 0; at < count; ++at) {
            left = left && points[at].x < view_->x0;
            right = right && points[at].x > view_->x1;
            above = above && points[at].y < view_->y0;
            below = below && points[at].y > view_->y1;
        }

        return left || right || above || below;
    }

    // The count of `steps` (at least one), within what the limit still allows.
    [[nodiscard]] std::size_t counted(double steps) const {
        const double room = static_cast<double>(most_pieces - std::min(most_pieces, points_));

        if (!(steps <= room)) throw Trouble{Kind::full, "A path of more than " + std::to_string(most_pieces) + " points once flattened."};

        return std::max<std::size_t>(1, static_cast<std::size_t>(steps));
    }

    // `at` added to the line so far, a corner of the path or a point within a curve; for a dashed
    // stroke the line to it keeps its `length` (that of its part of a curve; its own when none is
    // given). Throws Trouble full past the limit.
    void point(Point at, bool corner, double length = -1) {
        if (++points_ > most_pieces) throw Trouble{Kind::full, "A path of more than " + std::to_string(most_pieces) + " points once flattened."};

        Line& line = lines_.back();

        if (sum_ > 0 && !line.points.empty()) line.lengths.push_back(length >= 0 ? length : std::hypot(at.x - line.points.back().x, at.y - line.points.back().y));

        line.points.push_back(at);
        line.corners.push_back(corner);
    }

    // `one` scaled by `by`.
    static Point times(Point one, double by) { return Point{one.x * by, one.y * by}; }

    // The quadratic curve of `p` at `t`.
    static Point quadratic(const Point* p, double t) {
        const double u = 1 - t;

        return Point{u * u * p[0].x + 2 * u * t * p[1].x + t * t * p[2].x, u * u * p[0].y + 2 * u * t * p[1].y + t * t * p[2].y};
    }

    // The cubic curve of `p` at `t`.
    static Point cubical(const Point* p, double t) {
        const double u = 1 - t;

        return Point{u * u * u * p[0].x + 3 * u * u * t * p[1].x + 3 * u * t * t * p[2].x + t * t * t * p[3].x, u * u * u * p[0].y + 3 * u * u * t * p[1].y + 3 * u * t * t * p[2].y + t * t * t * p[3].y};
    }

    static constexpr double leaf = 64;
    static constexpr int deepest = 1100;
    static constexpr const char* beyond_range = "A path the matrix carries beyond the range of numbers.";
    static constexpr std::size_t finest = 40;

    const Matrix& matrix_;
    double tolerance_;
    std::optional<Bounds> view_;
    double turning_;
    const Dashes& dashes_;
    double sum_ = 0;
    std::size_t index_ = 0;
    double left_ = 0;
    double passed_ = -1;
    std::size_t ends_ = 0;
    std::vector<Line> lines_;
    std::size_t points_ = 0;
};

}

// The point at `angle` on the ellipse around `center` of radii `rx` and `ry`, turned by `turn`.
Point ellipse_point(Point center, double rx, double ry, double turn, double angle) {
    const double x = rx * std::cos(angle);
    const double y = ry * std::sin(angle);
    const double cosine = std::cos(turn);
    const double sine = std::sin(turn);

    return Point{center.x + x * cosine - y * sine, center.y + x * sine + y * cosine};
}

// The largest stretch of `matrix`: its largest singular value, how much longer it makes the
// longest of all lines.
double stretch(const Matrix& matrix) {
    const double sum = matrix.a * matrix.a + matrix.b * matrix.b + matrix.c * matrix.c + matrix.d * matrix.d;
    const double determinant = matrix.a * matrix.d - matrix.b * matrix.c;
    const double inside = std::max(0.0, sum * sum - 4 * determinant * determinant);

    return std::sqrt((sum + std::sqrt(inside)) / 2);
}

// `path` mapped by `matrix` as lines within `tolerance` of it, curves beyond an edge of `view` as
// straight lines, the lines at the ends of curves within the angle `turning` of their direction
// (not asked when of no size) and the lines on which `dashes` end within their angle; a subpath of
// a single point stays a line of one point, for whoever needs it.
std::vector<Line> flattened(const Path& path, const Matrix& matrix, double tolerance, std::optional<Bounds> view, double turning, const Dashes& dashes) {
    Flattening flattening{matrix, tolerance, view, turning, dashes};
    Point last;

    for (const Path::Piece& piece : path.pieces()) {
        switch (piece.kind) {
        case Path::Kind::move:
            flattening.moved(piece.points[0]);
            last = piece.points[0];
            break;
        case Path::Kind::line:
            flattening.lined(piece.points[0]);
            last = piece.points[0];
            break;
        case Path::Kind::quad: {
            const Point points[3] = {last, piece.points[0], piece.points[1]};

            flattening.curved(points, 3);
            last = piece.points[1];
            break;
        }
        case Path::Kind::cubic: {
            const Point points[4] = {last, piece.points[0], piece.points[1], piece.points[2]};

            flattening.curved(points, 4);
            last = piece.points[2];
            break;
        }
        case Path::Kind::arc:
            flattening.arced(piece);
            last = piece.points[1];
            break;
        case Path::Kind::close:
            flattening.closed();
            break;
        }
    }

    return std::move(flattening).made();
}

}
