#include "curving.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace talcite::curving {

namespace {

constexpr std::size_t wide = 32;

// p − 2, od najmłodszego bajtu.
constexpr std::array<std::uint8_t, 32> inverting{0xeb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                                 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                                 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f};

}

// Przeniesienie: każda kończyna do 16 bitów, nadmiar z ostatniej wraca jako 38 · (bo 2²⁵⁶ ≡ 38).
void carried(Limbs& one) {
    for (std::size_t at = 0; at < 16; ++at) {
        one[at] += std::int64_t{1} << 16;

        const std::int64_t carry = one[at] >> 16;

        if (at < 15) one[at + 1] += carry - 1;
        else one[0] += 38 * (carry - 1);

        one[at] -= carry << 16;
    }
}

Limbs sum(const Limbs& one, const Limbs& two) {
    Limbs made{};

    for (std::size_t at = 0; at < 16; ++at) made[at] = one[at] + two[at];

    return made;
}

Limbs difference(const Limbs& one, const Limbs& two) {
    Limbs made{};

    for (std::size_t at = 0; at < 16; ++at) made[at] = one[at] - two[at];

    return made;
}

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

Limbs small(std::int64_t value) {
    Limbs made{};

    made[0] = value;

    return made;
}

void swapped(Limbs& one, Limbs& two, std::int64_t swap) {
    const std::int64_t mask = -swap;

    for (std::size_t at = 0; at < 16; ++at) {
        const std::int64_t flip = mask & (one[at] ^ two[at]);

        one[at] ^= flip;
        two[at] ^= flip;
    }
}

// Od najstarszego bitu wykładnika: kwadrat, a przy jedynce jeszcze mnożenie przez podstawę.
Limbs powered(const Limbs& base, const std::array<std::uint8_t, 32>& exponent) {
    Limbs made = small(1);

    for (int bit = 255; bit >= 0; --bit) {
        made = product(made, made);

        if (((exponent[static_cast<std::size_t>(bit / 8)] >> (bit % 8)) & 1) != 0) made = product(made, base);
    }

    return made;
}

Limbs inverse(const Limbs& one) { return powered(one, inverting); }

// Po przeniesieniach wartość jest poniżej 2²⁵⁶, a to mniej niż 3p — wystarczą dwa odjęcia p, oba
// w czasie stałym: wynik odjęcia zostaje tylko wtedy, gdy nie było pożyczki.
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

Limbs unpacked(const Bytes& one) {
    Limbs made{};

    for (std::size_t at = 0; at < 16; ++at)
        made[at] = std::to_integer<std::int64_t>(one[2 * at]) + (std::to_integer<std::int64_t>(one[2 * at + 1]) << 8);

    made[15] &= 0x7fff;

    return made;
}

void wiped(Limbs& one) {
    volatile std::int64_t* at = one.data();

    for (std::size_t i = 0; i < one.size(); ++i) at[i] = 0;
}

}
