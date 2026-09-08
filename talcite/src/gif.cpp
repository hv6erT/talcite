#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "lighting.hpp"
#include "picturing.hpp"

namespace talcite {

namespace {

constexpr std::size_t bands = 4;

constexpr std::uint8_t image = 0x2C;
constexpr std::uint8_t extra = 0x21;
constexpr std::uint8_t ending = 0x3B;

constexpr std::uint8_t steering = 0xF9;

// Norma mówi „tak szybko, jak się da", ale w praktyce znaczy to sto klatek na sekundę i stroboskop.
// Przeglądarki od dwudziestu lat czytają zero i jedną setną jako dziesięć setnych i pod to jest
// robiona połowa GIF-ów w sieci — **naginamy się do świata, nie do litery**.
constexpr int slowest = 1;
constexpr int instead = 10;

// Przeplot: cztery przebiegi po wierszach, każdy z własnym początkiem i krokiem. Dane w pliku idą
// po kolei, a wiersze obrazka nie — i o to potknięcie jest tu najłatwiej.
constexpr int starts[4] = {0, 4, 2, 1};
constexpr int steps[4] = {8, 8, 4, 2};

// Kursor po wejściu z **jednym znacznikiem zepsucia, który raz zapalony już nie gaśnie**. Każdy
// odczyt za końcem zapala go i oddaje zero, więc pętla sprawdza jeden bit raz na blok zamiast
// dwudziestu sprawdzeń długości rozsianych po kodzie. Właśnie takie zapomniane sprawdzenie jest
// tym, co w dekoderach zabija.
struct Reader {
    std::span<const std::byte> what;
    std::size_t at = 0;
    bool spoiled = false;

    std::uint8_t byte() {
        if (at >= what.size()) {
            spoiled = true;

            return 0;
        }

        return sample(what[at++]);
    }

    unsigned word() {
        const unsigned low = byte();

        return low | (static_cast<unsigned>(byte()) << 8);
    }

