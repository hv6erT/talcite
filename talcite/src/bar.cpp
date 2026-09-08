#include "talcite/bar.hpp"

#include <algorithm>
#include <limits>
#include <string>

#include "talcite/skin.hpp"

namespace talcite::ui {

// Pasek jest jedną kratką szeroki: strzałka na początku, strzałka na końcu, a między nimi tor
// z suwakiem. Suwak mówi dwie rzeczy naraz — gdzie jesteśmy **i** jaka część całości mieści się
// w oknie — bo jego długość jest ułamkiem toru w tej samej proporcji.
namespace {

// **Pasek liczy proporcję, nie sztuki.** Wolno mu więc podzielić wszystkie trzy liczby wspólnym
// dzielnikiem, byle całość zmieściła się w `int` — a to jest jedyny sposób, żeby dokument
// o dwóch miliardach wierszy albo bajtów w ogóle dał się narysować.
struct Scaled {
    int shift = 0;
    int whole = 0;
    int window = 0;
};

Scaled scaled(std::size_t shift, std::size_t whole, std::size_t window) {
    constexpr auto brim = static_cast<std::size_t>(std::numeric_limits<int>::max());

    std::size_t by = 1;

    while (whole / by > brim) by *= 2;

    return {static_cast<int>(shift / by), static_cast<int>(whole / by),
            static_cast<int>(window / by)};
}

}

// Pasek przy krawędzi: pionowy z prawej, poziomy u dołu. Samo liczenie miejsca było dotąd
// przepisane w każdym przewijaku z osobna, a to dokładnie ten rodzaj arytmetyki, który w kolejnej
// kopii wychodzi o jedną kratkę obok.
void edged(View& into, Size given, Area seen, bool upright, int much, std::size_t shift,
           std::size_t whole, std::size_t window, const Skin& how, const std::string& role) {
    const Scaled small = scaled(shift, whole, window);

    const Spot at = upright ? Spot{given.wide - 1 - seen.at.x, -seen.at.y}
                            : Spot{-seen.at.x, given.high - 1 - seen.at.y};

    barred(into, at, much, upright, small.shift, small.whole, small.window, how, role);
}

void barred(View& into, Spot at, int much, bool upright, int shift, int whole, int window,
            const Skin& how, const std::string& role) {
    if (much <= 0) return;

    // Pasek pyta skórkę sam, bo jest wolną funkcją i nie ma węzła, przez który mógłby do niej
    // dojść. Wołający podaje ją z `Node::skin()` — jedno wskazanie zamiast dwóch stylów w wykazie
    // parametrów, a przy okazji miejsce, z którego kiedyś przyjdą i znaki.
    const Style look = how.get<Style>(role);
    const Style full = how.get<Style>(role + ".thumb");

    const char32_t empty = how.get<char32_t>(role + ".track");
    const char32_t block = how.get<char32_t>(role + ".thumb");

    into.letter(at, how.get<char32_t>(role + (upright ? ".up" : ".left")), look);

    if (much == 1) return;

    const Spot last = upright ? Spot{at.x, at.y + much - 1} : Spot{at.x + much - 1, at.y};

    into.letter(last, how.get<char32_t>(role + (upright ? ".down" : ".right")), look);

    const int track = much - 2;

    if (track <= 0) return;

    const int most = std::max(whole - window, 0);

    // Gdy treść mieści się w oknie, suwak zajmuje cały tor. Inaczej trzeba by rysować pasek,
    // który niczego nie pokazuje, a mimo to zajmuje miejsce.
    const int thumb =
        most == 0 ? track : std::clamp(track * window / std::max(whole, 1), 1, track);

    const int room = track - thumb;
    const int from = most == 0 ? 0 : (room * shift + most / 2) / most;

    for (int which = 0; which < track; ++which) {
        const bool kept = which >= from && which < from + thumb;
        const Spot spot =
            upright ? Spot{at.x, at.y + 1 + which} : Spot{at.x + 1 + which, at.y};

        into.letter(spot, kept ? block : empty, kept ? full : look);
    }
}

}
