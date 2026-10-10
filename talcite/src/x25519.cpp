#include "talcite/x25519.hpp"

#include <cstddef>
#include <cstdint>

#include "talcite/trouble.hpp"

#include "curving.hpp"

// X25519 from RFC 7748, for when the system does not provide it (CNG has curve 25519 only from
// Windows 10 on): the Montgomery ladder on the u coordinate alone, in projective form (X : Z), over
// the field from `curving`.
//
// The ladder invariant: R1 − R0 = P. On every bit of the scalar, from the most significant, one of
// them doubles and the other becomes their sum; the difference stays P, so differential addition
// is enough. Which point doubles depends on a secret bit: instead of a branch there is a
// constant-time swap by mask, done only when the bit differs from the previous one. At the end
// u = X0 · Z0^(p − 2). There is no swap after the loop as in the RFC: the last bit is clamped to
// zero, so no swap is left pending.
//
// The scalar is clamped as in the RFC (bits 0–2 and 255 zero, bit 254 one) without a copy: the
// clamped bits follow from the bit index, which is public. A zero result (a point of small order,
// also when written non-canonically) is a refusal, as in libcrypto and in the CNG adapter. The
// ladder state is derived from the secret, so it is wiped at the end.
namespace talcite {

namespace {

constexpr std::size_t wide = 32;

// (A − 2) / 4 for A = 486662.
constexpr std::int64_t a24 = 121665;

// Bit `at` of the clamped scalar (little-endian): bit 254 is always one, bits 0 to 2 always zero.
std::int64_t bit(const Bytes& scalar, int at) {
    if (at == 254) return 1;

    if (at < 3) return 0;

    return (std::to_integer<std::int64_t>(scalar[static_cast<std::size_t>(at / 8)]) >> (at % 8)) & 1;
}

}

// The X25519 function: `scalar` times the point with coordinate `u`. Throws when the scalar is not
// 32 bytes; returns nothing for a `u` of the wrong length or an all-zero result.
std::optional<Bytes> x25519(const Bytes& scalar, const Bytes& u) {
    using namespace curving;

    if (scalar.size() != wide) throw Trouble{"An X25519 key has 32 bytes."};

    if (u.size() != wide) return std::nullopt;

    const Limbs x1 = unpacked(u);
    Limbs x2 = small(1);
    Limbs z2{};
    Limbs x3 = x1;
    Limbs z3 = small(1);

    std::int64_t swap = 0;

    for (int at = 254; at >= 0; --at) {
        const std::int64_t now = bit(scalar, at);

        swap ^= now;

        swapped(x2, x3, swap);
        swapped(z2, z3, swap);

        swap = now;

        const Limbs a = sum(x2, z2);
        const Limbs aa = product(a, a);
        const Limbs b = difference(x2, z2);
        const Limbs bb = product(b, b);
        const Limbs e = difference(aa, bb);
        const Limbs c = sum(x3, z3);
        const Limbs d = difference(x3, z3);
        const Limbs da = product(d, a);
        const Limbs cb = product(c, b);

        const Limbs added = sum(da, cb);
        const Limbs taken = difference(da, cb);

        x3 = product(added, added);
        z3 = product(x1, product(taken, taken));
        x2 = product(aa, bb);
        z2 = product(e, sum(aa, product(small(a24), e)));
    }

    Limbs result = product(x2, inverse(z2));

    Bytes made = packed(result);

    wiped(x2);
    wiped(z2);
    wiped(x3);
    wiped(z3);
    wiped(result);

    std::byte any{0};

    for (const std::byte one : made) any |= one;

    if (any == std::byte{0}) return std::nullopt;

    return made;
}

}
