#include "talcite/mosaic.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

#include "lighting.hpp"

namespace talcite {

namespace {

constexpr std::size_t bands = 4;

// **Oko waży kanały bardzo różnie** — zielony siedmiokrotnie mocniej niż niebieski. Fork sumuje
// wariancje surowego RGB i przez to potrafi rozciąć kratkę tam, gdzie liczby są duże, a nie tam,
// gdzie różnicę widać. Te trzy współczynniki to cały koszt poprawki.
constexpr double weights[3] = {0.2126, 0.7152, 0.0722};

// Podkratka liczy się za pokrytą, gdy jest bardziej widoczna niż przezroczysta. Ostrzejszego
// progu nie ma po co szukać: kratka niesie dwie barwy i żadnej półprzezroczystości nie wyrazi.
constexpr std::uint8_t solid = 128;

// Rozstrzyga remisy na korzyść jednej barwy. **Nie jest regulatorem** — wariancja chodzi w setkach
// tysięcy, więc pół jednostki znaczy dokładnie tyle, ile ma znaczyć: gdy dwa cięcia są tak samo
// dobre, wygrywa to bez drugiej barwy.
constexpr double lonely = 0.5;

enum class Family : std::uint8_t { quarters, rows, columns };

struct Shape {
    char32_t glyph;
    Family family;
    std::uint8_t keys;
};

// Znaki blokowe z Unicode 1.1 — ma je każdy font terminalowy. Ćwiartki opisują wszystkie szesnaście
// wzorów 2x2; ósemki tną kratkę poziomo i pionowo w ośmiu miejscach.
//
// **Tablica jest niesymetryczna i taka być musi**: ósemek „górnych" i „prawych" poza połówkami
// w Block Elements po prostu nie ma — mieszkają dopiero w `U+1FB00`, czyli tam, gdzie już są
// sekstanty. Wzory dopełniające i tak wychodzą, bo dopełnienie ósemki dolnej jest ósemką dolną
// o innej wysokości, a atrament z papierem zamieniają się rolami same.
constexpr std::array<Shape, 28> shapes{{
    {0x2598, Family::quarters, 0b0001},
    {0x259D, Family::quarters, 0b0010},
    {0x2580, Family::quarters, 0b0011},
    {0x2596, Family::quarters, 0b0100},
    {0x258C, Family::quarters, 0b0101},
    {0x259E, Family::quarters, 0b0110},
    {0x259B, Family::quarters, 0b0111},
    {0x2597, Family::quarters, 0b1000},
    {0x259A, Family::quarters, 0b1001},
    {0x2590, Family::quarters, 0b1010},
    {0x259C, Family::quarters, 0b1011},
    {0x2584, Family::quarters, 0b1100},
    {0x2599, Family::quarters, 0b1101},
    {0x259F, Family::quarters, 0b1110},
    {0x2588, Family::quarters, 0b1111},

    {0x2594, Family::rows, 0b00000001},
    {0x2581, Family::rows, 0b10000000},
    {0x2582, Family::rows, 0b11000000},
    {0x2583, Family::rows, 0b11100000},
    {0x2585, Family::rows, 0b11111000},
    {0x2586, Family::rows, 0b11111100},
    {0x2587, Family::rows, 0b11111110},

    {0x258F, Family::columns, 0b00000001},
    {0x258E, Family::columns, 0b00000011},
    {0x258D, Family::columns, 0b00000111},
    {0x258B, Family::columns, 0b00011111},
    {0x258A, Family::columns, 0b00111111},
    {0x2589, Family::columns, 0b01111111},
}};

// Sekstant z sześciu bitów, liczonych od lewej-górnej. Cztery wzory mieszkają poza swoim blokiem:
// pustka i pełnia to spacja i `█`, a same kolumny to `▌` i `▐` — dlatego `U+1FB00` liczy sześćdziesiąt
// znaków, a nie sześćdziesiąt cztery, i dlatego trzeba te trzy dziury po drodze przeskoczyć.
[[nodiscard]] char32_t sextant(unsigned bits) {
    if (bits == 0) return U' ';
    if (bits == 63) return U'█';
    if (bits == 21) return U'▌';
    if (bits == 42) return U'▐';

    unsigned at = bits - 1;

    if (bits > 21) --at;
    if (bits > 42) --at;

    return static_cast<char32_t>(0x1FB00 + at);
}

// Kupka statystyk. Trzyma **dwie sumy naraz i to jest rozmyślne**: średnią liczymy w świetle
// liniowym, bo tam mieszka jasność, a wariancję na bajtach sRGB, bo tam mieszka wrażenie. Każda
// z nich jest właściwa do swojej roboty i żadna do obu.
struct Heap {
    double straightly[3] = {};
    double plain[3] = {};
    double squares[3] = {};
    int taken = 0;

