#include "proving.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "talcite/sixel.hpp"

using namespace talcite;

namespace {

struct Rgba {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 255;
};

Pixels held(ui::Size size, const std::vector<Rgba>& all) {
    Bytes made;

    for (const Rgba& one : all) {
        made.push_back(std::byte{one.red});
        made.push_back(std::byte{one.green});
        made.push_back(std::byte{one.blue});
        made.push_back(std::byte{one.alpha});
    }

    return Pixels{size, {Still{made, std::chrono::milliseconds{0}}}};
}

int percent(std::uint8_t value) { return (value * 100 + 127) / 255; }

// Odczyt tego, co koder pisze — tyle sixela, ile potrzeba próbom: nagłówek, raster, barwy
// w procentach RGB, przebiegi, `!`, `$` i `-`. Piksel bez barwy to -1.
struct Read {
    bool whole = false;
    int wide = 0;
    int high = 0;
    std::map<int, std::array<int, 3>> palette;
    std::vector<int> picked;

    [[nodiscard]] int at(int x, int y) const { return picked[static_cast<std::size_t>(y * wide + x)]; }
};

Read decoded(const std::string& text) {
    Read made;
    std::size_t at = 0;

    const auto number = [&] {
        int value = 0;

        while (at < text.size() && text[at] >= '0' && text[at] <= '9') value = value * 10 + (text[at++] - '0');

        return value;
    };

    if (text.rfind("\x1bP0;1;0q", 0) != 0) return made;

    at = 8;

    if (at >= text.size() || text[at] != '"') return made;

    ++at;
    number();
    ++at;
    number();
    ++at;
    made.wide = number();
    ++at;
    made.high = number();
    made.picked.assign(static_cast<std::size_t>(made.wide * made.high), -1);

    int colour = 0;
    int x = 0;
    int top = 0;

    while (at < text.size()) {
        const char sign = text[at];

        if (sign == '\x1b') {
            made.whole = text.compare(at, 2, "\x1b\\") == 0 && at + 2 == text.size();

            return made;
        }

        if (sign == '#') {
            ++at;
            colour = number();

            if (at < text.size() && text[at] == ';') {
                ++at;
                number();
                ++at;
                const int red = number();
                ++at;
                const int green = number();
                ++at;
                const int blue = number();

                made.palette[colour] = {red, green, blue};
            }

            continue;
        }

        if (sign == '$') {
            x = 0;
            ++at;

            continue;
        }

        if (sign == '-') {
            x = 0;
            top += 6;
            ++at;

            continue;
        }

        int run = 1;

        if (sign == '!') {
            ++at;
            run = number();
        }

        const int bits = text[at++] - 63;

        for (int step = 0; step < run; ++step, ++x)
            for (int row = 0; row < 6; ++row)
                if ((bits & (1 << row)) != 0 && x < made.wide && top + row < made.high)
                    made.picked[static_cast<std::size_t>((top + row) * made.wide + x)] = colour;
    }

    return made;
}

// Barwa piksela po odczycie, w procentach; -1 dla piksela pustego.
std::array<int, 3> tone(const Read& read, int x, int y) {
    const int which = read.at(x, y);

    if (which < 0) return {-1, -1, -1};

    return read.palette.at(which);
}

std::array<int, 3> wanted(Rgba one) { return {percent(one.red), percent(one.green), percent(one.blue)}; }

}

TRIAL("sixel: obraz pusty i klatka spoza obrazu dają pusty ciąg") {
    same(sixel(Pixels{}, 0), std::string{});
    same(sixel(held({1, 1}, {Rgba{1, 2, 3}}), 1), std::string{});
}

TRIAL("sixel: nagłówek z P2 = 1, raster o kwadratowych pikselach i domknięcie ST") {
    const std::string made = sixel(held({2, 1}, {Rgba{255, 0, 0}, Rgba{0, 0, 255}}), 0);

    ought(made.rfind("\x1bP0;1;0q\"1;1;2;1#", 0) == 0);
    ought(made.size() > 2 && made.substr(made.size() - 2) == "\x1b\\");
    ought(decoded(made).whole);
}