    std::span<const std::byte> take(std::size_t much) {
        if (much > what.size() - at) {
            spoiled = true;
            at = what.size();

            return {};
        }

        const std::span<const std::byte> made = what.subspan(at, much);

        at += much;

        return made;
    }
};

struct Table {
    std::array<std::uint8_t, 256 * 3> tones{};
    int much = 0;
};

[[nodiscard]] Table tabled(Reader& from, int much) {
    Table made;

    const std::span<const std::byte> got = from.take(static_cast<std::size_t>(much) * 3);

    if (from.spoiled) return made;

    made.much = much;

    for (std::size_t at = 0; at < got.size(); ++at) made.tones[at] = sample(got[at]);

    return made;
}

// To, co niesie Graphic Control Extension — i co dotyczy **następnego** obrazka, nie tego, po
// którym stoi. Po każdym obrazku wraca do wartości domyślnych, bo rozszerzenie działa raz.
struct Control {
    std::chrono::milliseconds after{instead * 10};
    int clear = -1;
    int disposal = 0;
};

// Dane obrazka leżą w podblokach: bajt długości, tyle bajtów, i tak aż do zera. Sklejamy je
// najpierw w jeden ciągły bufor, a dopiero potem czytamy bity. Czytnik bitów świadomy podbloków
// byłby o jedną kopię tańszy i o klasę trudniejszy do sprawdzenia, a skompresowane dane GIF-a
// są małe.
[[nodiscard]] Bytes glued(Reader& from) {
    Bytes made;

    while (true) {
        const std::uint8_t much = from.byte();

        if (from.spoiled || much == 0) break;

        // Ostatni podblok bywa urwany razem z plikiem. Bierzemy tyle, ile go naprawdę jest —
        // rozpakowywanie i tak policzy, ile podkratek z tego wyszło.
        const std::size_t left = from.what.size() - from.at;
        const std::span<const std::byte> got = from.take(std::min<std::size_t>(much, left));

        made.insert(made.end(), got.begin(), got.end());

        if (much > left) {
            from.spoiled = true;

            break;
        }
    }

    return made;
}

// LZW z GIF-a. **Tablice o stałym rozmiarze, ani jednej alokacji na kod.** Rozwinięcie idzie po
// łańcuchu prefiksów na stos i zdejmuje się odwrotnie; łańcuch nie może być nieskończony, bo każdy
// wpis wskazuje wstecz na **numer mniejszy od własnego** — więc pętla kończy się z konstrukcji,
// a nie z pilnowania. Wyjście jest z góry ograniczone rozmiarem klatki: gdy się zapełni, resztę
// kodów porzucamy zamiast pisać obok.
//
// Oddaje **liczbę zapisanych podkratek**, bo przy pliku urwanym w połowie ta liczba jest całą
// odpowiedzią: tyle obrazka jest prawdziwe, a reszta ma zostać pusta, a nie wypełniona barwą zero.
[[nodiscard]] std::size_t unpacked(std::span<const std::byte> from, int least,
                                   std::span<std::uint8_t> into) {
    if (least < 2 || least > 8 || into.empty()) return 0;

    const unsigned clear = 1u << least;
    const unsigned stop = clear + 1;
    const unsigned none = 4096;

    std::array<std::uint16_t, 4096> prefix{};
    std::array<std::uint8_t, 4096> suffix{};
    std::array<std::uint8_t, 4096> stack{};

    unsigned free = clear + 2;
    unsigned width = static_cast<unsigned>(least) + 1;
    unsigned last = none;
    std::uint8_t first = 0;

    std::size_t made = 0;
    std::size_t at = 0;
    unsigned held = 0;
    unsigned bits = 0;

    while (true) {
        while (bits < width) {
            // Urwany strumień to nie to samo co zepsuty: oddajemy tyle klatki, ile wyszło, bo
            // ucięty GIF ma pokazać swój początek, a nie nic.
            if (at >= from.size()) return made;

            held |= static_cast<unsigned>(sample(from[at++])) << bits;
            bits += 8;
        }

        const unsigned code = held & ((1u << width) - 1);

        held >>= width;
        bits -= width;

        if (code == clear) {
            free = clear + 2;
            width = static_cast<unsigned>(least) + 1;
            last = none;

            continue;
        }

        if (code == stop) break;

        if (code > free) return made;

        std::size_t deep = 0;
        unsigned walk = code;

        if (code == free) {
            if (last == none) return made;

            stack[deep++] = first;
            walk = last;
        }

        while (walk >= clear) {
            if (deep >= stack.size()) return made;

            stack[deep++] = suffix[walk];
            walk = prefix[walk];
        }

        first = static_cast<std::uint8_t>(walk);
        stack[deep++] = first;

        while (deep > 0) {
            if (made >= into.size()) return made;

            into[made++] = stack[--deep];
        }

        if (last != none && free < 4096) {
            prefix[free] = static_cast<std::uint16_t>(last);
            suffix[free] = first;
            ++free;

            if (free == (1u << width) && width < 12) ++width;
        }

        last = code;
    }

    return made;
}

[[nodiscard]] std::size_t spot(ui::Size screen, int x, int y) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(screen.wide) +
            static_cast<std::size_t>(x)) *
           bands;
}

void wiped(Bytes& canvas, ui::Size screen, ui::Area where) {
    for (int y = where.at.y; y < where.at.y + where.size.high; ++y) {
        if (y < 0 || y >= screen.high) continue;

        for (int x = where.at.x; x < where.at.x + where.size.wide; ++x) {
            if (x < 0 || x >= screen.wide) continue;

            const std::size_t at = spot(screen, x, y);

            for (std::size_t band = 0; band < bands; ++band) canvas[at + band] = std::byte{0};
        }
    }
}

