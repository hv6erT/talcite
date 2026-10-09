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
    same(got.style.foreground, (Color{false, 30, 160, 90}));

    // Drugiej barwy nie ma, więc papier zostaje taki, jaki podał wołający — domyślnie przezroczysty,
    // i wtedy kratka idzie samym pierwszym planem: tło spod spodu także pod pełnym klockiem, bo terminal z odstępem
    // między wierszami pokazałby tam szew w barwie swojego tła.
    ought(got.style.background.transparent);
    same(static_cast<int>(got.shown), static_cast<int>(Shown::foreground));
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
    same(got.style.foreground, (Color{false, 200, 0, 0}));
    ought(got.style.background.transparent);

    // Bez papieru kratka idzie samym pierwszym planem: tło weźmie przy wklejaniu z tego, co pod spodem.
    same(static_cast<int>(got.shown), static_cast<int>(Shown::foreground));
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

    under.background = Color{false, 0, 0, 160};

    const Cell got = one(held({2, 3}, bytes), ui::Blocks::sextants, under);

    same(got.style.background, (Color{false, 0, 0, 160}));
    same(static_cast<int>(got.shown), static_cast<int>(Shown::own));
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
    same(got.style.foreground, (Color{false, 255, 255, 255}));
    same(got.style.background, (Color{false, 0, 0, 0}));
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
    same(got.style.foreground, (Color{false, 255, 255, 255}));
    same(got.style.background, (Color{false, 0, 0, 0}));
}