    void operator+=(const Heap& other) {
        for (std::size_t band = 0; band < 3; ++band) {
            straightly[band] += other.straightly[band];
            plain[band] += other.plain[band];
            squares[band] += other.squares[band];
        }

        taken += other.taken;
    }

    [[nodiscard]] double cost() const {
        if (taken == 0) return 0.0;

        double made = 0.0;

        for (std::size_t band = 0; band < 3; ++band) {
            const double sum = plain[band];

            made += weights[band] * (squares[band] - sum * sum / taken);
        }

        return made;
    }

    [[nodiscard]] Color tone() const {
        if (taken == 0) return {};

        return Color{false, bent(straightly[0] / taken), bent(straightly[1] / taken),
                     bent(straightly[2] / taken)};
    }
};

[[nodiscard]] Heap gathered(const std::byte* at) {
    Heap made;

    const std::array<double, 256>& tone = straight();

    for (std::size_t band = 0; band < 3; ++band) {
        const std::uint8_t held = sample(at[band]);
        const double one = held;

        made.straightly[band] = tone[held];
        made.plain[band] = one;
        made.squares[band] = one * one;
    }

    made.taken = 1;

    return made;
}

[[nodiscard]] Style dressed(Color foreground, Color background) {
    Style made{};

    made.foreground = foreground;
    made.background = background;

    return made;
}

// Kratka w pełni pokryta: szukamy cięcia, po którym obie połówki są najbardziej jednolite. To jest
// najlepszy pomysł forka i zostaje — kratka niesie dwie barwy, więc pytanie brzmi dokładnie tak.
struct Split {
    char32_t glyph = U' ';
    Heap kept;
    Heap rest;
};

[[nodiscard]] Split blocked(const std::array<Heap, 64>& parts, const Heap& whole) {
    // Cztery ćwiartki, osiem wierszy i osiem kolumn — zbierane **raz na kratkę**, nie raz na
    // kształt. Bez tego dwadzieścia osiem kształtów przechodziłoby po sześćdziesięciu czterech
    // podkratkach każdy.
    std::array<Heap, 4> quarters;
    std::array<Heap, 8> rows;
    std::array<Heap, 8> columns;

    for (std::size_t at = 0; at < parts.size(); ++at) {
        const std::size_t down = at / 8;
        const std::size_t across = at % 8;

        rows[down] += parts[at];
        columns[across] += parts[at];
        quarters[(down >= 4 ? 2 : 0) + (across >= 4 ? 1 : 0)] += parts[at];
    }

    Split made;

    double least = std::numeric_limits<double>::max();

    for (const Shape& shape : shapes) {
        Heap front;

        const std::size_t much = shape.family == Family::quarters ? 4 : 8;

        for (std::size_t key = 0; key < much; ++key) {
            if ((shape.keys & (1u << key)) == 0) continue;

            front += shape.family == Family::quarters ? quarters[key]
                     : shape.family == Family::rows   ? rows[key]
                                                      : columns[key];
        }

        if (front.taken == 0) continue;

        Heap other = whole;

        other.taken -= front.taken;

        for (std::size_t band = 0; band < 3; ++band) {
            other.straightly[band] -= front.straightly[band];
            other.plain[band] -= front.plain[band];
            other.squares[band] -= front.squares[band];
        }

        const double here = front.cost() + other.cost() + (other.taken > 0 ? lonely : 0.0);

        if (here >= least) continue;

        least = here;
        made.glyph = shape.glyph;
        made.kept = front;
        made.rest = other;
    }

    return made;
}

// Sekstanty stać na **przegląd wyczerpujący** — sześćdziesiąt trzy wzory po sześć podkratek to
// trzysta osiemdziesiąt działań na kratkę. Wyczerpujący znaczy optymalny, więc żadna heurystyka
// nie ma tu czego poprawiać; sprytniejszy dobór kandydatów przez sortowanie po jasności gubiłby
// krawędź czerwone-zielone o równej jasności, a nic by nie oszczędził.
[[nodiscard]] Split sexted(const std::array<Heap, 64>& parts, const Heap& whole) {
    Split made;

    double least = std::numeric_limits<double>::max();

    for (unsigned bits = 1; bits < 64; ++bits) {
        Heap front;

        for (std::size_t at = 0; at < 6; ++at)
            if ((bits & (1u << at)) != 0) front += parts[at];

        if (front.taken == 0) continue;

        Heap other = whole;

        other.taken -= front.taken;

        for (std::size_t band = 0; band < 3; ++band) {
            other.straightly[band] -= front.straightly[band];
            other.plain[band] -= front.plain[band];
            other.squares[band] -= front.squares[band];
        }

        const double here = front.cost() + other.cost() + (other.taken > 0 ? lonely : 0.0);

        if (here >= least) continue;

        least = here;
        made.glyph = sextant(bits);
        made.kept = front;
        made.rest = other;
    }

    return made;
}

// Kratka pokryta częściowo: kształt idzie **za sylwetką, nie za barwą**. Wycięty brzeg ma wyjść
// tam, gdzie naprawdę jest, a barwa i tak jest jedna, bo widocznych podkratek jest garstka.
[[nodiscard]] char32_t shaped(const std::array<bool, 64>& covered, int filled, ui::Blocks kind) {
    if (kind >= ui::Blocks::sextants) {
        unsigned bits = 0;

        for (std::size_t at = 0; at < 6; ++at)
            if (covered[at]) bits |= 1u << at;

        return sextant(bits);
    }

    std::array<int, 4> quarters{};
    std::array<int, 8> rows{};
    std::array<int, 8> columns{};

    for (std::size_t at = 0; at < covered.size(); ++at) {
        if (!covered[at]) continue;

        const std::size_t down = at / 8;
        const std::size_t across = at % 8;

        ++rows[down];
        ++columns[across];
        ++quarters[(down >= 4 ? 2 : 0) + (across >= 4 ? 1 : 0)];
    }

    char32_t made = U'█';
    int wrongest = std::numeric_limits<int>::max();

    for (const Shape& shape : shapes) {
        const std::size_t much = shape.family == Family::quarters ? 4 : 8;
        const int weight = shape.family == Family::quarters ? 16 : 8;

        int shared = 0;
        int held = 0;

        for (std::size_t key = 0; key < much; ++key) {
            if ((shape.keys & (1u << key)) == 0) continue;

            shared += shape.family == Family::quarters ? quarters[key]
                      : shape.family == Family::rows   ? rows[key]
                                                       : columns[key];
            held += weight;
        }

        // Różnica symetryczna: ile podkratek kształt zajmuje nadmiarowo plus ile pokrytych pomija.
        const int wrong = held + filled - 2 * shared;

        if (wrong >= wrongest) continue;

        wrongest = wrong;
        made = shape.glyph;
    }

    return made;
}

// Znaki ASCII od najrzadszego do najgęstszego — ile z ośmiu podkratek pokrywa tusz.
constexpr std::string_view ramp = ".:-=+*%#";

// Tusz w czarno-białym: terminal nie ma barw, więc zostaje sama sylwetka. Obraz z przezroczystością
// (logo, ikona) ma ją w kanale alfa — tuszem jest to, co widać. Obraz pełny (zdjęcie) sylwetki nie
// ma, więc tuszem jest to, co jaśniejsze od średniej jasności obrazu.
struct Ink {
    bool clear = false;
    double middle = 0.0;
};

[[nodiscard]] double lightness(const std::byte* at) {
    const std::array<double, 256>& tone = straight();

    return weights[0] * tone[sample(at[0])] + weights[1] * tone[sample(at[1])] + weights[2] * tone[sample(at[2])];
}

[[nodiscard]] Ink inked(const Bytes& bytes) {
    Ink made;

    double sum = 0.0;
    std::size_t much = 0;

    for (std::size_t at = 0; at + bands <= bytes.size(); at += bands) {
        if (sample(bytes[at + 3]) < 255) made.clear = true;

        sum += lightness(bytes.data() + at);
        ++much;
    }

    made.middle = much == 0 ? 0.0 : sum / static_cast<double>(much);

    return made;
}

}

// Ile podkratek przypada na kratkę. Znaki blokowe potrzebują ośmiu na osiem, bo tyle miejsc tnie
// ósemka; sekstanty dwóch na trzy; bez Unicode dwóch na cztery — tyle, ile liczy drabina znaków ASCII.
//
// **Oktanty ścinamy do sekstantów i to jest świadome.** Znaków jest dwieście trzydzieści, a nie
// dwieście pięćdziesiąt sześć — dwadzieścia sześć wzorów mieszka w Block Elements i w `U+1FB00` —
// więc odwzorowanie jest tablicą, a nie wzorem. Tablicy nie wolno zgadywać: pomyłka to śmieci na
// ekranie, a nie gorszy obraz. Wejdą osobno, gdy dojdziemy do właściwego wykazu.
ui::Size grain(ui::Blocks kind, bool unicode) {
    if (!unicode) return {2, 4};

    return kind >= ui::Blocks::sextants ? ui::Size{2, 3} : ui::Size{8, 8};
}

// Obrazek musi być **już przeskalowany** do siatki podkratek — liczbę kratek wyprowadzamy z jego
// rozmiaru, więc nie da się podać jednego, a mieć na myśli drugie.
//
// Przezroczystość wpada tu wprost w model biblioteki: kratka bez ani jednej pokrytej podkratki
// wychodzi jako `bare()`, czyli **widać przez nią tło rodzica**. Kratka pokryta częściowo dostaje
// papier od wołającego, gdy ten go poda (tło roli obrazu w skórce); bez niego idzie **samym pierwszym
// planem** (`View::foreground`) — tło weźmie przy wklejaniu z tego, co leży pod spodem. Dawniej zostawało
// przezroczyste aż do terminala, który malował tam swoje tło: czarne kratki wokół logo w oknie.
//
// **Najlepiej, jak się da.** W pełnych kolorach i w palecie kratka niesie dwie barwy, a do palety
// terminala sprowadza je dopiero wypisanie. W czarno-białym barw nie ma, więc kratka dwubarwna
// straciłaby tło i zrobiłaby dziurę — rysujemy samą sylwetkę tuszu (`Ink`). Bez Unicode nie ma znaków
// blokowych: kratka to znak z drabiny ASCII według pokrycia, a w kolorze kratka pełna to tło w barwie
// obrazu.
void drawn(View& into, ui::Spot at, const Pixels& one, std::size_t still, ui::Blocks kind,
           const Style& under, Colors colors, bool unicode) {
    if (!one.held() || still >= one.stills().size()) return;

    const ui::Size step = grain(kind, unicode);
    const ui::Size size = one.size();

    const int columns = size.wide / step.wide;
    const int rows = size.high / step.high;

    if (columns <= 0 || rows <= 0) return;

    const Bytes& bytes = one.stills()[still].bytes;

    const std::size_t much = static_cast<std::size_t>(step.wide) *
                             static_cast<std::size_t>(step.high);

    const bool mono = colors == Colors::mono;
    const Ink ink = mono ? inked(bytes) : Ink{};

    for (int row = 0; row < rows; ++row) {
        for (int column = 0; column < columns; ++column) {
            std::array<Heap, 64> parts;
            std::array<bool, 64> covered{};

            Heap whole;
            int filled = 0;

            for (int down = 0; down < step.high; ++down) {
                for (int across = 0; across < step.wide; ++across) {
                    const std::size_t where =
                        (static_cast<std::size_t>(row * step.high + down) *
                             static_cast<std::size_t>(size.wide) +
                         static_cast<std::size_t>(column * step.wide + across)) *
                        bands;

                    const std::size_t which =
                        static_cast<std::size_t>(down) * static_cast<std::size_t>(step.wide) +
                        static_cast<std::size_t>(across);

                    if (sample(bytes[where + 3]) < solid) continue;

                    if (mono && !ink.clear && lightness(bytes.data() + where) < ink.middle) continue;

                    parts[which] = gathered(bytes.data() + where);
                    covered[which] = true;
                    whole += parts[which];

                    ++filled;
                }
            }

            const ui::Spot spot{at.x + column, at.y + row};

            if (filled == 0) {
                into.clear({spot, {1, 1}});

                continue;
            }

            if (!unicode) {
                const bool full = static_cast<std::size_t>(filled) == much;

                if (full && !mono)
                    into.foreground(spot, U' ', dressed(under.foreground, whole.tone()));
                else
                    into.foreground(spot, static_cast<char32_t>(ramp[static_cast<std::size_t>(filled) - 1]),
                             dressed(whole.tone(), under.background));

                continue;
            }

            if (mono || static_cast<std::size_t>(filled) < much) {
                into.foreground(spot, shaped(covered, filled, kind),
                            dressed(whole.tone(), under.background));

                continue;
            }

            const Split split = kind >= ui::Blocks::sextants ? sexted(parts, whole)
                                                         : blocked(parts, whole);

            into.foreground(spot, split.glyph,
                        dressed(split.kept.tone(),
                                split.rest.taken > 0 ? split.rest.tone() : under.background));
        }
    }
}

}
