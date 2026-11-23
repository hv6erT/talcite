#pragma once

#include <optional>
#include <vector>

#include "tal/drawing.hpp"

namespace tal::drawing::flattening {

struct Line {
    std::vector<Point> points;
    std::vector<bool> corners;
    std::vector<double> lengths;
    bool closed = false;
};

struct Bounds {
    double x0 = 0;
    double y0 = 0;
    double x1 = 0;
    double y1 = 0;
};

struct Dashes {
    std::vector<double> lengths;
    double offset = 0;
    double turning = 0;
    double reach = 0;
};

[[nodiscard]] Point ellipse_point(Point center, double rx, double ry, double turn, double angle);

[[nodiscard]] double stretch(const Matrix& matrix);

[[nodiscard]] std::vector<Line> flattened(const Path& path, const Matrix& matrix, double tolerance, std::optional<Bounds> view = std::nullopt, double turning = 0, const Dashes& dashes = {});

}
