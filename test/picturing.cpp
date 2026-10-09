#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/picture.hpp"
#include "talcite/pixels.hpp"

#include "faking.hpp"

using namespace talcite;

namespace {

// Rozpoznanie z nagłówka zostaje w talcite — to osiem bajtów, nie dekoder — a dekoder mieszka
// w talcite-adapters razem ze swoimi próbami (`test/gifting.cpp` tam). Tu szew obrazu sprawdza
// się na atrapie, która GIF-a nie czyta, tylko oddaje klatki z nagłówka.
[[nodiscard]] Bytes lump(std::string_view hex) {
    Bytes made;

    made.reserve(hex.size() / 2);

    const auto value = [](char one) -> int {
        if (one >= '0' && one <= '9') return one - '0';

        return one - 'a' + 10;
    };

    for (std::size_t at = 0; at + 1 < hex.size(); at += 2)
        made.push_back(std::byte{static_cast<std::uint8_t>(value(hex[at]) * 16 +
                                                            value(hex[at + 1]))});

    return made;
}

constexpr std::string_view leaving =
    "47494638396110000c00810000000000ff000000000000000021ff0b4e45545343415045322e300301000000"
    "21f90404040000002c0000000010000c000008220003080c00a0a0c18305071244c85021c386031f4a9c48b1"
    "a2c58b18336adc2831200021f90405000002002c00000000070006008100000000ff00000000000000081900"
    "01080420a0e0408206050658186020c30005053c8cf830200021f90405010002002c03000200070006008100"
    "00000000ff00000000000008190001080420a0e0408206050658186020c30005053c8cf830200021f904050c"
    "0002002c060004000700060081000000ff000000000000000008190001080420a0e0408206050658186020c3"
    "0005053c8cf83020003b";

}

// --- Rozpoznanie ---------------------------------------------------------------------------

TRIAL("guessed: GIF poznaje po obu wiekach podpisu") {
    same(static_cast<int>(format_of(lump(leaving))), static_cast<int>(Format::gif));
    same(static_cast<int>(format_of(lump("474946383761040004000000003b"))), static_cast<int>(Format::gif));
}

TRIAL("guessed: PNG, JPEG i BMP poznaje po nagłówku, bez dekodera") {
    same(static_cast<int>(format_of(lump("89504e470d0a1a0a0000000d49484452"))),
         static_cast<int>(Format::png));
    same(static_cast<int>(format_of(lump("ffd8ffe000104a464946"))),
         static_cast<int>(Format::jpeg));
    same(static_cast<int>(format_of(lump("424d3a0000000000000036000000280000000100000001000000"))),
         static_cast<int>(Format::bmp));
}

// Wymiary z nagłówka, przed dekoderem: PNG z IHDR, GIF z ekranu logicznego, BMP z nagłówka mapy
// (wysokość ujemna to „od góry", stary nagłówek OS/2 na dwóch bajtach), JPEG z SOF za blokami
// APP — po znacznikach, z ich długością. Ucięty nagłówek albo SOF poza podanymi bajtami to pustka,
// nie zgadywanie.
TRIAL("shaped: wymiary z nagłówka PNG, GIF, BMP i JPEG, pustka gdy ich nie ma") {
    const std::optional<ui::Size> png = size_of(lump("89504e470d0a1a0a0000000d49484452000002800000016008060000"));

    ought(png.has_value());
    same(png->wide, 640);
    same(png->high, 352);

    const std::optional<ui::Size> gif = size_of(lump(leaving));

    ought(gif.has_value());
    same(gif->wide, 16);
    same(gif->high, 12);

    const std::optional<ui::Size> bmp =
        size_of(lump("424d3a0000000000000036000000280000000300000005000000"));

    ought(bmp.has_value());
    same(bmp->wide, 3);
    same(bmp->high, 5);

    const std::optional<ui::Size> upside =
        size_of(lump("424d3a00000000000000360000002800000003000000fbffffff"));

    ought(upside.has_value());
    same(upside->wide, 3);
    same(upside->high, 5);

    const std::optional<ui::Size> old =
        size_of(lump("424d3a00000000000000360000000c0000000700090001000100"));

    ought(old.has_value());
    same(old->wide, 7);
    same(old->high, 9);

    // JPEG: SOI, APP1 (EXIF, długość 16), potem SOF0: długość, głębia, wysokość 16, szerokość 32.
    const std::optional<ui::Size> jpeg = size_of(lump("ffd8"
                                                     "ffe100104578696600000000000000000000"
                                                     "ffc0001108001000200301220002110103110100"));

    ought(jpeg.has_value());
    same(jpeg->wide, 32);
    same(jpeg->high, 16);

    ought(!size_of(lump("ffd8ffe100104578696600000000000000000000")).has_value());
    ought(!size_of(lump("ffd8ffe1001045786966")).has_value());
    ought(!size_of(lump("89504e470d0a1a0a0000000d49484452")).has_value());
    ought(!size_of(lump("47494638396110")).has_value());
    ought(!size_of(lump("424d3a0000000000000036000000280000")).has_value());
    ought(!size_of(lump("89504e470d0a1a0a0000000d494844520000000000000160")).has_value());
    ought(!size_of(lump("hello")).has_value());
}

