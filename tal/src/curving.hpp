#pragma once

#include <array>
#include <cstdint>

#include "tal/bytes.hpp"

namespace tal::crypto::curving {

using Limbs = std::array<std::int64_t, 16>;

void carried(Limbs& one);

[[nodiscard]] Limbs sum(const Limbs& one, const Limbs& two);

[[nodiscard]] Limbs difference(const Limbs& one, const Limbs& two);

[[nodiscard]] Limbs product(const Limbs& one, const Limbs& two);

[[nodiscard]] Limbs small(std::int64_t value);

void swapped(Limbs& one, Limbs& two, std::int64_t swap);

[[nodiscard]] Limbs powered(const Limbs& base, const std::array<std::uint8_t, 32>& exponent);

inline constexpr std::array<std::uint8_t, 32> halving{0xf6, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                                      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                                      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f};

[[nodiscard]] Limbs inverse(const Limbs& one);

[[nodiscard]] Bytes packed(const Limbs& one);

[[nodiscard]] Limbs unpacked(const Bytes& one);

}