TRIAL("sixel: obraz o niewielu barwach wraca dokładnie, piksel po pikselu") {
    const std::vector<Rgba> tones = {{255, 0, 0}, {0, 128, 0}, {10, 20, 250}, {255, 255, 255}, {0, 0, 0}};
    const ui::Size size{13, 13};
    std::vector<Rgba> all;

    for (int y = 0; y < size.high; ++y)
        for (int x = 0; x < size.wide; ++x) all.push_back(tones[static_cast<std::size_t>((x * 7 + y * 3) % 5)]);

    const Read read = decoded(sixel(held(size, all), 0));

    ought(read.whole);
    same(read.wide, 13);
    same(read.high, 12);
    same(read.palette.size(), std::size_t{5});

    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 13; ++x) ought(tone(read, x, y) == wanted(all[static_cast<std::size_t>(y * 13 + x)]));
}

TRIAL("sixel: piksel prawie przezroczysty nie jest rysowany, od połowy krycia już tak") {
    const std::vector<Rgba> all = {{200, 0, 0, 0}, {200, 0, 0, 127}, {200, 0, 0, 128}, {200, 0, 0, 255}};
    const Read read = decoded(sixel(held({4, 1}, all), 0));

    ought(read.whole);
    same(read.at(0, 0), -1);
    same(read.at(1, 0), -1);
    ought(read.at(2, 0) >= 0);
    ought(read.at(3, 0) >= 0);
    same(read.palette.size(), std::size_t{1});
}

TRIAL("sixel: obraz całkiem przezroczysty to sam nagłówek bez barw") {
    const std::string made = sixel(held({3, 2}, std::vector<Rgba>(6, Rgba{9, 9, 9, 0})), 0);
    const Read read = decoded(made);

    ought(read.whole);
    ought(read.palette.empty());
    same(made, std::string{"\x1bP0;1;0q\"1;1;3;2\x1b\\"});
}

TRIAL("sixel: wysokość tnie się do pełnych pasów, obraz niższy niż pas zostaje") {
    same(decoded(sixel(held({1, 7}, std::vector<Rgba>(7, Rgba{})), 0)).high, 6);
    same(decoded(sixel(held({1, 17}, std::vector<Rgba>(17, Rgba{})), 0)).high, 12);
    same(decoded(sixel(held({1, 5}, std::vector<Rgba>(5, Rgba{})), 0)).high, 5);
    same(decoded(sixel(held({1, 12}, std::vector<Rgba>(12, Rgba{})), 0)).high, 12);
}

TRIAL("sixel: pasy idą po kolei, kreska między nimi, a nie po ostatnim") {
    std::vector<Rgba> all(12, Rgba{255, 255, 255});

    for (std::size_t at = 6; at < 12; ++at) all[at] = Rgba{0, 0, 0};

    const std::string made = sixel(held({1, 12}, all), 0);
    const Read read = decoded(made);

    ought(read.whole);

    for (int y = 0; y < 12; ++y) ought(tone(read, 0, y) == wanted(all[static_cast<std::size_t>(y)]));

    ought(made.find("-\x1b\\") == std::string::npos);
    same(std::count(made.begin(), made.end(), '-'), std::ptrdiff_t{1});

    // Jedna barwa w pasie: żadnego powrotu karetki — pierwsza barwa pasa zaczyna od lewej i tak.
    same(std::count(made.begin(), made.end(), '$'), std::ptrdiff_t{0});
}

TRIAL("sixel: od czterech powtórzeń znak idzie przez wykrzyknik, krótsze wprost") {
    const std::string ten = sixel(held({10, 1}, std::vector<Rgba>(10, Rgba{0, 0, 0})), 0);
    const std::string three = sixel(held({3, 1}, std::vector<Rgba>(3, Rgba{0, 0, 0})), 0);
    const std::string four = sixel(held({4, 1}, std::vector<Rgba>(4, Rgba{0, 0, 0})), 0);

    ought(ten.find("#0!10@") != std::string::npos);
    ought(three.find("#0@@@\x1b") != std::string::npos);
    ought(four.find("#0!4@\x1b") != std::string::npos);
}

