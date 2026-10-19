#pragma once

#include "talcite/area.hpp"
#include "talcite/child.hpp"

namespace talcite {

[[nodiscard]] bool teletyped();

[[nodiscard]] Child teletype(const Command& what, ui::Size size);

void resized(const Child& one, ui::Size size);

void hung_up(const Child& one);

}
