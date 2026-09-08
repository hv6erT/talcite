#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "talcite/mosaic.hpp"

using namespace talcite;

namespace {

Bytes lump(const std::vector<std::uint8_t>& bytes) {
    Bytes made;

    made.reserve(bytes.size());

    for (const std::uint8_t one : bytes) made.push_back(std::byte{one});

    return made;
}

Pixels held(ui::Size size, const std::vector<std::uint8_t>& bytes) {
    return Pixels{size, {Still{lump(bytes), std::chrono::milliseconds{0}}}};
}

// Jedna kratka, narysowana i oddana do obejrzenia.
Cell one(const Pixels& what, ui::Blocks kind, const Style& under = {}) {
    View made{{1, 1}};

    drawn(made, {0, 0}, what, 0, kind, under);

    return made.at({0, 0});
}

std::vector<std::uint8_t> flat(ui::Size size, std::uint8_t red, std::uint8_t green,
                               std::uint8_t blue, std::uint8_t alpha = 255) {
    std::vector<std::uint8_t> made(static_cast<std::size_t>(size.wide) *
                                   static_cast<std::size_t>(size.high) * 4);

    for (std::size_t at = 0; at < made.size(); at += 4) {
        made[at] = red;
        made[at + 1] = green;
        made[at + 2] = blue;
        made[at + 3] = alpha;
    }

    return made;
}

}

// --- Ziarno ---------------------------------------------------------------------------------

TRIAL("grain: klocki tną kratkę na osiem na osiem, sekstanty na dwa na trzy") {
    same(grain(ui::Blocks::basic).wide, 8);
    same(grain(ui::Blocks::basic).high, 8);
    same(grain(ui::Blocks::sextants).wide, 2);
    same(grain(ui::Blocks::sextants).high, 3);
}

// Odwzorowanie oktantów jest **tablicą dwustu trzydziestu znaków, a nie wzorem** — dwadzieścia
// sześć wzorów mieszka w Block Elements i w `U+1FB00`. Tablicy nie wolno zgadywać, więc do czasu
// jej dodania ścinamy je do sekstantów.
TRIAL("grain: oktanty ścinają się do sekstantów") {
    same(grain(ui::Blocks::octants).wide, grain(ui::Blocks::sextants).wide);
    same(grain(ui::Blocks::octants).high, grain(ui::Blocks::sextants).high);
}

// --- Jednolitość i przezroczystość ------------------------------------------------------------

TRIAL("Mosaic: jednolita barwa daje pełny klocek") {
    const Cell got = one(held({2, 3}, flat({2, 3}, 30, 160, 90)), ui::Blocks::sextants);

    same(got.letter, U'█');
    same(got.style.ink, (Color{false, 30, 160, 90}));

    // Drugiej barwy nie ma, więc papier zostaje taki, jaki podał wołający — domyślnie przezroczysty.
    ought(got.style.paper.transparent);
}

// **Przezroczystość wpada wprost w model biblioteki.** Kratka bez ani jednej widocznej podkratki
// wychodzi jako nierysowana, czyli widać przez nią tło rodzica — dokładnie tak, jak przez pojemnik.
TRIAL("Mosaic: kratka bez treści jest nierysowana") {
    const Cell got = one(held({2, 3}, flat({2, 3}, 255, 0, 0, 0)), ui::Blocks::sextants);

    same(static_cast<int>(got.shown), static_cast<int>(Shown::none));
}

TRIAL("Mosaic: kratka pokryta częściowo idzie za sylwetką") {
    std::vector<std::uint8_t> bytes = flat({2, 3}, 0, 0, 0, 0);

    for (int y = 0; y < 3; ++y) {
        std::uint8_t* at = bytes.data() + static_cast<std::size_t>(y * 2) * 4;

        at[0] = 200;
        at[3] = 255;
    }

    const Cell got = one(held({2, 3}, bytes), ui::Blocks::sextants);

    same(got.letter, U'▌');
    same(got.style.ink, (Color{false, 200, 0, 0}));
    ought(got.style.paper.transparent);
}

// Papier dla części niepokrytej podaje wołający, bo tylko on wie, co ma pod sobą ze skórki.
TRIAL("Mosaic: podany papier trafia pod sylwetkę") {
    std::vector<std::uint8_t> bytes = flat({2, 3}, 0, 0, 0, 0);

    for (int y = 0; y < 3; ++y) {
        std::uint8_t* at = bytes.data() + static_cast<std::size_t>(y * 2) * 4;

        at[0] = 200;
        at[3] = 255;
    }

    Style under{};

    under.paper = Color{false, 0, 0, 160};

    const Cell got = one(held({2, 3}, bytes), ui::Blocks::sextants, under);

    same(got.style.paper, (Color{false, 0, 0, 160}));
}

// --- Cięcie ---------------------------------------------------------------------------------

TRIAL("Mosaic: poziomy podział daje sekstant górnego wiersza") {
    std::vector<std::uint8_t> bytes = flat({2, 3}, 0, 0, 0);

    for (int x = 0; x < 2; ++x) {
        std::uint8_t* at = bytes.data() + static_cast<std::size_t>(x) * 4;

        at[0] = at[1] = at[2] = 255;
    }

    const Cell got = one(held({2, 3}, bytes), ui::Blocks::sextants);

    same(static_cast<unsigned>(got.letter), 0x1FB02u);
    same(got.style.ink, (Color{false, 255, 255, 255}));
    same(got.style.paper, (Color{false, 0, 0, 0}));
}

// --- Jakość: sekstanty kontra klocki -----------------------------------------------------------
//
// Pas przez **środek** kratki jest tym wzorem, którego znaki blokowe nie wyrażą jednym glifem:
// mają ósemki dolne i jedną górną, ale pasa w środku nie mają. Sekstanty mają go wprost.
// Różnicę widać nie w kształcie, tylko w barwach: sekstanty oddają obie czysto, klocki muszą
// którąś zmieszać.

