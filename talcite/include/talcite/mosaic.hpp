#pragma once

#include <cstddef>

#include "talcite/area.hpp"
#include "talcite/blocks.hpp"
#include "talcite/pixels.hpp"
#include "talcite/view.hpp"

namespace talcite {

[[nodiscard]] ui::Size grain(ui::Blocks kind);

void drawn(View& into, ui::Spot at, const Pixels& one, std::size_t still, ui::Blocks kind,
           const Style& under = {});

}
