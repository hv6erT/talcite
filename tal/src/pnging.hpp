#pragma once

#include <cstddef>
#include <span>

#include "tal/pixels.hpp"

namespace tal::media::pnging {

void pnged(std::span<const std::byte> what, const Taking& each, Size most = {});

[[nodiscard]] Bytes encoded(std::span<const std::byte> rgba, Size size);

}