TRIAL("guessed: samo BM bez reszty nagłówka to nie obrazek") {
    same(static_cast<int>(format_of(lump("424d"))), static_cast<int>(Format::none));
    same(static_cast<int>(format_of(lump("424d3a000000"))), static_cast<int>(Format::none));
}

TRIAL("guessed: śmieci i pustka to nie obrazek") {
    same(static_cast<int>(format_of(Bytes{})), static_cast<int>(Format::none));
    same(static_cast<int>(format_of(lump("47494638"))), static_cast<int>(Format::none));
    same(static_cast<int>(format_of(lump("474946383861"))), static_cast<int>(Format::none));
}

// --- Szew obrazu ---------------------------------------------------------------------------------

TRIAL("Obraz: pusty szew odmawia Unbound z nazwą rodzaju") {
    const Seam<ImageProvider> none;

    ought(!none.bound());

    bool amiss = false;

    try {
        (void)none.decoded(lump(leaving));
    } catch (const Unbound& why) {
        amiss = std::string{why.what()} == "No provider for Image.";
    }

    ought(amiss);

    amiss = false;

    try {
        none.decoded(lump(leaving), [](const Bytes&, ui::Size, std::chrono::milliseconds) { return true; });
    } catch (const Unbound&) {
        amiss = true;
    }

    ought(amiss);
}

// Zdanie dostawcy wychodzi jako `Refused` z nazwą rodzaju z przodu — w obu postaciach.
TRIAL("Obraz: odmowa dostawcy wychodzi jako Refused") {
    const Seam<ImageProvider> images = faking::images();

    same(std::string{images.provider().name()}, std::string{"fake images"});

    bool amiss = false;

    try {
        (void)images.decoded(lump("ffd8ffe000104a464946000102000000"));
    } catch (const Refused& why) {
        amiss = std::string{why.what()} == "Image provider refused: not a GIF the fake would read";
    }

    ought(amiss);
}

TRIAL("Obraz: zebranie strumienia daje wszystkie klatki z rozmiarem i opóźnieniami") {
    const Seam<ImageProvider> images = faking::images();

    const Pixels one = images.decoded(lump(leaving));

    ought(one.held());
    same(one.size().wide, 16);
    same(one.size().high, 12);
    same(one.stills().size(), std::size_t{4});
    same(one.stills()[0].after.count(), 40LL);
    same(one.stills()[3].after.count(), 120LL);
    same(one.stills()[0].bytes.size(), std::size_t{16 * 12 * 4});
}

TRIAL("Obraz: prośba o mniej klatek zatrzymuje rozwijanie") {
    const Seam<ImageProvider> images = faking::images();

    same(images.decoded(lump(leaving), 1).stills().size(), std::size_t{1});
    same(images.decoded(lump(leaving), 3).stills().size(), std::size_t{3});
    same(images.decoded(lump(leaving), 9).stills().size(), std::size_t{4});
}

