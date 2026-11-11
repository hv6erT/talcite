#pragma once

#include <vector>

#include "tal/node.hpp"

namespace tal::dividing {

[[nodiscard]] std::vector<int> divided(const std::vector<int>& wanted, const std::vector<Share>& shares, int room);

}
