#pragma once

#include <array>
#include <cstdint>

#include "talcite/bytes.hpp"

// Ciało 2²⁵⁵ − 19 krzywej 25519 w czasie stałym: szesnaście kończyn po 16 bitów, jak w TweetNaCl, bez
// gałęzi i bez dostępu do pamięci zależnego od danych. Liczą na nim Elligator2 w CPace i drabina
// X25519, gdy system krzywej nie ma.
//
// Zakresy: `product` oddaje kończyny przeniesione (do 16 bitów); `sum` i `difference` nie przenoszą,
// więc ich wynik wolno dać do `product` od razu, ale nie dwa razy z rzędu. `small` kładzie stałą
// (do 2²⁰) w najmłodszej kończynie bez przeniesienia — iloczyn ma na nią zapas. `packed` wyrównuje
// trzema przeniesieniami także kończyny ujemne i dłuższe niż 16 bitów: dwa nie wystarczą, gdy nadmiar
// z ostatniej kończyny wraca do pierwszej, a ta przenosi go przez wszystkie pełne.
namespace talcite::curving {

using Limbs = std::array<std::int64_t, 16>;

void carried(Limbs& one);

[[nodiscard]] Limbs sum(const Limbs& one, const Limbs& two);

[[nodiscard]] Limbs difference(const Limbs& one, const Limbs& two);

[[nodiscard]] Limbs product(const Limbs& one, const Limbs& two);

[[nodiscard]] Limbs small(std::int64_t value);

// Zamiana w czasie stałym: przy `swap` = 1 wartości zamieniają się miejscami, przy 0 — nic.
void swapped(Limbs& one, Limbs& two, std::int64_t swap);

// Potęga o jawnym wykładniku (od najmłodszego bajtu): gałąź zależy od wykładnika, nie od podstawy.
[[nodiscard]] Limbs powered(const Limbs& base, const std::array<std::uint8_t, 32>& exponent);

// (p − 1) / 2, od najmłodszego bajtu — wykładnik symbolu Legendre'a.
inline constexpr std::array<std::uint8_t, 32> halving{0xf6, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                                      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                                      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f};

// Odwrotność jako potęga p − 2, więc odwrotność zera to zero.
[[nodiscard]] Limbs inverse(const Limbs& one);

// Postać kanoniczna, 32 bajty od najmłodszego.
[[nodiscard]] Bytes packed(const Limbs& one);

// Współrzędna u według RFC 7748 z 32 bajtów: najstarszy bit odrzucony, wartości niekanoniczne
// (od p w górę) liczą się tak, jakby były zredukowane.
[[nodiscard]] Limbs unpacked(const Bytes& one);

// Zerowanie, którego kompilator nie usunie — stan drabiny to pochodna sekretu.
void wiped(Limbs& one);

}