TRIAL("Mosaic: klocki tego samego pasa czysto nie oddadzą") {
    const Cell got = one(held({8, 8}, banded(8, 8, 3, 5)), ui::Blocks::basic);

    // Któraś z dwóch barw musi być mieszanką — i jest: papier wychodzi szary.
    const bool pure = (got.style.foreground == Color{false, 255, 255, 255} ||
                       got.style.foreground == Color{false, 0, 0, 0}) &&
                      (got.style.background == Color{false, 255, 255, 255} ||
                       got.style.background == Color{false, 0, 0, 0});

    ought(!pure);
    same(got.style.background, (Color{false, 170, 170, 170}));
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
    same(got.style.foreground, (Color{false, 0, 0, 188}));
    same(got.style.background, (Color{false, 0, 255, 188}));
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
    same(made.at({0, 0}).style.foreground, (Color{false, 0, 0, 0}));
    same(made.at({1, 0}).letter, U'█');
    same(made.at({1, 0}).style.foreground, (Color{false, 255, 0, 0}));
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

// --- Barwność i znaki -------------------------------------------------------------------------

namespace {

// Kratka narysowana przy podanej barwności i znakach.
Cell dressed_as(const Pixels& what, ui::Blocks kind, Colors colors, bool unicode, const Style& under = {}) {
    View made{{1, 1}};

    drawn(made, {0, 0}, what, 0, kind, under, colors, unicode);

    return made.at({0, 0});
}

// Obrazek 2×4 (jedna kratka bez Unicode), w którym pierwszych `much` podkratek, wierszami, jest
// pokryte, a reszta przezroczysta.
Pixels covered(int much, std::uint8_t red = 200) {
    std::vector<std::uint8_t> bytes = flat({2, 4}, 0, 0, 0, 0);

    for (int at = 0; at < much; ++at) {
        bytes[static_cast<std::size_t>(at) * 4] = red;
        bytes[static_cast<std::size_t>(at) * 4 + 3] = 255;
    }

    return held({2, 4}, bytes);
}

}

// Bez Unicode nie ma znaków blokowych: kratka to dwie na cztery podkratki, bez względu na gęstość,
// jaką terminal umiałby z Unicode.
TRIAL("grain: bez Unicode dwie na cztery podkratki") {
    for (const ui::Blocks kind : {ui::Blocks::basic, ui::Blocks::sextants, ui::Blocks::octants}) {
        same(grain(kind, false).wide, 2);
        same(grain(kind, false).high, 4);
    }

    same(grain(ui::Blocks::basic, true).wide, 8);
}

// Bez Unicode znak idzie z drabiny ASCII według pokrycia, w barwie obrazu; kratka pełna w kolorze to
// tło w barwie obrazu, a w czarno-białym — najgęstszy znak.
TRIAL("Mosaic: bez Unicode drabina znaków według pokrycia") {
    const std::u32string ladder = U".:-=+*%#";

    for (int much = 1; much <= 8; ++much) {
        const Cell mono = dressed_as(covered(much), ui::Blocks::sextants, Colors::mono, false);

        same(mono.letter, ladder[static_cast<std::size_t>(much) - 1]);
    }

    for (int much = 1; much < 8; ++much) {
        const Cell got = dressed_as(covered(much), ui::Blocks::sextants, Colors::ansi, false);

        same(got.letter, ladder[static_cast<std::size_t>(much) - 1]);
        same(got.style.foreground, (Color{false, 200, 0, 0}));
        ought(got.style.background.transparent);
        same(static_cast<int>(got.shown), static_cast<int>(Shown::foreground));
    }

    Style under{};

    under.foreground = Color{false, 1, 2, 3};

    const Cell full = dressed_as(covered(8), ui::Blocks::sextants, Colors::rgb, false, under);

    same(full.letter, U' ');
    same(full.style.background, (Color{false, 200, 0, 0}));
    same(full.style.foreground, (Color{false, 1, 2, 3}));

    const Cell none = dressed_as(covered(0), ui::Blocks::sextants, Colors::rgb, false);

    same(static_cast<int>(none.shown), static_cast<int>(Shown::none));
}

// Czarno-białe: kratka dwubarwna w pełnych kolorach to dwie barwy, a w czarno-białym straciłaby tło
// i zrobiła dziurę. Obraz z przezroczystością rysuje więc samą sylwetkę tego, co widać — pełna
// kratka to pełny klocek, choćby miała dwie barwy.
TRIAL("Mosaic: czarno-białe z przezroczystością to sylwetka tego, co widać") {
    // Dwie kratki 2×3: lewa przezroczysta, prawa czerwona u góry, niebieska u dołu.
    std::vector<std::uint8_t> bytes = flat({4, 3}, 0, 0, 0, 0);

    for (int y = 0; y < 3; ++y)
        for (int x = 2; x < 4; ++x) {
            std::uint8_t* at = bytes.data() + static_cast<std::size_t>(y * 4 + x) * 4;

            at[0] = y == 0 ? 220 : 0;
            at[2] = y == 0 ? 0 : 220;
            at[3] = 255;
        }

    View colored{{2, 1}};
    View mono{{2, 1}};

    drawn(colored, {0, 0}, held({4, 3}, bytes), 0, ui::Blocks::sextants, {}, Colors::rgb, true);
    drawn(mono, {0, 0}, held({4, 3}, bytes), 0, ui::Blocks::sextants, {}, Colors::mono, true);

    ought(colored.at({1, 0}).letter != U'█');
    same(mono.at({1, 0}).letter, U'█');
    same(static_cast<int>(mono.at({0, 0}).shown), static_cast<int>(Shown::none));

    // Znaki blokowe tak samo: pełna kratka pełnym klockiem.
    std::vector<std::uint8_t> big = flat({8, 8}, 0, 0, 200, 255);

    for (std::size_t at = 0; at < 32 * 4; at += 4) big[at] = 200;

    big[3] = 254;

    same(dressed_as(held({8, 8}, big), ui::Blocks::basic, Colors::mono, true).letter, U'█');
}

// Czarno-białe bez przezroczystości (zdjęcie): tuszem jest to, co jaśniejsze od średniej jasności
// obrazu — górna jasna połowa daje górną połówkę klocka.
TRIAL("Mosaic: czarno-białe bez przezroczystości bierze jaśniejsze od średniej") {
    std::vector<std::uint8_t> bytes = flat({2, 3}, 20, 20, 20);

    for (int x = 0; x < 2; ++x) {
        std::uint8_t* at = bytes.data() + static_cast<std::size_t>(x) * 4;

        at[0] = at[1] = at[2] = 240;
    }

    same(dressed_as(held({2, 3}, bytes), ui::Blocks::sextants, Colors::mono, true).letter, U'\U0001FB02');

    // Jasna jedna trzecia z trzech wierszy w siatce 2×4 bez Unicode: dwie podkratki z ośmiu.
    std::vector<std::uint8_t> tall = flat({2, 4}, 20, 20, 20);

    for (int x = 0; x < 2; ++x) {
        std::uint8_t* at = tall.data() + static_cast<std::size_t>(x) * 4;

        at[0] = at[1] = at[2] = 240;
    }

    same(dressed_as(held({2, 4}, tall), ui::Blocks::sextants, Colors::mono, false).letter, U':');

    // Jasność waży kanały jak oko: czysta czerwień jest ciemniejsza od średniej zieleni, więc tuszem
    // są dwa dolne wiersze zieleni, a nie górny czerwieni.
    std::vector<std::uint8_t> hues = flat({2, 3}, 0, 160, 0);

    for (int x = 0; x < 2; ++x) {
        std::uint8_t* at = hues.data() + static_cast<std::size_t>(x) * 4;

        at[0] = 255;
        at[1] = 0;
    }

    same(dressed_as(held({2, 3}, hues), ui::Blocks::sextants, Colors::mono, true).letter, U'\U0001FB39');

    // Barwa jednolita jest równa średniej, więc cała jest tuszem.
    same(dressed_as(held({2, 3}, flat({2, 3}, 90, 90, 90)), ui::Blocks::sextants, Colors::mono, true).letter, U'█');
}

// Palety (256 i 16 kolorów) rysują się jak pełne kolory — do palety terminala barwy sprowadza dopiero
// wypisanie.
TRIAL("Mosaic: palety rysują jak pełne kolory") {
    std::vector<std::uint8_t> bytes = flat({2, 3}, 0, 0, 200);

    for (int x = 0; x < 2; ++x) bytes[static_cast<std::size_t>(x) * 4] = 200, bytes[static_cast<std::size_t>(x) * 4 + 2] = 0;

    const Cell full = dressed_as(held({2, 3}, bytes), ui::Blocks::sextants, Colors::rgb, true);

    for (const Colors colors : {Colors::palette, Colors::ansi}) {
        const Cell got = dressed_as(held({2, 3}, bytes), ui::Blocks::sextants, colors, true);

        same(got.letter, full.letter);
        same(got.style.foreground, full.style.foreground);
        same(got.style.background, full.style.background);
    }
}
