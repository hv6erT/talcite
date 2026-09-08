#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "talcite/pixels.hpp"

using namespace talcite;

namespace {

// Próby piszą piksele zwykłymi liczbami, bo tak się je czyta; zamiana na `Bytes` siedzi w jednym
// miejscu, żeby nie rozłazić `std::byte{}` po stu wierszach oczekiwań.
Bytes lump(const std::vector<std::uint8_t>& bytes) {
    Bytes made;

    made.reserve(bytes.size());

    for (const std::uint8_t one : bytes) made.push_back(std::byte{one});

    return made;
}

// Obrazek z podanych pikseli, po cztery bajty na każdy.
Pixels made(ui::Size size, const std::vector<std::uint8_t>& bytes,
            std::chrono::milliseconds after = std::chrono::milliseconds{0}) {
    return Pixels{size, {Still{lump(bytes), after}}};
}

std::uint8_t band(const Pixels& one, int x, int y, std::size_t which, std::size_t still = 0) {
    const std::size_t at = (static_cast<std::size_t>(y) *
                                static_cast<std::size_t>(one.size().wide) +
                            static_cast<std::size_t>(x)) *
                           4;

    return std::to_integer<std::uint8_t>(one.stills()[still].bytes[at + which]);
}

}

// --- Budowa -------------------------------------------------------------------------------------

TRIAL("Pixels: pusty nic nie niesie") {
    const Pixels one;

    ought(!one.held());
    same(one.size().wide, 0);
    ought(one.stills().empty());
}

TRIAL("Pixels: klatka nie zgadzająca się z wymiarami jest błędem") {
    bool threw = false;

    try {
        (void)made({2, 2}, std::vector<std::uint8_t>(8, 0));
    } catch (const std::logic_error&) {
        threw = true;
    }

    ought(threw);
}

TRIAL("Pixels: klatki bez wymiarów są błędem") {
    bool threw = false;

    try {
        (void)made({0, 0}, std::vector<std::uint8_t>{});
    } catch (const std::logic_error&) {
        threw = true;
    }

    ought(threw);
}

TRIAL("Pixels: kilka klatek trzyma swoje opóźnienia") {
    const Pixels one{{1, 1},
                     {Still{lump({1, 2, 3, 255}), std::chrono::milliseconds{40}},
                      Still{lump({4, 5, 6, 255}), std::chrono::milliseconds{80}}}};

    same(one.stills().size(), static_cast<std::size_t>(2));
    same(one.stills()[0].after.count(), static_cast<long long>(40));
    same(one.stills()[1].after.count(), static_cast<long long>(80));
}

// --- Światło liniowe ----------------------------------------------------------------------------
//
// **To jest ta poprawka, dla której cały ten etap istnieje.** Bajt w pliku nie jest jasnością —
// niesie wartość po krzywej gamma. Średnia jasność czerni i bieli to 0,5, a bajtem 0,5 jest 188,
// nie 127. Uśrednianie surowych bajtów przyciemnia każdą krawędź; tak robi większość programów
// i tak robi fork.

TRIAL("Pixels: czerń i biel dają w połowie 188, a nie 127") {
    const Pixels one = made({2, 1}, {0, 0, 0, 255, 255, 255, 255, 255});

    const Pixels half = one.scaled({1, 1});

    same(static_cast<int>(band(half, 0, 0, 0)), 188);
    same(static_cast<int>(band(half, 0, 0, 3)), 255);
}

TRIAL("Pixels: jednolita barwa przetrwa zmniejszenie bez zmiany") {
    const Pixels red = made({4, 4}, [] {
        std::vector<std::uint8_t> these(64, 0);

        for (std::size_t at = 0; at < these.size(); at += 4) {
            these[at] = 200;
            these[at + 3] = 255;
        }

        return these;
    }());

    const Pixels small = red.scaled({2, 2});

    same(static_cast<int>(band(small, 0, 0, 0)), 200);
    same(static_cast<int>(band(small, 1, 1, 0)), 200);
    same(static_cast<int>(band(small, 1, 1, 1)), 0);
}

// --- Alfa ---------------------------------------------------------------------------------------
//
// Piksel przezroczysty niesie w pliku jakąś barwę, zwykle czarną. Wliczony na równi z widocznymi
// robi **ciemną obwódkę wokół każdego wyciętego kształtu** — błąd, który popełnia większość szybkich
// skalowań i którego potem nikt nie umie nazwać. Barwę ważymy więc alfą i przez nią dzielimy.

TRIAL("Pixels: przezroczysty sąsiad nie przyciemnia barwy") {
    const Pixels one = made({2, 1}, {255, 0, 0, 255, 0, 0, 0, 0});

    const Pixels half = one.scaled({1, 1});

    // Czerwień zostaje pełna — naiwne uśrednienie dałoby tu 128.
    same(static_cast<int>(band(half, 0, 0, 0)), 255);
    same(static_cast<int>(band(half, 0, 0, 1)), 0);

    // Przezroczystość uśrednia się normalnie: połowa z widocznego i niewidocznego.
    same(static_cast<int>(band(half, 0, 0, 3)), 128);
}

TRIAL("Pixels: sama przezroczystość zostaje przezroczystością") {
    const Pixels one = made({2, 1}, {255, 0, 0, 0, 0, 255, 0, 0});

    const Pixels half = one.scaled({1, 1});

    same(static_cast<int>(band(half, 0, 0, 3)), 0);
    same(static_cast<int>(band(half, 0, 0, 0)), 0);
}

