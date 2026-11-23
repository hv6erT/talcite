#pragma once

#include <optional>
#include <vector>

#include "tal/drawing.hpp"

#include "flattening.hpp"

namespace tal::drawing::stroking {

[[nodiscard]] std::vector<double> pattern(const Stroke& style);

[[nodiscard]] std::vector<flattening::Line> outlined(const std::vector<flattening::Line>& lines, const Stroke& style, double tolerance,
                                                     const std::optional<flattening::Bounds>& view = std::nullopt);

}