// **Postać strumieniowa podaje klatkę i zapomina ją.** Fałsz od wołającego znaczy „dosyć"
// i nie jest błędem — dzięki temu podgląd, który chce tylko pierwszej, nie płaci za resztę.
TRIAL("Obraz: strumień daje się przerwać, a bez czynności nic nie robi") {
    const Seam<ImageProvider> images = faking::images();

    std::size_t seen = 0;

    images.decoded(lump(leaving), [&](const Bytes& canvas, ui::Size size, std::chrono::milliseconds) {
        ++seen;

        same(size.wide, 16);
        same(canvas.size(), std::size_t{16 * 12 * 4});

        return seen < 2;
    });

    same(seen, std::size_t{2});

    images.decoded(lump(leaving), Taking{});

    ought(true);
}

// Sufit pilnowany jest przez szew, dla każdego dostawcy jednakowo: klatka ponad `ceiling` nie
// dociera do ujścia i przerywa strumień. Atrapa deklaruje nagłówkowy rozmiar 65535 na 65535 —
// siedemnaście terabajtów RGBA, gdyby ktoś to przydzielił — z pustym buforem; ujście wołającego
// nie ma prawa tego zobaczyć.
TRIAL("Obraz: klatka ponad sufit nie dociera do wołającego") {
    const Seam<ImageProvider> images = faking::images();

    std::size_t seen = 0;

    images.decoded(lump("474946383961ffffffff000000" "21f9040400000000" "3b"),
                   [&](const Bytes&, ui::Size, std::chrono::milliseconds) {
                       ++seen;

                       return true;
                   });

    same(seen, std::size_t{0});
    ought(!images.decoded(lump("474946383961ffffffff000000" "21f9040400000000" "3b")).held());
}

// --- Obraz jako źródło ----------------------------------------------------------------------------

// `figured` domyka bajty i szew w źródło: rodzaj i kształt od ręki, z nagłówka, a piksele na
// zamówienie w rozmiarze, o który pyta wołający — już zmniejszone, nigdy w rozmiarze pliku.
TRIAL("Obraz: figured daje rodzaj i kształt od ręki, a piksele w zadanym rozmiarze") {
    const Figure one = figured(lump(leaving), faking::images());

    same(static_cast<int>(one.format), static_cast<int>(Format::gif));
    same(one.size.wide, 16);
    same(one.size.high, 12);
    ought(static_cast<bool>(one.fetch));

    const Pixels made = one.fetch({8, 6}).await();

    ought(made.held());
    same(made.size().wide, 8);
    same(made.size().high, 6);
    same(made.stills().size(), std::size_t{4});
    same(made.stills()[3].after.count(), 120LL);
    same(made.stills()[0].bytes.size(), std::size_t{8 * 6 * 4});
}

// Odmowa dostawcy nie ginie w źródle — obietnica odrzuca się tym samym `Refused`, które rzuca
// szew; kto chce zdania odmowy, ma je. Rodzaj zostaje z nagłówka także wtedy.
TRIAL("Obraz: figured przy odmowie dostawcy odrzuca obietnicę") {
    const Figure one = figured(lump("ffd8ffe000104a464946000102000000"), faking::images());

    same(static_cast<int>(one.format), static_cast<int>(Format::jpeg));

    bool amiss = false;

    try {
        (void)one.fetch({8, 6}).await();
    } catch (const Refused& why) {
        amiss = std::string{why.what()} == "Image provider refused: not a GIF the fake would read";
    }

    ought(amiss);
}

// Bez dostawcy nie ma czego obiecywać: kształt pusty, żeby układ nie prosił o miejsce, a źródło
// odrzuca `Unbound` — nie udaje, że coś ma.
TRIAL("Obraz: figured bez dostawcy nie ma kształtu i odrzuca Unbound") {
    const Figure one = figured(lump(leaving), Seam<ImageProvider>{});

    same(static_cast<int>(one.format), static_cast<int>(Format::gif));
    same(one.size.wide, 0);
    same(one.size.high, 0);

    bool amiss = false;

    try {
        (void)one.fetch({8, 6}).await();
    } catch (const Unbound&) {
        amiss = true;
    }

    ought(amiss);
}
