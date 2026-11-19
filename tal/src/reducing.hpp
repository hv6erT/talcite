#pragma once

#include <cstdint>

#include "tal/pixels.hpp"

namespace tal::media {

inline constexpr std::uint64_t most_source = 32 * ceiling;

[[nodiscard]] std::uint32_t reduction(Size source, Size most);

[[nodiscard]] Size reduced(Size source, std::uint32_t by);

}
