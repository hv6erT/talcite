#pragma once

#include "talcite/bytes.hpp"

namespace talcite {

[[nodiscard]] bool shielding();

[[nodiscard]] Bytes shielded(const Bytes& plain);

[[nodiscard]] Bytes unshielded(const Bytes& kept);

}