// --- Skalowanie ---------------------------------------------------------------------------------

// Filtr pudełkowy bierze **wszystkie** piksele źródła, więc cienka jasna kreska nie znika przez to,
// że akurat nie trafiła w próbkę. Najbliższy sąsiad zgubiłby ją w trzech przypadkach na cztery.
TRIAL("Pixels: cienka kreska nie ginie przy zmniejszaniu") {
    const Pixels one =
        made({4, 1}, {0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255});

    const Pixels small = one.scaled({1, 1});

    ought(band(small, 0, 0, 0) > 0);
}

TRIAL("Pixels: zmniejszenie łączy kwadraty po równo") {
    const Pixels one = made({2, 2}, {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0,
                                     255});

    const Pixels small = one.scaled({1, 1});

    same(small.size().wide, 1);
    same(small.size().high, 1);
    same(static_cast<int>(band(small, 0, 0, 0)), 255);
}

// Powiększenie daje najbliższego sąsiada, czyli ostre kwadraty — i to jest **właściwa** odpowiedź
// dla ikony i pikselowej grafiki, a nie zaniedbanie. Rozmyty piksel-art wygląda gorzej.
TRIAL("Pixels: powiększenie mnoży piksele, a nie rozmywa") {
    const Pixels one = made({2, 1}, {10, 0, 0, 255, 200, 0, 0, 255});

    const Pixels big = one.scaled({4, 1});

    same(big.size().wide, 4);
    same(static_cast<int>(band(big, 0, 0, 0)), 10);
    same(static_cast<int>(band(big, 1, 0, 0)), 10);
    same(static_cast<int>(band(big, 2, 0, 0)), 200);
    same(static_cast<int>(band(big, 3, 0, 0)), 200);
}

TRIAL("Pixels: ten sam rozmiar oddaje to samo") {
    const Pixels one = made({2, 1}, {1, 2, 3, 255, 4, 5, 6, 255});

    const Pixels same_size = one.scaled({2, 1});

    same(static_cast<int>(band(same_size, 1, 0, 1)), 5);
    same(same_size.size().wide, 2);
}

TRIAL("Pixels: zerowy cel nie niesie nic") {
    const Pixels one = made({2, 1}, {1, 2, 3, 255, 4, 5, 6, 255});

    ought(!one.scaled({0, 4}).held());
    ought(!one.scaled({4, 0}).held());
    ought(!Pixels{}.scaled({4, 4}).held());
}

TRIAL("Pixels: opóźnienia przeżywają skalowanie") {
    const Pixels one{{2, 1},
                     {Still{lump({0, 0, 0, 255, 255, 255, 255, 255}), std::chrono::milliseconds{40}},
                      Still{lump({255, 255, 255, 255, 0, 0, 0, 255}), std::chrono::milliseconds{90}}}};

    const Pixels half = one.scaled({1, 1});

    same(half.stills().size(), static_cast<std::size_t>(2));
    same(half.stills()[0].after.count(), static_cast<long long>(40));
    same(half.stills()[1].after.count(), static_cast<long long>(90));
    same(static_cast<int>(band(half, 0, 0, 0, 1)), 188);
}

// --- Dopasowanie ---------------------------------------------------------------------------------
//
// **Kratka nie jest kwadratem.** U jednych jest 10 na 20 pikseli, u innych 9 na 21 — bez jej wymiaru
// koło wychodzi jajkiem. Zero znaczy „terminal nie powiedział" i wtedy przyjmujemy 1:2.

TRIAL("fitting: kwadrat w kratce 1:2 zajmuje dwa razy mniej wierszy") {
    const ui::Size made_size = fitting({100, 100}, {40, 40}, {10, 20});

    same(made_size.wide, 40);
    same(made_size.high, 20);
}

TRIAL("fitting: nieznana kratka znaczy 1:2") {
    same(fitting({100, 100}, {40, 40}, {0, 0}).high, 20);
    same(fitting({100, 100}, {40, 40}, {0, 0}).wide, 40);
}

TRIAL("fitting: kwadratowa kratka nie ściska wysokości") {
    const ui::Size one = fitting({100, 100}, {40, 40}, {10, 10});

    same(one.wide, 40);
    same(one.high, 40);
}

TRIAL("fitting: obrazek szeroki bierze całą szerokość") {
    const ui::Size one = fitting({100, 50}, {40, 20}, {10, 20});

    same(one.wide, 40);
    same(one.high, 10);
}

TRIAL("fitting: obrazek wysoki ogranicza się wysokością miejsca") {
    const ui::Size one = fitting({50, 100}, {40, 20}, {10, 20});

    same(one.wide, 20);
    same(one.high, 20);
}

// Obrazek zajmujący zero kratek to obrazek, którego nie widać — a wołający prosił o podgląd.
TRIAL("fitting: zawsze co najmniej jedna kratka") {
    same(fitting({1000, 1}, {40, 20}, {10, 20}).high, 1);
    same(fitting({1, 1000}, {40, 20}, {10, 20}).wide, 1);
}

TRIAL("fitting: cokolwiek zerowego daje nic") {
    same(fitting({0, 10}, {40, 20}, {10, 20}).wide, 0);
    same(fitting({10, 0}, {40, 20}, {10, 20}).wide, 0);
    same(fitting({10, 10}, {0, 20}, {10, 20}).wide, 0);
    same(fitting({10, 10}, {40, 0}, {10, 20}).wide, 0);
}
