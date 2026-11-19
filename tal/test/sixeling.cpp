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

#include "tal/pixels.hpp"

#include "tal/testing.hpp"

#include "sixeling.hpp"

// Sixel: the encoder against a reader of what it writes, pixel by pixel.

namespace {

using tal::Bytes;
using tal::media::Pixels;
using tal::media::Still;
using tal::sixeling::sixel;

// A size in pixels as the trials count it.
struct Shape {
    int wide = 0;
    int high = 0;
};

// One RGBA pixel of a test image; opaque unless said otherwise.
struct Rgba {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 255;
};

// A still image of the given size made from pixels listed row by row.
Pixels held(Shape size, const std::vector<Rgba>& all) {
    Bytes made;

    for (const Rgba& one : all) {
        made.push_back(std::byte{one.red});
        made.push_back(std::byte{one.green});
        made.push_back(std::byte{one.blue});
        made.push_back(std::byte{one.alpha});
    }

    return Pixels{{static_cast<std::uint32_t>(size.wide), static_cast<std::uint32_t>(size.high)}, {Still{made, std::chrono::milliseconds{0}}}};
}

// One 0..255 channel value as a percentage, rounded to the nearest, as sixel colors are written.
int percent(std::uint8_t value) { return (value * 100 + 127) / 255; }

// A reader of what the encoder writes, covering as much sixel as the trials need: the header, the
// raster, colors in RGB percent, runs, `!`, `$` and `-`. A pixel with no color is -1.
struct Read {
    bool whole = false;
    int wide = 0;
    int high = 0;
    std::map<int, std::array<int, 3>> palette;
    std::vector<int> picked;

    // The color register of pixel (x, y), or -1.
    [[nodiscard]] int at(int x, int y) const { return picked[static_cast<std::size_t>(y * wide + x)]; }
};

// Decodes `text` into a `Read`; `whole` is set only when the text ends with ST right after the
// data.
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

// The color of a pixel after decoding, in percent; -1 for an empty pixel.
std::array<int, 3> tone(const Read& read, int x, int y) {
    const int which = read.at(x, y);

    if (which < 0) return {-1, -1, -1};

    return read.palette.at(which);
}

// The color the encoder should give pixel `one`, in percent.
std::array<int, 3> wanted(Rgba one) { return {percent(one.red), percent(one.green), percent(one.blue)}; }

}

TRIAL("sixel: an empty image and a frame outside the image give an empty string") {
    SAME(sixel(Pixels{}, 0), std::string{});
    SAME(sixel(held({1, 1}, {Rgba{1, 2, 3}}), 1), std::string{});
}

TRIAL("sixel: header with P2 = 1, a raster of square pixels and a closing ST") {
    const std::string made = sixel(held({2, 1}, {Rgba{255, 0, 0}, Rgba{0, 0, 255}}), 0);

    OUGHT(made.rfind("\x1bP0;1;0q\"1;1;2;1#", 0) == 0);
    OUGHT(made.size() > 2 && made.substr(made.size() - 2) == "\x1b\\");
    OUGHT(decoded(made).whole);
}

TRIAL("sixel: an image with few colors comes back exactly, pixel by pixel") {
    const std::vector<Rgba> tones = {{255, 0, 0}, {0, 128, 0}, {10, 20, 250}, {255, 255, 255}, {0, 0, 0}};
    const Shape size{13, 13};
    std::vector<Rgba> all;

    for (int y = 0; y < size.high; ++y)
        for (int x = 0; x < size.wide; ++x) all.push_back(tones[static_cast<std::size_t>((x * 7 + y * 3) % 5)]);

    const Read read = decoded(sixel(held(size, all), 0));

    OUGHT(read.whole);
    SAME(read.wide, 13);
    SAME(read.high, 12);
    SAME(read.palette.size(), std::size_t{5});

    for (int y = 0; y < 12; ++y)
        for (int x = 0; x < 13; ++x) OUGHT(tone(read, x, y) == wanted(all[static_cast<std::size_t>(y * 13 + x)]));
}

TRIAL("sixel: a nearly transparent pixel is not drawn, from half opacity it is") {
    const std::vector<Rgba> all = {{200, 0, 0, 0}, {200, 0, 0, 127}, {200, 0, 0, 128}, {200, 0, 0, 255}};
    const Read read = decoded(sixel(held({4, 1}, all), 0));

    OUGHT(read.whole);
    SAME(read.at(0, 0), -1);
    SAME(read.at(1, 0), -1);
    OUGHT(read.at(2, 0) >= 0);
    OUGHT(read.at(3, 0) >= 0);
    SAME(read.palette.size(), std::size_t{1});
}

