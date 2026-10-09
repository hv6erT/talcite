#include "talcite/x25519.hpp"

#include <cstddef>
#include <cstdint>

#include "talcite/trouble.hpp"

#include "curving.hpp"

// **X25519 z RFC 7748**, gdy system go nie daje (CNG ma krzywą 25519 dopiero od Windows 10): drabina
// Montgomery'ego na samej współrzędnej u, w rzucie (X : Z), na ciele z `curving`.
//
// Niezmiennik drabiny: R1 − R0 = P. Na każdym bicie skalara od najstarszego jedno z nich się podwaja,
// a drugie staje się ich sumą — różnica zostaje P, więc wystarcza dodawanie różnicowe. Który punkt
// się podwaja, zależy od bitu sekretu: zamiast gałęzi zamiana w czasie stałym, maską, i to tylko
// wtedy, gdy bit różni się od poprzedniego. Na końcu u = X0 · Z0^(p − 2). Zamiany po pętli, jak
// w RFC, nie ma: ostatni bit jest przycięty do zera, więc żadna zamiana nie zostaje w zawieszeniu.
//
// Skalar przycinamy jak RFC (bity 0–2 i 255 zero, bit 254 jeden) bez kopii — przycięte bity wynikają
// z numeru, a ten jest jawny. Wynik zerowy (punkt małego rzędu, także zapisany niekanonicznie) to
// odmowa, jak w libcrypto i w przejściówce CNG. Stan drabiny to pochodna sekretu — na końcu go
// zerujemy.
namespace talcite {

namespace {

constexpr std::size_t wide = 32;

// (A − 2) / 4 dla A = 486662.
constexpr std::int64_t a24 = 121665;

std::int64_t bit(const Bytes& scalar, int at) {
    if (at == 254) return 1;

    if (at < 3) return 0;

    return (std::to_integer<std::int64_t>(scalar[static_cast<std::size_t>(at / 8)]) >> (at % 8)) & 1;
}

}

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
