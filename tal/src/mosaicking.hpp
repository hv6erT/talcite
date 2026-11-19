#pragma once

#include <cstddef>

#include "tal/capabilities.hpp"
#include "tal/geometry.hpp"
#include "tal/pixels.hpp"
#include "tal/style.hpp"
#include "tal/surface.hpp"

namespace tal::mosaicking {

[[nodiscard]] Size grain(Blocks kind, bool unicode = true);

void drawn(Canvas& into, Spot at, const media::Pixels& one, std::size_t still, Blocks kind, const Look& under = {}, Colors colors = Colors::rgb, bool unicode = true);

}