TRIAL("sixel: dwie barwy w pasie dzieli powrót karetki, puste kolumny na końcu nie idą") {
    const std::vector<Rgba> all = {{255, 0, 0}, {0, 0, 255}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const std::string made = sixel(held({4, 1}, all), 0);
    const Read read = decoded(made);

    ought(read.whole);
    ought(tone(read, 0, 0) == wanted(all[0]));
    ought(tone(read, 1, 0) == wanted(all[1]));
    same(read.at(2, 0), -1);
    same(read.at(3, 0), -1);

    // Paleta po kolei barw: (0,0,255) przed (255,0,0). Niebieska stoi w drugiej kolumnie,
    // czerwona tylko w pierwszej — żaden przebieg nie dopisuje pustych kolumn za sobą.
    ought(made.find("#0?@$#1@\x1b") != std::string::npos);
}

TRIAL("sixel: barwy w procentach, zaokrąglone do najbliższego") {
    const std::string made = sixel(held({1, 1}, {Rgba{255, 128, 1}}), 0);

    ought(made.find("#0;2;100;50;0") != std::string::npos);
}

TRIAL("sixel: więcej barw niż wolno — paleta się mieści, a piksele idą do bliskich") {
    const ui::Size size{40, 30};
    std::vector<Rgba> all;

    for (int y = 0; y < size.high; ++y)
        for (int x = 0; x < size.wide; ++x)
            all.push_back(Rgba{static_cast<std::uint8_t>(x * 6), static_cast<std::uint8_t>(y * 8),
                               static_cast<std::uint8_t>((x * 3 + y * 5) % 256)});

    const Read read = decoded(sixel(held(size, all), 0));

    ought(read.whole);
    ought(read.palette.size() <= 256);
    ought(read.palette.size() > 200);

    int worst = 0;

    for (int y = 0; y < size.high; ++y)
        for (int x = 0; x < size.wide; ++x) {
            const std::array<int, 3> got = tone(read, x, y);
            const std::array<int, 3> want = wanted(all[static_cast<std::size_t>(y * size.wide + x)]);

            ought(got[0] >= 0);

            for (std::size_t band = 0; band < 3; ++band) worst = std::max(worst, std::abs(got[band] - want[band]));
        }

    ought(worst <= 12);
}

TRIAL("sixel: mała paleta — dokładnie tyle barw, ile wolno, i każda z nich w użyciu") {
    const ui::Size size{8, 6};
    std::vector<Rgba> all;

    for (int y = 0; y < size.high; ++y)
        for (int x = 0; x < size.wide; ++x)
            all.push_back(x < 4 ? Rgba{static_cast<std::uint8_t>(250 - x), 0, 0}
                                : Rgba{0, 0, static_cast<std::uint8_t>(250 - x)});

    const Read read = decoded(sixel(held(size, all), 0, 2));

    ought(read.whole);
    same(read.palette.size(), std::size_t{2});

    std::set<int> seen;

    for (int y = 0; y < size.high; ++y)
        for (int x = 0; x < size.wide; ++x) {
            seen.insert(read.at(x, y));

            const std::array<int, 3> got = tone(read, x, y);

            ought(x < 4 ? got[0] > 90 && got[2] == 0 : got[2] > 90 && got[0] == 0);
        }

    same(seen.size(), std::size_t{2});
}

// Granica drogi dokładnej: barw dokładnie tyle, ile wolno. Te dwie leżą w jednym kubełku 5×5×5,
// więc droga przez kubełki zlałaby je w jedną.
TRIAL("sixel: barw tyle, ile wolno — wszystkie dokładnie") {
    const Read read = decoded(sixel(held({2, 1}, {Rgba{255, 0, 0}, Rgba{249, 0, 0}}), 0, 2));

    ought(read.whole);
    same(read.palette.size(), std::size_t{2});
    ought(tone(read, 0, 0) == wanted(Rgba{255, 0, 0}));
    ought(tone(read, 1, 0) == wanted(Rgba{249, 0, 0}));
}

TRIAL("sixel: zero barw to jedna — koder nie pisze obrazu bez palety") {
    const Read read = decoded(sixel(held({2, 1}, {Rgba{255, 0, 0}, Rgba{0, 0, 255}}), 0, 0));

    ought(read.whole);
    same(read.palette.size(), std::size_t{1});
    ought(read.at(0, 0) == 0 && read.at(1, 0) == 0);
}

TRIAL("sixel: druga klatka obrazu ruchomego to jej własne piksele") {
    Bytes first;
    Bytes second;

    for (const std::uint8_t one : {255, 0, 0, 255}) first.push_back(std::byte{one});
    for (const std::uint8_t one : {0, 255, 0, 255}) second.push_back(std::byte{one});

    const Pixels moving{{1, 1}, {Still{first, std::chrono::milliseconds{50}}, Still{second, std::chrono::milliseconds{50}}}};

    ought(sixel(moving, 0).find("#0;2;100;0;0") != std::string::npos);
    ought(sixel(moving, 1).find("#0;2;0;100;0") != std::string::npos);
}
