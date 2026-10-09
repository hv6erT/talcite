#include "talcite/picture.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "lighting.hpp"

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

Format format_of(const Bytes& what) {
    if (leads(what, png, 8)) return Format::png;

    if (leads(what, jpeg, 3)) return Format::jpeg;

    // Sam „BM" to dwa bajty i trafiłby w niejeden plik tekstowy, więc żądamy jeszcze **całego
    // nagłówka pliku**: czternaście bajtów, a za nimi nagłówek mapy.
    if (leads(what, bmp, 2) && what.size() >= 14 + 12) return Format::bmp;

    if (leads(what, gif, 4) && what.size() >= 6) {
        const std::uint8_t age = sample(what[4]);

        if ((age == '7' || age == '9') && sample(what[5]) == 'a') return Format::gif;
    }

    return Format::none;
}

namespace {

[[nodiscard]] std::uint32_t big(const Bytes& what, std::size_t at, int much) {
    std::uint32_t made = 0;

    for (int each = 0; each < much; ++each) made = (made << 8) | sample(what[at + static_cast<std::size_t>(each)]);

    return made;
}

[[nodiscard]] std::uint32_t little(const Bytes& what, std::size_t at, int much) {
    std::uint32_t made = 0;

    for (int each = much - 1; each >= 0; --each) made = (made << 8) | sample(what[at + static_cast<std::size_t>(each)]);

    return made;
}

[[nodiscard]] std::optional<ui::Size> sized(std::uint32_t wide, std::uint32_t high) {
    if (wide == 0 || high == 0 || wide > 65535 || high > 65535) return std::nullopt;

    return ui::Size{static_cast<int>(wide), static_cast<int>(high)};
}

}

// **Wymiary z nagłówka, przed dekodowaniem** — po to, żeby ramka na obraz mówiła, ile ma, zanim
// piksele dojdą. PNG trzyma je w IHDR tuż za podpisem, GIF w nagłówku logicznego ekranu, BMP
// w nagłówku mapy (wysokość ze znakiem: ujemna znaczy „od góry"), a JPEG w znaczniku SOF, który
// stoi za blokami APP/COM o własnej długości — więc idziemy po znacznikach, aż trafimy na SOF
// albo skończą się podane bajty. Pustka, gdy w tym, co dano, wymiarów nie ma: wołający czyta
// nagłówek oknem, a EXIF potrafi być dłuższy niż okno.
std::optional<ui::Size> size_of(const Bytes& head) {
    switch (format_of(head)) {
        case Format::png:
            if (head.size() < 24) return std::nullopt;

            return sized(big(head, 16, 4), big(head, 20, 4));

        case Format::gif:
            if (head.size() < 10) return std::nullopt;

            return sized(little(head, 6, 2), little(head, 8, 2));

        case Format::bmp: {
            if (head.size() < 26) return std::nullopt;

            const std::uint32_t told = little(head, 14, 4);

            // Stary nagłówek OS/2 (12 bajtów) ma wymiary na dwóch bajtach, każdy nowszy na czterech.
            if (told == 12) return sized(little(head, 18, 2), little(head, 20, 2));

            const auto high = static_cast<std::int32_t>(little(head, 22, 4));

            return sized(little(head, 18, 4), static_cast<std::uint32_t>(high < 0 ? -high : high));
        }

        case Format::jpeg: {
            std::size_t at = 2;

            while (at + 4 <= head.size()) {
                if (sample(head[at]) != 0xFF) return std::nullopt;

                const std::uint8_t marker = sample(head[at + 1]);

                if (marker == 0xFF) {
                    ++at;
                    continue;
                }

                // SOF0..SOF15 bez DHT (C4), JPG (C8) i DAC (CC): wysokość, potem szerokość.
                if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                    if (at + 9 > head.size()) return std::nullopt;

                    return sized(big(head, at + 7, 2), big(head, at + 5, 2));
                }

                if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
                    at += 2;
                    continue;
                }

                if (marker == 0xD9 || marker == 0xDA) return std::nullopt;

                at += 2 + big(head, at + 2, 2);
            }

            return std::nullopt;
        }

        case Format::none: break;
    }

    return std::nullopt;
}

// Trzeci kształt szwu, obok przekształcenia (`Codec`) i sesji (`TlsProvider`): **dekoder
// strumieniowy**. Obraz tylko się czyta i klatka po klatce — pełna klatka istnieje chwilę i ginie
// po zmniejszeniu — więc `Codec<Pixels>` z całą animacją w pamięci byłby złym kształtem. Uchwyt
// tłumaczy język szwu na język talcite w jednym miejscu, jak `Seam<Codec<…>>`: brak dostawcy to
// `Unbound`, zdanie dostawcy to `Refused`, a fałsz z ujścia znaczy „dosyć" i nie jest błędem.
//
// **Sufit pilnowany jest tu, nie u dostawcy.** Budżet pikseli był dotąd prywatny, bo pilnował go
// tylko nasz GIF; teraz każdy dostawca podlega mu jednakowo: klatka ponad `ceiling` nie dociera
// do ujścia wołającego i przerywa strumień. Dostawca może sprawdzić nagłówek wcześniej i odmówić,
// zanim cokolwiek rozpakuje — to jego oszczędność, nie nasz obowiązek.
ImageProvider& Seam<ImageProvider>::provider() const {
    if (provider_ == nullptr) throw Unbound{ImageProvider::kind()};

    return *provider_;
}