namespace {

std::vector<std::uint8_t> banded(int wide, int high, int from, int upto) {
    std::vector<std::uint8_t> made(static_cast<std::size_t>(wide) * high * 4);

    for (int y = 0; y < high; ++y)
        for (int x = 0; x < wide; ++x) {
            const bool light = y >= from && y < upto;

            std::uint8_t* at = made.data() + (static_cast<std::size_t>(y) * wide + x) * 4;

            at[0] = at[1] = at[2] = light ? 255 : 0;
            at[3] = 255;
        }

    return made;
}

}

TRIAL("Mosaic: sekstanty oddają pas przez środek obiema barwami czysto") {
    const Cell got = one(held({2, 3}, banded(2, 3, 1, 2)), ui::Blocks::sextants);

    same(static_cast<unsigned>(got.letter), 0x1FB0Bu);
    same(got.style.ink, (Color{false, 255, 255, 255}));
    same(got.style.paper, (Color{false, 0, 0, 0}));
}

TRIAL("Mosaic: klocki tego samego pasa czysto nie oddadzą") {
    const Cell got = one(held({8, 8}, banded(8, 8, 3, 5)), ui::Blocks::basic);

    // Któraś z dwóch barw musi być mieszanką — i jest: papier wychodzi szary.
    const bool pure = (got.style.ink == Color{false, 255, 255, 255} ||
                       got.style.ink == Color{false, 0, 0, 0}) &&
                      (got.style.paper == Color{false, 255, 255, 255} ||
                       got.style.paper == Color{false, 0, 0, 0});

    ought(!pure);
    same(got.style.paper, (Color{false, 170, 170, 170}));
}

// --- Wariancja ważona percepcyjnie -------------------------------------------------------------
//
// Oko waży zielony siedmiokrotnie mocniej niż niebieski. Ta kratka jest tak dobrana, że **oba
// kryteria wybierają co innego**: cięcie kolumnowe ma koszt surowy 86700 wobec 97538 dla
// wierszowego, a po zważeniu 62007 wobec 7042. Fork wybrałby kolumnę i rozciąłby kratkę tam, gdzie
// liczby są duże, a nie tam, gdzie różnicę widać.

TRIAL("Mosaic: cięcie idzie za tym, co widać, a nie za surowymi liczbami") {
    const std::vector<std::uint8_t> bytes{0, 0,   0,   255, 0, 0,   255, 255,
                                          0, 255, 0,   255, 0, 255, 255, 255,
                                          0, 255, 0,   255, 0, 255, 255, 255};

    const Cell got = one(held({2, 3}, bytes), ui::Blocks::sextants);

    // Wiersz górny, nie kolumna lewa: `🬂`, a nie `▌`.
    same(static_cast<unsigned>(got.letter), 0x1FB02u);
    ought(got.letter != U'▌');

    // Średnia liczy się w świetle liniowym: z zera i 255 wychodzi 188.
    same(got.style.ink, (Color{false, 0, 0, 188}));
    same(got.style.paper, (Color{false, 0, 255, 188}));
}

// --- Rozkład na kratki -------------------------------------------------------------------------

TRIAL("Mosaic: każda kratka bierze swój kawałek obrazka") {
    std::vector<std::uint8_t> bytes = flat({4, 3}, 0, 0, 0);

    for (int y = 0; y < 3; ++y)
        for (int x = 2; x < 4; ++x) {
            std::uint8_t* at = bytes.data() +
                               (static_cast<std::size_t>(y) * 4 + static_cast<std::size_t>(x)) * 4;

            at[0] = 255;
        }

    View made{{2, 1}};

    drawn(made, {0, 0}, held({4, 3}, bytes), 0, ui::Blocks::sextants);

    same(made.at({0, 0}).letter, U'█');
    same(made.at({0, 0}).style.ink, (Color{false, 0, 0, 0}));
    same(made.at({1, 0}).letter, U'█');
    same(made.at({1, 0}).style.ink, (Color{false, 255, 0, 0}));
}

TRIAL("Mosaic: rysuje od podanego miejsca") {
    View made{{3, 3}};

    drawn(made, {1, 2}, held({2, 3}, flat({2, 3}, 10, 20, 30)), 0, ui::Blocks::sextants);

    same(made.at({1, 2}).letter, U'█');
    same(made.at({0, 0}).letter, U' ');
    same(static_cast<int>(made.at({0, 0}).shown), static_cast<int>(Shown::own));
}

// --- Odmowy -------------------------------------------------------------------------------------

TRIAL("Mosaic: pusty obrazek nie rysuje nic") {
    View made{{2, 2}};

    drawn(made, {0, 0}, Pixels{}, 0, ui::Blocks::sextants);

    same(static_cast<int>(made.at({0, 0}).shown), static_cast<int>(Shown::own));
    same(made.at({0, 0}).letter, U' ');
}

TRIAL("Mosaic: klatka spoza zakresu nie rysuje nic") {
    View made{{1, 1}};

    drawn(made, {0, 0}, held({2, 3}, flat({2, 3}, 255, 0, 0)), 7, ui::Blocks::sextants);

    same(static_cast<int>(made.at({0, 0}).shown), static_cast<int>(Shown::own));
}

TRIAL("Mosaic: obrazek mniejszy niż kratka nie rysuje nic") {
    View made{{1, 1}};

    drawn(made, {0, 0}, held({1, 1}, flat({1, 1}, 255, 0, 0)), 0, ui::Blocks::sextants);

    same(static_cast<int>(made.at({0, 0}).shown), static_cast<int>(Shown::own));
}
