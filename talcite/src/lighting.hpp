#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace talcite {

// Szew między tym, co leży w buforze, a tym, na czym się liczy. `Bytes` to `std::byte`, bo tak
// wygląda w talcite każda bryła danych — z dysku, z sieci, z dekodera — ale arytmetyki na
// `std::byte` nie ma i być nie powinno. Obie zamiany kosztują **zero w czasie wykonania**:
// `std::byte` jest wyliczeniem o zakresie `unsigned char`, więc po obu stronach stoi ta sama
// liczba w tym samym rejestrze, a cały koszt ponosi kompilator.
[[nodiscard]] constexpr std::uint8_t sample(std::byte one) {
    return std::to_integer<std::uint8_t>(one);
}

[[nodiscard]] constexpr std::byte stored(std::uint8_t one) { return std::byte{one}; }

// **Bajt w pliku nie jest jasnością.** sRGB niesie wartość po krzywej gamma, więc dwa piksele
// o wartościach 0 i 255 mają średnią jasność ~0,5 — a to jest bajt 188, nie 127. Uśrednianie
// surowych bajtów przyciemnia wszystko, co ma krawędź: to jest ten szary muł, po którym poznaje
// się zmniejszony obrazek w większości programów.
//
// Stoi **w jednym miejscu**, bo liczy to i skalowanie, i mozaika — a dwie kopie tej samej tablicy
// w dwóch jednostkach to ta sama pułapka, którą złapaliśmy przy `HandPromise` i przy `along`.
inline const std::array<double, 256>& straight() {
    static const std::array<double, 256> made = [] {
        std::array<double, 256> these{};

        for (std::size_t at = 0; at < these.size(); ++at) {
            const double one = static_cast<double>(at) / 255.0;

            these[at] = one <= 0.04045 ? one / 12.92 : std::pow((one + 0.055) / 1.055, 2.4);
        }

        return these;
    }();

    return made;
}

// Droga powrotna. Zamiast liczyć potęgę na każdy piksel — co przy pełnoekranowym obrazku znaczy
// miliony wywołań — szukamy po **progach między sąsiednimi bajtami**. Wynik jest zaokrąglony
// dokładnie tak, jak zaokrągliłaby potęga, a kosztuje osiem porównań.
inline const std::array<double, 255>& edges() {
    static const std::array<double, 255> made = [] {
        const std::array<double, 256>& these = straight();

        std::array<double, 255> among{};

        for (std::size_t at = 0; at + 1 < these.size(); ++at)
            among[at] = (these[at] + these[at + 1]) / 2.0;

        return among;
    }();

    return made;
}

[[nodiscard]] inline std::uint8_t bent(double one) {
    const std::array<double, 255>& among = edges();

    const auto found = std::upper_bound(among.begin(), among.end(), one);

    return static_cast<std::uint8_t>(found - among.begin());
}

}
