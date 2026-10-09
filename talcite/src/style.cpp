#include "talcite/style.hpp"

#include <array>
#include <cstdint>

// **Paleta 256 barw, jedna dla biblioteki i dla aplikacji.** Szesnaście podstawowych w wartościach
// xterma, sześcian 6×6×6 i rampa szarości. Terminal liczy z niej najbliższą barwę przy wypisywaniu
// w 16 i 256 barwach, a aplikacja czyta z niej barwy podane numerem w pliku skórki. Dawniej obie
// strony trzymały własną kopię tych samych tablic — twa w `look.cpp`, talcite w `term.cpp` — i nic
// nie pilnowało, żeby `blue` z pliku wracało na terminal jako `blue`.
namespace talcite {

namespace {

constexpr std::array<std::array<std::uint8_t, 3>, 16> basics{{{0, 0, 0},
                                                            {128, 0, 0},
                                                            {0, 128, 0},
                                                            {128, 128, 0},
                                                            {0, 0, 128},
                                                            {128, 0, 128},
                                                            {0, 128, 128},
                                                            {192, 192, 192},
                                                            {128, 128, 128},
                                                            {255, 0, 0},
                                                            {0, 255, 0},
                                                            {255, 255, 0},
                                                            {0, 0, 255},
                                                            {255, 0, 255},
                                                            {0, 255, 255},
                                                            {255, 255, 255}}};

constexpr std::array<std::uint8_t, 6> steps{0, 95, 135, 175, 215, 255};

Color solid(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    Color made;

    made.transparent = false;
    made.red = red;
    made.green = green;
    made.blue = blue;

    return made;
}

}

// Numer palety na barwę: 0–15 podstawowe, 16–231 sześcian, 232–255 rampa szarości co dziesięć od
// ośmiu. Spoza palety — pustka.
std::optional<Color> Color::indexed(int which) {
    if (which < 0 || which > 255) return std::nullopt;

    if (which < 16) {
        const auto& one = basics[static_cast<std::size_t>(which)];

        return solid(one[0], one[1], one[2]);
    }

    if (which >= 232) {
        const auto grey = static_cast<std::uint8_t>(8 + 10 * (which - 232));

        return solid(grey, grey, grey);
    }

    const int cube = which - 16;

    return solid(steps[static_cast<std::size_t>(cube / 36)], steps[static_cast<std::size_t>((cube / 6) % 6)],
                 steps[static_cast<std::size_t>(cube % 6)]);
}

}