TRIAL("sixel: a fully transparent image is just the header with no colors") {
    const std::string made = sixel(held({3, 2}, std::vector<Rgba>(6, Rgba{9, 9, 9, 0})), 0);
    const Read read = decoded(made);

    OUGHT(read.whole);
    OUGHT(read.palette.empty());
    SAME(made, std::string{"\x1bP0;1;0q\"1;1;3;2\x1b\\"});
}

TRIAL("sixel: the height is cut to full bands, an image lower than a band stays") {
    SAME(decoded(sixel(held({1, 7}, std::vector<Rgba>(7, Rgba{})), 0)).high, 6);
    SAME(decoded(sixel(held({1, 17}, std::vector<Rgba>(17, Rgba{})), 0)).high, 12);
    SAME(decoded(sixel(held({1, 5}, std::vector<Rgba>(5, Rgba{})), 0)).high, 5);
    SAME(decoded(sixel(held({1, 12}, std::vector<Rgba>(12, Rgba{})), 0)).high, 12);
}

TRIAL("sixel: bands go in order with a dash between them, not after the last") {
    std::vector<Rgba> all(12, Rgba{255, 255, 255});

    for (std::size_t at = 6; at < 12; ++at) all[at] = Rgba{0, 0, 0};

    const std::string made = sixel(held({1, 12}, all), 0);
    const Read read = decoded(made);

    OUGHT(read.whole);

    for (int y = 0; y < 12; ++y) OUGHT(tone(read, 0, y) == wanted(all[static_cast<std::size_t>(y)]));

    OUGHT(made.find("-\x1b\\") == std::string::npos);
    SAME(std::count(made.begin(), made.end(), '-'), std::ptrdiff_t{1});

    // One color per band: no carriage return, the first color of a band starts at the left anyway.
    SAME(std::count(made.begin(), made.end(), '$'), std::ptrdiff_t{0});
}

TRIAL("sixel: from four repeats a character goes through an exclamation mark, shorter ones directly") {
    const std::string ten = sixel(held({10, 1}, std::vector<Rgba>(10, Rgba{0, 0, 0})), 0);
    const std::string three = sixel(held({3, 1}, std::vector<Rgba>(3, Rgba{0, 0, 0})), 0);
    const std::string four = sixel(held({4, 1}, std::vector<Rgba>(4, Rgba{0, 0, 0})), 0);

    OUGHT(ten.find("#0!10@") != std::string::npos);
    OUGHT(three.find("#0@@@\x1b") != std::string::npos);
    OUGHT(four.find("#0!4@\x1b") != std::string::npos);
}

TRIAL("sixel: a carriage return separates two colors in a band, empty columns at the end are left out") {
    const std::vector<Rgba> all = {{255, 0, 0}, {0, 0, 255}, {0, 0, 0, 0}, {0, 0, 0, 0}};
    const std::string made = sixel(held({4, 1}, all), 0);
    const Read read = decoded(made);

    OUGHT(read.whole);
    OUGHT(tone(read, 0, 0) == wanted(all[0]));
    OUGHT(tone(read, 1, 0) == wanted(all[1]));
    SAME(read.at(2, 0), -1);
    SAME(read.at(3, 0), -1);

    // The palette is in color order: (0,0,255) before (255,0,0). Blue stands in the second column,
    // red only in the first; no run adds empty columns after itself.
    OUGHT(made.find("#0?@$#1@\x1b") != std::string::npos);
}

TRIAL("sixel: colors in percent, rounded to the nearest") {
    const std::string made = sixel(held({1, 1}, {Rgba{255, 128, 1}}), 0);

    OUGHT(made.find("#0;2;100;50;0") != std::string::npos);
}

TRIAL("sixel: more colors than allowed: the palette fits and pixels go to close colors") {
    const Shape size{40, 30};
    std::vector<Rgba> all;

    for (int y = 0; y < size.high; ++y)
        for (int x = 0; x < size.wide; ++x)
            all.push_back(Rgba{static_cast<std::uint8_t>(x * 6), static_cast<std::uint8_t>(y * 8),
                               static_cast<std::uint8_t>((x * 3 + y * 5) % 256)});

    const Read read = decoded(sixel(held(size, all), 0));

    OUGHT(read.whole);
    OUGHT(read.palette.size() <= 256);
    OUGHT(read.palette.size() > 200);

    int worst = 0;

    for (int y = 0; y < size.high; ++y)
        for (int x = 0; x < size.wide; ++x) {
            const std::array<int, 3> got = tone(read, x, y);
            const std::array<int, 3> want = wanted(all[static_cast<std::size_t>(y * size.wide + x)]);

            OUGHT(got[0] >= 0);

            for (std::size_t band = 0; band < 3; ++band) worst = std::max(worst, std::abs(got[band] - want[band]));
        }

    OUGHT(worst <= 12);
}

