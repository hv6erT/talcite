#pragma once

#include <cstddef>
#include <span>

#include "talcite/pixels.hpp"

namespace talcite {

// **Jeden budżet na obrazek, wydawany albo na rozdzielczość, albo na czas.** Jedna klatka nie może
// być większa, i suma klatek nie może być większa. Osiem milionów pikseli to trzydzieści dwa
// megabajty RGBA: zdjęcie 3840x2000 przechodzi jako pojedyncza klatka, a animacja 500x500 dostaje
// trzydzieści dwie klatki i tyle. Dzięki temu **żaden GIF nie kosztuje więcej niż jedno duże
// zdjęcie** — a to jest ta granica, której przeglądarki pilnują od zawsze i której brak poznaje
// się po zjedzonej pamięci.
inline constexpr std::size_t ceiling = 8'000'000;

// Postać strumieniowa jest tu jedyna, bo dekoder animacji **z natury podaje klatki po kolei**,
// a `Pixels` z całą animacją to tylko jeden ze sposobów ich zebrania. Fałsz od wołającego znaczy
// „dosyć" i nie jest błędem. Zwracana prawda znaczy, że wyszła co najmniej jedna klatka.
[[nodiscard]] bool gifted(std::span<const std::byte> what, const Taking& each);

}
