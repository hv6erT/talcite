#include "tal/drawing.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numbers>
#include <string>
#include <utility>

#include "tal/trouble.hpp"

#include "flattening.hpp"

// Drawing (E7a): the geometry a pen draws with. A path keeps what it was told in its own
// coordinates, as Path2D of the browser does, and a pen maps it through its matrix when it draws;
// so one path can be drawn anywhere. The pieces of a path follow the rules of Canvas: every subpath
// begins with a move (a line, curve or arc with no point yet makes one), a closed subpath is
// followed, when anything but a move comes, by a move back to its start, and an arc first draws a
// line from the point so far to where it begins. Matrices are those of Canvas, (a c e; b d f), the
// y axis pointing down, so that a positive turn goes clockwise on the screen.
//
// A number that is not finite is a mistake of the program (most often a simulation gone astray),
// and it is told at once as Trouble malformed rather than drawn as nothing; the path or the paint
// stays as it was.
namespace tal::drawing {

namespace {

// Throws Trouble malformed when any of `numbers` is not finite.
void finite(std::initializer_list<double> numbers) {
    for (const double one : numbers)
        if (!std::isfinite(one)) throw Trouble{tal::Kind::malformed, "A point, size or angle that is not a finite number."};
}

// Throws Trouble malformed when any part of `color` is not finite.
void finite(const Color& color) {
    if (!std::isfinite(color.r) || !std::isfinite(color.g) || !std::isfinite(color.b) || !std::isfinite(color.a)) throw Trouble{tal::Kind::malformed, "A color that is not a finite number."};
}

// Throws Trouble malformed when any part of `matrix` is not finite.
void finite(const Matrix& matrix) { finite({matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f}); }

}

// A move by `x` and `y`.
Matrix Matrix::moved(double x, double y) { return Matrix{1, 0, 0, 1, x, y}; }

// A scaling by `x` across and `y` down.
Matrix Matrix::scaled(double x, double y) { return Matrix{x, 0, 0, y, 0, 0}; }

// A turn by `radians`, clockwise on the screen (the y axis points down).
Matrix Matrix::turned(double radians) {
    const double cosine = std::cos(radians);
    const double sine = std::sin(radians);

    return Matrix{cosine, sine, -sine, cosine, 0, 0};
}

// `one` mapped by the matrix.
Point Matrix::applied(Point one) const { return Point{a * one.x + c * one.y + e, b * one.x + d * one.y + f}; }

// This matrix and then `next`: a point is mapped by this one first.
Matrix Matrix::then(const Matrix& next) const {
    return Matrix{next.a * a + next.c * b, next.b * a + next.d * b, next.a * c + next.c * d, next.b * c + next.d * d, next.a * e + next.c * f + next.e, next.b * e + next.d * f + next.f};
}

// The matrix that undoes this one, none when it flattens the plane (or is not finite).
std::optional<Matrix> Matrix::inverted() const {
    const double determinant = a * d - b * c;

    if (determinant == 0 || !std::isfinite(determinant)) return std::nullopt;

    const Matrix made{d / determinant, -b / determinant, -c / determinant, a / determinant, (c * f - d * e) / determinant, (b * e - a * f) / determinant};

    if (!std::isfinite(made.a) || !std::isfinite(made.b) || !std::isfinite(made.c) || !std::isfinite(made.d) || !std::isfinite(made.e) || !std::isfinite(made.f)) return std::nullopt;

    return made;
}

// A gradient of `shape` between `first` and `second`, with the radii `inner` and `outer` when
// radial.
Gradient::Gradient(Shape shape, Point first, Point second, double inner, double outer) : shape_(shape), first_(first), second_(second), inner_(inner), outer_(outer) {}

// A linear gradient: its colors run from `from` (0) to `to` (1) and stay at the end colors beyond.
Gradient Gradient::linear(Point from, Point to) {
    finite({from.x, from.y, to.x, to.y});

    return Gradient{Shape::linear, from, to, 0, 0};
}

// A radial gradient of two circles, as in Canvas: the circle of `inner` and radius `from` (0) grows
// into that of `outer` and radius `to` (1), and a point takes the color of the last circle through
// it. Throws Trouble malformed for a negative radius.
Gradient Gradient::radial(Point inner, double from, Point outer, double to) {
    finite({inner.x, inner.y, from, outer.x, outer.y, to});

    if (from < 0 || to < 0) throw Trouble{tal::Kind::malformed, "A gradient circle of a negative radius."};

    return Gradient{Shape::radial, inner, outer, from, to};
}

// The color `color` at `at`, between 0 and 1; stops at one place keep the order they came in, so
// that two of them make a sharp edge. Throws Trouble malformed for a place outside [0, 1].
void Gradient::stop(double at, Color color) {
    finite({at});
    finite(color);

    if (at < 0 || at > 1) throw Trouble{tal::Kind::malformed, "A gradient stop outside 0 to 1."};

    const auto later = std::upper_bound(stops_.begin(), stops_.end(), at, [](double place, const std::pair<double, Color>& one) { return place < one.first; });

    stops_.insert(later, std::pair{at, color});
}

// The still `still` of `image` repeated across and down as asked. An image of no pixels paints
// nothing; throws Trouble other for a still the image does not have.
Pattern::Pattern(media::Pixels image, bool across, bool down, std::size_t still) : image_(std::make_shared<const media::Pixels>(std::move(image))), still_(still), across_(across), down_(down) {
    if (image_->held() && still_ >= image_->stills().size()) throw Trouble{tal::Kind::other, "A pattern of a still the image does not have."};
}

// The pattern's own matrix, from its pixels to the coordinates of the path drawn.
void Pattern::place(const Matrix& where) {
    finite(where);
    placed_ = where;
}

// A new subpath at `to`.
void Path::move(Point to) {
    finite({to.x, to.y});
    added(Piece{.kind = Kind::move, .points = {to, Point{}, Point{}}});
    start_ = to;
    last_ = to;
}

// A straight line to `to`; with no point yet, a new subpath there.
void Path::line(Point to) {
    finite({to.x, to.y});

    if (!last_) {
        move(to);

        return;
    }

    added(Piece{.kind = Kind::line, .points = {to, Point{}, Point{}}});
    last_ = to;
}

// A quadratic curve by `control` to `to`; with no point yet, starting at `control`.
void Path::quad(Point control, Point to) {
    finite({control.x, control.y, to.x, to.y});

    if (!last_) started(control);

    added(Piece{.kind = Kind::quad, .points = {control, to, Point{}}});
    last_ = to;
}

// A cubic curve by `first` and `second` to `to`; with no point yet, starting at `first`.
void Path::cubic(Point first, Point second, Point to) {
    finite({first.x, first.y, second.x, second.y, to.x, to.y});

    if (!last_) started(first);

    added(Piece{.kind = Kind::cubic, .points = {first, second, to}});
    last_ = to;
}

// An arc of the circle around `center`, see `ellipse`.
void Path::arc(Point center, double radius, double from, double to, bool backwards) { ellipse(center, radius, radius, 0, from, to, backwards); }

// An arc of the ellipse around `center` of radii `rx` and `ry`, turned by `turn`, from the angle
// `from` to `to`: clockwise on the screen, or counterclockwise when `backwards`. As in Canvas, a
// whole turn or more is the whole ellipse, anything less goes the asked way round to the end angle,
// and a straight line first joins the point so far to the start of the arc. Throws Trouble
// malformed for a negative radius.
void Path::ellipse(Point center, double rx, double ry, double turn, double from, double to, bool backwards) {
    finite({center.x, center.y, rx, ry, turn, from, to});

    if (rx < 0 || ry < 0) throw Trouble{tal::Kind::malformed, "An arc of a negative radius."};

    constexpr double whole = 2 * std::numbers::pi;
    double sweep = 0;

    if (!backwards) {
        if (to - from >= whole) {
            sweep = whole;
        } else {
            sweep = std::fmod(to - from, whole);

            if (sweep < 0) sweep += whole;
        }
    } else {
        if (from - to >= whole) {
            sweep = -whole;
        } else {
            sweep = std::fmod(from - to, whole);

            if (sweep < 0) sweep += whole;

            sweep = -sweep;
        }
    }

    finite({sweep, from + sweep});

    const Point begin = flattening::ellipse_point(center, rx, ry, turn, from);
    const Point end = flattening::ellipse_point(center, rx, ry, turn, from + sweep);

    finite({begin.x, begin.y, end.x, end.y});

    if (last_) {
        added(Piece{.kind = Kind::line, .points = {begin, Point{}, Point{}}});
    } else {
        added(Piece{.kind = Kind::move, .points = {begin, Point{}, Point{}}});
        start_ = begin;
    }

    added(Piece{.kind = Kind::arc, .points = {center, end, Point{}}, .rx = rx, .ry = ry, .turn = turn, .from = from, .sweep = sweep});
    last_ = end;
}

// The rectangle `box` as a closed subpath of its own, and then a new subpath at its corner, as in
// Canvas.
void Path::rectangle(Rect box) {
    finite({box.at.x, box.at.y, box.wide, box.high});

    const Point corner = box.at;

    finite({corner.x + box.wide, corner.y + box.high});
    move(corner);
    line(Point{corner.x + box.wide, corner.y});
    line(Point{corner.x + box.wide, corner.y + box.high});
    line(Point{corner.x, corner.y + box.high});
    close();
    move(corner);
}

// The subpath so far closed back to its start; nothing when there is none or it is closed already.
void Path::close() {
    if (!last_ || pieces_.empty() || pieces_.back().kind == Kind::close) return;

    added(Piece{.kind = Kind::close});
    last_ = start_;
}

// A new subpath at `at`, for a curve that came with no point.
void Path::started(Point at) {
    added(Piece{.kind = Kind::move, .points = {at, Point{}, Point{}}});
    start_ = at;
    last_ = at;
}

// `one` added; after a closed subpath anything but a move first moves back to its start. Throws
// Trouble full past `most_pieces`.
void Path::added(const Piece& one) {
    const bool reopened = !pieces_.empty() && pieces_.back().kind == Kind::close && one.kind != Kind::move && one.kind != Kind::close;

    if (pieces_.size() + (reopened ? 2 : 1) > most_pieces) throw Trouble{tal::Kind::full, "A path of more than " + std::to_string(most_pieces) + " pieces."};

    if (reopened) pieces_.push_back(Piece{.kind = Kind::move, .points = {*start_, Point{}, Point{}}});

    pieces_.push_back(one);
}

}
