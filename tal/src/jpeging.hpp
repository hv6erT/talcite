#pragma once

#include <cstddef>
#include <span>

#include "tal/pixels.hpp"

namespace tal::media::jpeging {

void jpeged(std::span<const std::byte> what, const Taking& each, Size most = {});

}
