#pragma once

#include <cstddef>
#include <span>

#include "tal/bytes.hpp"

namespace tal::deflating {

[[nodiscard]] Bytes deflated(std::span<const std::byte> given);

}
