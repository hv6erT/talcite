#pragma once

#include <vector>

#include "tal/node.hpp"

namespace tal::dividing {

[[nodiscard]] std::vector<int> divided(const std::vector<int>& wanted, const std::vector<Share>& shares, int room);

[[nodiscard]] std::vector<bool> kept(const std::vector<int>& wanted, const std::vector<Share>& shares, int room, int gap);

[[nodiscard]] std::vector<int> yielded(const std::vector<int>& wanted, const std::vector<Share>& shares, int room, int gap);

}
