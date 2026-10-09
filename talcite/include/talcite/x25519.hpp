#pragma once

#include <optional>

#include "talcite/bytes.hpp"

namespace talcite {

[[nodiscard]] std::optional<Bytes> x25519(const Bytes& scalar, const Bytes& u);

}
