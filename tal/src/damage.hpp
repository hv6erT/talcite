#pragma once

#include <cstddef>
#include <vector>

#include "tal/geometry.hpp"

namespace tal::damage {

void added(std::vector<Area>& into, Area part, std::size_t most);

}
