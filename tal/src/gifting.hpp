#pragma once

#include <cstddef>
#include <span>

#include "tal/pixels.hpp"

namespace tal::media::gifting {

void gifted(std::span<const std::byte> what, const Taking& each);

}
