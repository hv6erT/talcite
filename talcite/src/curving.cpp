#include "curving.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

// The field 2²⁵⁵ − 19 of curve 25519 in constant time: sixteen limbs of 16 bits each, as in
// TweetNaCl, with no branches and no memory access that depends on the data. Elligator2 in CPace
// and the X25519 ladder (when the system does not have the curve) compute on it.
//
// Ranges: `product` returns carried limbs (up to 16 bits); `sum` and `difference` do not carry,
// so their result may go into `product` right away, but not twice in a row. `small` puts a
// constant (up to 2²⁰) into the lowest limb without carrying; the product has headroom for it.
// `packed` evens out with three carries, also limbs that are negative or longer than 16 bits: two
// are not enough when the excess from the last limb comes back to the first one and that one
// carries it through all the full limbs.
//
// The header also holds `halving`, (p − 1) / 2 from the least significant byte, the exponent of
// the Legendre symbol.
namespace talcite::curving {

namespace {

constexpr std::size_t wide = 32;

// p − 2, from the least significant byte.
constexpr std::array<std::uint8_t, 32> inverting{0xeb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                                 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                                 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f};

}

// Carrying: every limb down to 16 bits; the excess from the last limb comes back multiplied by
// 38 (because 2²⁵⁶ ≡ 38).
void carried(Limbs& one) {
    for (std::size_t at = 0; at < 16; ++at) {
        one[at] += std::int64_t{1} << 16;

        const std::int64_t carry = one[at] >> 16;

        if (at < 15) one[at + 1] += carry - 1;
        else one[0] += 38 * (carry - 1);

        one[at] -= carry << 16;
    }
}

// Limb-by-limb sum, without carrying.
Limbs sum(const Limbs& one, const Limbs& two) {
    Limbs made{};

    for (std::size_t at = 0; at < 16; ++at) made[at] = one[at] + two[at];

    return made;
}

// Limb-by-limb difference, without carrying; limbs may become negative.
Limbs difference(const Limbs& one, const Limbs& two) {
    Limbs made{};

    for (std::size_t at = 0; at < 16; ++at) made[at] = one[at] - two[at];

    return made;
}

// The product modulo p: the upper half of the 31-limb schoolbook product folds back multiplied by
// 38, and two carries bring the limbs back to 16 bits.
Limbs product(const Limbs& one, const Limbs& two) {
    std::array<std::int64_t, 31> wide_product{};

    for (std::size_t i = 0; i < 16; ++i)
        for (std::size_t j = 0; j < 16; ++j) wide_product[i + j] += one[i] * two[j];

    for (std::size_t at = 0; at < 15; ++at) wide_product[at] += 38 * wide_product[at + 16];

    Limbs made{};

    for (std::size_t at = 0; at < 16; ++at) made[at] = wide_product[at];

    carried(made);
    carried(made);

    return made;
}

// A field element holding a small constant in the lowest limb, without carrying.
Limbs small(std::int64_t value) {
    Limbs made{};

    made[0] = value;

    return made;
}

// Constant-time swap: with `swap` = 1 the values change places, with 0 nothing happens.
void swapped(Limbs& one, Limbs& two, std::int64_t swap) {
    const std::int64_t mask = -swap;

    for (std::size_t at = 0; at < 16; ++at) {
        const std::int64_t flip = mask & (one[at] ^ two[at]);

        one[at] ^= flip;
        two[at] ^= flip;
    }
}

// A power with a public exponent (from the least significant byte), going from the exponent's
// most significant bit: a square, and for a one bit also a multiplication by the base. The
// branch depends on the exponent, not on the base.
Limbs powered(const Limbs& base, const std::array<std::uint8_t, 32>& exponent) {
    Limbs made = small(1);

    for (int bit = 255; bit >= 0; --bit) {
        made = product(made, made);

        if (((exponent[static_cast<std::size_t>(bit / 8)] >> (bit % 8)) & 1) != 0) made = product(made, base);
    }

    return made;
}

// The inverse as the power p − 2, so the inverse of zero is zero.
Limbs inverse(const Limbs& one) { return powered(one, inverting); }

// The canonical form, 32 bytes from the least significant one. After the carries the value is
// below 2²⁵⁶, which is less than 3p, so two subtractions of p are enough, both in constant time:
// the result of a subtraction is kept only when there was no borrow.
Bytes packed(const Limbs& one) {
    Limbs value = one;

    carried(value);
    carried(value);
    carried(value);

    for (int round = 0; round < 2; ++round) {
        Limbs less{};

        less[0] = value[0] - 0xffed;

        for (std::size_t at = 1; at < 15; ++at) {
            less[at] = value[at] - 0xffff - ((less[at - 1] >> 16) & 1);
            less[at - 1] &= 0xffff;
        }

        less[15] = value[15] - 0x7fff - ((less[14] >> 16) & 1);

        const std::int64_t borrowed = (less[15] >> 16) & 1;

        less[14] &= 0xffff;

        swapped(value, less, 1 - borrowed);
    }

    Bytes made(wide);

    for (std::size_t at = 0; at < 16; ++at) {
        made[2 * at] = static_cast<std::byte>(value[at] & 0xff);
        made[2 * at + 1] = static_cast<std::byte>((value[at] >> 8) & 0xff);
    }

    return made;
}

// The u coordinate from 32 bytes per RFC 7748: the top bit is dropped, and non-canonical values
// (from p up) count as if they were reduced.
Limbs unpacked(const Bytes& one) {
    Limbs made{};

    for (std::size_t at = 0; at < 16; ++at)
        made[at] = std::to_integer<std::int64_t>(one[2 * at]) + (std::to_integer<std::int64_t>(one[2 * at + 1]) << 8);

    made[15] &= 0x7fff;

    return made;
}

}
