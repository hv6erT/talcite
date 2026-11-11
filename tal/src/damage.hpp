#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include "tal/geometry.hpp"

namespace tal::damage {

void added(std::vector<Area>& into, Area part, std::size_t most);

[[nodiscard]] std::array<Area, 2> shadowed(Area box);

}