TRIAL("sixel: small palette: exactly as many colors as allowed, each of them in use") {
    const Shape size{8, 6};
    std::vector<Rgba> all;

    for (int y = 0; y < size.high; ++y)
        for (int x = 0; x < size.wide; ++x)
            all.push_back(x < 4 ? Rgba{static_cast<std::uint8_t>(250 - x), 0, 0}
                                : Rgba{0, 0, static_cast<std::uint8_t>(250 - x)});

    const Read read = decoded(sixel(held(size, all), 0, 2));

    OUGHT(read.whole);
    SAME(read.palette.size(), std::size_t{2});

    std::set<int> seen;

    for (int y = 0; y < size.high; ++y)
        for (int x = 0; x < size.wide; ++x) {
            seen.insert(read.at(x, y));

            const std::array<int, 3> got = tone(read, x, y);

            OUGHT(x < 4 ? got[0] > 90 && got[2] == 0 : got[2] > 90 && got[0] == 0);
        }

    SAME(seen.size(), std::size_t{2});
}

// The limit of the exact path: exactly as many colors as allowed. These two fall into one 5×5×5
// bucket, so the bucket path would merge them into one.
TRIAL("sixel: as many colors as allowed, all exact") {
    const Read read = decoded(sixel(held({2, 1}, {Rgba{255, 0, 0}, Rgba{249, 0, 0}}), 0, 2));

    OUGHT(read.whole);
    SAME(read.palette.size(), std::size_t{2});
    OUGHT(tone(read, 0, 0) == wanted(Rgba{255, 0, 0}));
    OUGHT(tone(read, 1, 0) == wanted(Rgba{249, 0, 0}));
}

// A pixel goes to the palette color nearest as the eye weighs the channels (2:4:3), not by the
// plain distance: this orange lies nearer to the red by numbers and nearer to the green by sight.
// A color covering more than half the pixels takes a box of its own, and the rest still split.
TRIAL("sixel: the nearest color as the eye sees it; a color of over half the pixels is a box alone") {
    std::vector<Rgba> mixed(100, Rgba{200, 0, 0});

    mixed.insert(mixed.end(), 100, Rgba{0, 200, 0});
    mixed.push_back(Rgba{240, 184, 0});

    const Read seen = decoded(sixel(held({201, 1}, mixed), 0, 2));

    SAME(seen.palette.at(seen.at(200, 0)), (std::array<int, 3>{0, percent(200), 0}));

    std::vector<Rgba> heavy(100, Rgba{0, 0, 0});

    heavy.push_back(Rgba{255, 0, 0});
    heavy.push_back(Rgba{255, 0, 40});

    const Read split = decoded(sixel(held({102, 1}, heavy), 0, 2));

    SAME(split.palette.at(split.at(0, 0)), (std::array<int, 3>{0, 0, 0}));
    SAME(split.palette.at(split.at(100, 0)), (std::array<int, 3>{100, 0, percent(20)}));
}

TRIAL("sixel: zero colors means one, the encoder does not write an image without a palette") {
    const Read read = decoded(sixel(held({2, 1}, {Rgba{255, 0, 0}, Rgba{0, 0, 255}}), 0, 0));

    OUGHT(read.whole);
    SAME(read.palette.size(), std::size_t{1});
    OUGHT(read.at(0, 0) == 0 && read.at(1, 0) == 0);
}

TRIAL("sixel: the second frame of a moving image is its own pixels") {
    Bytes first;
    Bytes second;

    for (const std::uint8_t one : {255, 0, 0, 255}) first.push_back(std::byte{one});
    for (const std::uint8_t one : {0, 255, 0, 255}) second.push_back(std::byte{one});

    const Pixels moving{{1, 1}, {Still{first, std::chrono::milliseconds{50}}, Still{second, std::chrono::milliseconds{50}}}};

    OUGHT(sixel(moving, 0).find("#0;2;100;0;0") != std::string::npos);
    OUGHT(sixel(moving, 1).find("#0;2;0;100;0") != std::string::npos);
}