[[nodiscard]] Bytes copied(const Bytes& canvas, ui::Size screen, ui::Area where) {
    Bytes made;

    made.reserve(static_cast<std::size_t>(where.size.wide) *
                 static_cast<std::size_t>(where.size.high) * bands);

    for (int y = where.at.y; y < where.at.y + where.size.high; ++y)
        for (int x = where.at.x; x < where.at.x + where.size.wide; ++x) {
            if (y < 0 || y >= screen.high || x < 0 || x >= screen.wide) {
                made.insert(made.end(), bands, std::byte{0});

                continue;
            }

            const std::size_t at = spot(screen, x, y);

            made.insert(made.end(), canvas.begin() + static_cast<std::ptrdiff_t>(at),
                        canvas.begin() + static_cast<std::ptrdiff_t>(at + bands));
        }

    return made;
}

void restored(Bytes& canvas, ui::Size screen, ui::Area where, const Bytes& kept) {
    std::size_t from = 0;

    for (int y = where.at.y; y < where.at.y + where.size.high; ++y)
        for (int x = where.at.x; x < where.at.x + where.size.wide; ++x, from += bands) {
            if (y < 0 || y >= screen.high || x < 0 || x >= screen.wide) continue;

            if (from + bands > kept.size()) return;

            const std::size_t at = spot(screen, x, y);

            for (std::size_t band = 0; band < bands; ++band)
                canvas[at + band] = kept[from + band];
        }
}

// Nałożenie rozpakowanej klatki na płótno. Trzy rzeczy naraz: przeplot, przezroczysty indeks
// i przycinanie do płótna — bo prostokąt zapowiedziany w pliku nie musi się w nim mieścić.
void laid(Bytes& canvas, ui::Size screen, const std::vector<std::uint8_t>& marks,
          std::size_t filled, ui::Area where, const Table& palette, int clear, bool woven) {
    int row = 0;

    for (int pass = 0; pass < (woven ? 4 : 1); ++pass) {
        const int from = woven ? starts[pass] : 0;
        const int step = woven ? steps[pass] : 1;

        for (int y = from; y < where.size.high; y += step, ++row) {
            const int onto = where.at.y + y;

            if (onto < 0 || onto >= screen.high) continue;

            for (int x = 0; x < where.size.wide; ++x) {
                const int across = where.at.x + x;

                if (across < 0 || across >= screen.wide) continue;

                const std::size_t which = static_cast<std::size_t>(row) *
                                              static_cast<std::size_t>(where.size.wide) +
                                          static_cast<std::size_t>(x);

                if (which >= filled) continue;

                const int mark = marks[which];

                if (mark == clear || mark >= palette.much) continue;

                const std::size_t at = spot(screen, across, onto);
                const std::size_t tone = static_cast<std::size_t>(mark) * 3;

                canvas[at + 0] = stored(palette.tones[tone + 0]);
                canvas[at + 1] = stored(palette.tones[tone + 1]);
                canvas[at + 2] = stored(palette.tones[tone + 2]);
                canvas[at + 3] = std::byte{255};
            }
        }
    }
}

[[nodiscard]] bool marked(std::span<const std::byte> what) {
    if (what.size() < 6) return false;

    const char* named = "GIF8";

    for (std::size_t at = 0; at < 4; ++at)
        if (sample(what[at]) != static_cast<std::uint8_t>(named[at])) return false;

    const std::uint8_t age = sample(what[4]);

    return (age == '7' || age == '9') && sample(what[5]) == 'a';
}

}

