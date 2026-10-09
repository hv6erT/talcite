#pragma once

#include <cstddef>
#include <string>

#include "talcite/pixels.hpp"

namespace talcite {

[[nodiscard]] std::string sixel(const Pixels& one, std::size_t still, std::size_t colors = 256);

}
