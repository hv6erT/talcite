#pragma once

#include <cstddef>
#include <string>

#include "tal/pixels.hpp"

namespace tal::sixeling {

[[nodiscard]] std::string sixel(const media::Pixels& one, std::size_t still, std::size_t colors = 256);

}
