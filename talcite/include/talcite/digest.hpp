#pragma once

#include <cstdint>

#include "talcite/bytes.hpp"

namespace talcite {

enum class Sum : std::uint8_t { md5, sha1, sha256 };

[[nodiscard]] bool summing();

[[nodiscard]] Bytes summed(Sum which, const Bytes& what);

[[nodiscard]] Bytes stamped(Sum which, const Bytes& key, const Bytes& what);

}