bool gifted(std::span<const std::byte> what, const Taking& each) {
    if (!marked(what)) return false;

    Reader from{what};

    from.take(6);

    const unsigned wide = from.word();
    const unsigned high = from.word();
    const std::uint8_t packed = from.byte();

    from.byte();
    from.byte();

    if (from.spoiled || wide == 0 || high == 0) return false;

    const ui::Size screen{static_cast<int>(wide), static_cast<int>(high)};

    if (static_cast<std::size_t>(wide) * static_cast<std::size_t>(high) > ceiling) return false;

    Table palette;

    if ((packed & 0x80) != 0) palette = tabled(from, 2 << (packed & 7));

    if (from.spoiled) return false;

    // Płótno powstaje **raz** i żyje do końca; klatki nakładają się na nie po kolei i każda wychodzi
    // do wołającego jako ten sam bufor. Kto chce ją zatrzymać, kopiuje sam — kto chce ją tylko
    // zmniejszyć i porzucić, nie płaci za nic.
    Bytes canvas(static_cast<std::size_t>(wide) * static_cast<std::size_t>(high) * bands,
                 std::byte{0});

    Bytes kept;
    ui::Area saved{};
    ui::Area last{};
    Control control;
    int disposal = 0;
    bool any = false;

    while (!from.spoiled) {
        const std::uint8_t lead = from.byte();

        if (from.spoiled || lead == ending) break;

        if (lead == extra) {
            const std::uint8_t mark = from.byte();
            const Bytes said = glued(from);

            if (mark == steering && said.size() >= 4) {
                const std::uint8_t how = sample(said[0]);
                const int hundredths =
                    sample(said[1]) | (static_cast<int>(sample(said[2])) << 8);

                control.disposal = (how >> 2) & 7;
                control.clear = (how & 1) != 0 ? static_cast<int>(sample(said[3])) : -1;
                control.after = std::chrono::milliseconds{
                    (hundredths <= slowest ? instead : hundredths) * 10};
            }

            continue;
        }

        // Nierozpoznany bajt znaczy, że zgubiliśmy się w pliku. Dalsze zgadywanie tylko wypluje
        // śmieci — oddajemy to, co już mamy.
        if (lead != image) break;

        const unsigned left = from.word();
        const unsigned top = from.word();
        const unsigned across = from.word();
        const unsigned down = from.word();
        const std::uint8_t flags = from.byte();

        if (from.spoiled) break;

        Table local = palette;

        if ((flags & 0x80) != 0) local = tabled(from, 2 << (flags & 7));

        const bool woven = (flags & 0x40) != 0;
        const std::uint8_t least = from.byte();
        const Bytes codes = glued(from);

        // **Plik urwany to nie plik pusty.** Menedżer plików ogląda też pliki w trakcie kopiowania
        // i uszkodzone — a tam górna połowa obrazka jest odpowiedzią lepszą niż nic. Bierzemy więc
        // to, co przyszło, pokazujemy raz i kończymy.
        const bool cut = from.spoiled;

        const std::size_t much =
            static_cast<std::size_t>(across) * static_cast<std::size_t>(down);

        if (much == 0 || much > ceiling) break;

        const ui::Area where{{static_cast<int>(left), static_cast<int>(top)},
                             {static_cast<int>(across), static_cast<int>(down)}};

        // Sprzątanie po **poprzedniej** klatce, a nie po tej: norma każe posprzątać dopiero po
        // pokazaniu, więc robimy to w chwili, gdy następna ma stanąć na płótnie.
        if (disposal == 2) wiped(canvas, screen, last);
        else if (disposal == 3 && !kept.empty()) restored(canvas, screen, saved, kept);

        if (control.disposal == 3) {
            // Odkładamy **sam prostokąt**, nie całe płótno: tylko on się zmieni.
            kept = copied(canvas, screen, where);
            saved = where;
        } else {
            kept.clear();
        }

        std::vector<std::uint8_t> marks(much, 0);

        const std::size_t filled = unpacked(codes, least, marks);

        if (filled == 0) break;

        laid(canvas, screen, marks, filled, where, local, control.clear, woven);

        any = true;

        if (!each(canvas, screen, control.after)) return true;

        if (cut) break;

        disposal = control.disposal;
        last = where;
        control = Control{};
    }

    return any;
}

}
