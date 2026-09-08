#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "lighting.hpp"
#include "picturing.hpp"

namespace talcite {

namespace {

// Rozpoznanie idzie **z nagłówka pliku, nie z nazwy**. Menedżer plików ogląda cudze dyski, a tam
// rozszerzenie bywa czym innym niż treść — i akurat obrazek jest tym miejscem, gdzie kłamstwo
// w nazwie jest najczęstsze.
[[nodiscard]] bool leads(const Bytes& what, const std::uint8_t* sign, std::size_t much) {
    if (what.size() < much) return false;

    for (std::size_t at = 0; at < much; ++at)
        if (sample(what[at]) != sign[at]) return false;

    return true;
}

constexpr std::uint8_t png[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
constexpr std::uint8_t jpeg[3] = {0xFF, 0xD8, 0xFF};
constexpr std::uint8_t bmp[2] = {'B', 'M'};
constexpr std::uint8_t gif[4] = {'G', 'I', 'F', '8'};

}

Picture guessed(const Bytes& what) {
    if (leads(what, png, 8)) return Picture::png;

    if (leads(what, jpeg, 3)) return Picture::jpeg;

    // Sam „BM" to dwa bajty i trafiłby w niejeden plik tekstowy, więc żądamy jeszcze **całego
    // nagłówka pliku**: czternaście bajtów, a za nimi nagłówek mapy.
    if (leads(what, bmp, 2) && what.size() >= 14 + 12) return Picture::bmp;

    if (leads(what, gif, 4) && what.size() >= 6) {
        const std::uint8_t age = sample(what[4]);

        if ((age == '7' || age == '9') && sample(what[5]) == 'a') return Picture::gif;
    }

    return Picture::none;
}

void decoded(const Bytes& what, const Taking& each) {
    if (!each) return;

    switch (guessed(what)) {
        case Picture::gif:
            (void)gifted(what, each);

            return;

        // PNG, JPEG i BMP rozpoznajemy, ale jeszcze nie czytamy. Rozpoznanie bez dekodera nie jest
        // połowicznością: dzięki niemu widżet powie „JPEG — tu nieobsługiwany" zamiast pokazać
        // puste pole, a to jest różnica między brakiem funkcji a wrażeniem zepsucia.
        default:
            return;
    }
}

// Zebranie strumienia w `Pixels`. **Budżet jest jeden i wydaje się go albo na rozdzielczość, albo
// na czas** — pierwsza klatka mieści się zawsze, bo dekoder odmówiłby wcześniej płótna ponad sufit,
// a każda następna musi się jeszcze w nim zmieścić.
Pixels decoded(const Bytes& what, std::size_t most) {
    std::vector<Still> made;
    std::size_t spent = 0;
    ui::Size size{};

    decoded(what, [&](const Bytes& canvas, ui::Size given, std::chrono::milliseconds after) {
        const std::size_t much =
            static_cast<std::size_t>(given.wide) * static_cast<std::size_t>(given.high);

        if (much == 0 || spent + much > ceiling) return false;

        spent += much;
        size = given;

        made.push_back(Still{canvas, after});

        return most == 0 || made.size() < most;
    });

    if (made.empty()) return {};

    return Pixels{size, std::move(made)};
}

}