void Seam<ImageProvider>::decoded(const Bytes& given, const Taking& each) const {
    ImageProvider& one = provider();

    if (!each) return;

    const Taking guarded = [&each](const Bytes& canvas, ui::Size size, std::chrono::milliseconds after) {
        const std::size_t much = static_cast<std::size_t>(size.wide) * static_cast<std::size_t>(size.high);

        if (much == 0 || much > ceiling) return false;

        return each(canvas, size, after);
    };

    if (const std::string said = one.decode(given, guarded); !said.empty())
        throw Refused{ImageProvider::kind(), said};
}

// Zebranie strumienia w `Pixels`. **Budżet jest jeden i wydaje się go albo na rozdzielczość, albo
// na czas** — pierwsza klatka mieści się zawsze, bo strumień odmówiłby wcześniej płótna ponad sufit,
// a każda następna musi się jeszcze w nim zmieścić.
Pixels Seam<ImageProvider>::decoded(const Bytes& given, std::size_t most) const {
    std::vector<Still> made;
    std::size_t spent = 0;
    ui::Size size{};

    decoded(given, [&](const Bytes& canvas, ui::Size seen, std::chrono::milliseconds after) {
        const std::size_t much =
            static_cast<std::size_t>(seen.wide) * static_cast<std::size_t>(seen.high);

        if (spent + much > ceiling) return false;

        spent += much;
        size = seen;

        made.push_back(Still{canvas, after});

        return most == 0 || made.size() < most;
    });

    if (made.empty()) return {};

    return Pixels{size, std::move(made)};
}

namespace {

// **Dekodowanie do rozmiaru kratek.** Strumień przez szew: pełna klatka istnieje przez chwilę na
// wątku, który to woła — puli, bo obietnica z `figured` biegnie tam — i ginie zaraz po zmniejszeniu
// do `want`; wynik to `Pixels` w rozmiarze kratek, nigdy w rozmiarze pliku. Przerwany strumień
// (fałsz z ujścia) nie jest wyjątkiem, więc zwykła droga nic tu nie łapie; odmowa i brak dostawcy
// wychodzą wyjątkiem szwu, a obietnica zamienia go w odrzucenie.
[[nodiscard]] Pixels shrunk(const Seam<ImageProvider>& images, const Bytes& raw, ui::Size want) {
    std::vector<Still> these;

    images.decoded(raw, [&](const Bytes& canvas, ui::Size size, std::chrono::milliseconds after) {
        const Pixels one{size, {Still{canvas, after}}};

        these.push_back(one.scaled(want).stills().front());
        these.back().after = after;

        return true;
    });

    if (these.empty()) return {};

    return Pixels{want, std::move(these)};
}

}

// **Obraz jako źródło, jak `Feed` dla tekstu.** Widżet nie zna szwu ani bajtów: dostaje rodzaj
// i kształt — do układu, zanim cokolwiek się zdekoduje — oraz funkcję, którą woła z rozmiarem
// poznanym przy malowaniu. Szew i bajty siedzą w domknięciu; przy zmianie rozmiaru widżet woła
// je znowu i dekoduje od nowa, bo trzymamy plik, nie klatki w pełnej rozdzielczości. Kształt
// bierze się z nagłówka (`size_of`) tylko przy związanym szwie: bez dostawcy obraz nie ma czego
// obiecywać i układ nie ma prosić o miejsce. Rodzaj zostaje zawsze — legenda po nim powie
// „JPEG, bez dostawcy".
Figure figured(Bytes raw, Seam<ImageProvider> images) {
    Figure made;

    made.format = format_of(raw);
    made.size = images.bound() ? size_of(raw).value_or(ui::Size{}) : ui::Size{};
    // Dekodowanie i skalowanie **liczy**, więc biegnie torem `cpu` — z sufitem równym liczbie
    // rdzeni. Na torze zwykłym dziesięć obrazków przewiniętych naraz to dziesięć wątków liczących
    // na kilku rdzeniach, a każdy kończy później, niż gdyby stały w kolejce.
    made.fetch = [raw = std::move(raw), images = std::move(images)](ui::Size want) {
        return Promise<Pixels>{
            [raw, images, want](auto resolve, auto) { resolve(shrunk(images, raw, want)); },
            Lane::cpu};
    };

    return made;
}

}
