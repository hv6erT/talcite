#pragma once

#include <vector>

#include "talcite/area.hpp"
#include "talcite/node.hpp"

namespace talcite::ui::lineup {

[[nodiscard]] std::vector<Area> placed(const Children& all, Size room, int gap, Axis along);

[[nodiscard]] Size wanted(const std::vector<Area>& places, Size room);

void laid(const Children& all, Size given, int gap, Axis along);

}
