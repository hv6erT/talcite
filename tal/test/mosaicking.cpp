#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "tal/capabilities.hpp"
#include "tal/pixels.hpp"
#include "tal/style.hpp"
#include "tal/surface.hpp"
#include "tal/text.hpp"

#include "tal/testing.hpp"

#include "mosaicking.hpp"

// The mosaic: pictures drawn in characters, cell by cell, on a surface whose cells lie on a paper
// of their own, so that the trials see what the picture leaves of what was below.

namespace {

using tal::Blocks;
using tal::Cell;
using tal::Color;
using tal::Colors;
using tal::Look;
using tal::Surface;
using tal::media::Pixels;
using tal::media::Still;
using tal::mosaicking::drawn;
using tal::mosaicking::grain;

// The paper of the cells before a picture is drawn on them.
constexpr Color below{5, 6, 7};

// A look of only the paper `color`.
Look papered(Color color) {
    Look made;

    made.paper = color;

    return made;
}

// A look of only the ink `color`.
Look inked(Color color) {
    Look made;

    made.ink = color;

    return made;
}

// A size in pixels as the trials count it.
struct Shape {
    int wide = 0;
    int high = 0;
};

// A single-frame image of `size` made of the RGBA numbers `these`.
Pixels held(Shape size, const std::vector<std::uint8_t>& these) {
    tal::Bytes made;

    for (const std::uint8_t one : these) made.push_back(std::byte{one});

    return Pixels{{static_cast<std::uint32_t>(size.wide), static_cast<std::uint32_t>(size.high)}, {Still{made, std::chrono::milliseconds{0}}}};
}

// RGBA numbers of an image of `size` filled with one color.
std::vector<std::uint8_t> flat(Shape size, std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha = 255) {
    std::vector<std::uint8_t> made(static_cast<std::size_t>(size.wide) * static_cast<std::size_t>(size.high) * 4);

    for (std::size_t at = 0; at < made.size(); at += 4) {
        made[at] = red;
        made[at + 1] = green;
        made[at + 2] = blue;
        made[at + 3] = alpha;
    }

    return made;
}

// The character `one` as a cell holds it.
std::string letter(char32_t one) {
    std::string made;

    tal::text::encoded(made, one);

    return made;
}

// A surface of `size` cells on the paper `below`, nothing of it damaged yet.
Surface laid(tal::Size size) {
    Surface made{size};

    made.canvas({{0, 0}, size}).fill({{0, 0}, size}, papered(below));
    made.forget();

    return made;
}

// One cell as a picture leaves it, and whether the picture touched it at all.
struct Drawn {
    Cell cell;
    bool touched = false;
};

// The picture `what` drawn on one cell lying on `below`.
Drawn one(const Pixels& what, Blocks kind, const Look& under = {}, Colors colors = Colors::rgb, bool unicode = true) {
    Surface made = laid({1, 1});
    tal::Canvas all = made.canvas({{0, 0}, {1, 1}});

    drawn(all, {0, 0}, what, 0, kind, under, colors, unicode);

    return {made.at({0, 0}), !made.damaged().empty()};
}

// RGBA numbers of an image that is white in rows [`from`, `upto`) and black elsewhere.
std::vector<std::uint8_t> banded(int wide, int high, int from, int upto) {
    std::vector<std::uint8_t> made(static_cast<std::size_t>(wide) * static_cast<std::size_t>(high) * 4);

    for (int y = 0; y < high; ++y)
        for (int x = 0; x < wide; ++x) {
            const bool light = y >= from && y < upto;
            std::uint8_t* at = made.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(wide) + static_cast<std::size_t>(x)) * 4;

            at[0] = at[1] = at[2] = light ? 255 : 0;
            at[3] = 255;
        }

    return made;
}

// A 2 by 4 image (one cell without Unicode) whose first `much` subcells, by rows, are covered in
// red and the rest transparent.
Pixels covered(int much) {
    std::vector<std::uint8_t> made = flat({2, 4}, 0, 0, 0, 0);

    for (int at = 0; at < much; ++at) {
        made[static_cast<std::size_t>(at) * 4] = 200;
        made[static_cast<std::size_t>(at) * 4 + 3] = 255;
    }

    return held({2, 4}, made);
}

// A 2 by 3 image whose left column is red and the rest transparent.
Pixels leftish() {
    std::vector<std::uint8_t> made = flat({2, 3}, 0, 0, 0, 0);

    for (int y = 0; y < 3; ++y) {
        std::uint8_t* at = made.data() + static_cast<std::size_t>(y * 2) * 4;

        at[0] = 200;
        at[3] = 255;
    }

    return held({2, 3}, made);
}

}

// --- Grain ---------------------------------------------------------------------------------------

// Blocks cut a cell into eight by eight, sextants into two by three; octants are reduced to
// sextants until their table of two hundred and thirty characters comes, checked rather than
// guessed; without Unicode a cell is two by four, as many as the ladder of ASCII characters counts.
TRIAL("Mosaic: the grain of blocks, sextants, octants and of a terminal without Unicode") {
    SAME(grain(Blocks::basic), (tal::Size{8, 8}));
    SAME(grain(Blocks::sextants), (tal::Size{2, 3}));
    SAME(grain(Blocks::octants), (tal::Size{2, 3}));

    for (const Blocks kind : {Blocks::basic, Blocks::sextants, Blocks::octants}) SAME(grain(kind, false), (tal::Size{2, 4}));
}

// --- Uniformity and transparency -----------------------------------------------------------------

// A uniform color is a full block in it. There is no second color, so the paper stays what lay
// below: a terminal with spacing between rows would show a seam there in its own background.
TRIAL("Mosaic: a uniform color gives a full block on the paper below") {
    const Drawn got = one(held({2, 3}, flat({2, 3}, 30, 160, 90)), Blocks::sextants);

    SAME(got.cell.cluster, std::string{"█"});
    OUGHT(got.cell.look.ink == (Color{30, 160, 90}));
    OUGHT(got.cell.look.paper == below);
}

// A cell without a single visible subcell (one more transparent than not) is left as it lies, so
// the parent's background shows through it, just as through a container; a partly covered one
// follows the silhouette, on the paper below or on the paper the caller gives, which only the
// caller knows from its style.
TRIAL("Mosaic: an empty cell is not drawn, a partly covered one follows the silhouette") {
    const Drawn clear = one(held({2, 3}, flat({2, 3}, 255, 0, 0, 0)), Blocks::sextants);
    const Drawn half = one(leftish(), Blocks::sextants);
    const Drawn given = one(leftish(), Blocks::sextants, papered(Color{0, 0, 160}));

    OUGHT(!clear.touched);
    OUGHT(!one(held({2, 3}, flat({2, 3}, 255, 0, 0, 127)), Blocks::sextants).touched);
    SAME(one(held({2, 3}, flat({2, 3}, 255, 0, 0, 128)), Blocks::sextants).cell.cluster, std::string{"█"});
    SAME(half.cell.cluster, std::string{"▌"});
    OUGHT(half.cell.look.ink == (Color{200, 0, 0}));
    OUGHT(half.cell.look.paper == below);
    OUGHT(given.cell.look.paper == (Color{0, 0, 160}));
}

// --- Cutting -------------------------------------------------------------------------------------

TRIAL("Mosaic: a horizontal split gives the sextant of the top row") {
    const Drawn got = one(held({2, 3}, banded(2, 3, 0, 1)), Blocks::sextants);

    SAME(got.cell.cluster, letter(0x1FB02));
    OUGHT(got.cell.look.ink == (Color{255, 255, 255}));
    OUGHT(got.cell.look.paper == (Color{0, 0, 0}));
}

// A band through the middle of the cell is what block characters cannot draw with one glyph: they
// have lower eighths and one upper eighth, but no band in the middle. Sextants have it, so both
// colors come out clean; blocks must mix one of them, and the paper comes out gray.
TRIAL("Mosaic: sextants draw a band through the middle cleanly, blocks cannot") {
    const Drawn sextant = one(held({2, 3}, banded(2, 3, 1, 2)), Blocks::sextants);
    const Drawn block = one(held({8, 8}, banded(8, 8, 3, 5)), Blocks::basic);
    const auto pure = [](const std::optional<Color>& one) { return one == Color{255, 255, 255} || one == Color{0, 0, 0}; };

    SAME(sextant.cell.cluster, letter(0x1FB0B));
    OUGHT(sextant.cell.look.ink == (Color{255, 255, 255}));
    OUGHT(sextant.cell.look.paper == (Color{0, 0, 0}));
    OUGHT(!(pure(block.cell.look.ink) && pure(block.cell.look.paper)));
    OUGHT(block.cell.look.paper == (Color{170, 170, 170}));
}

// The eye weighs green seven times more than blue. This cell is chosen so that the two criteria
// pick different cuts: the column cut has a raw cost of 86700 against 97538 for the row cut, and
// after weighting 62007 against 7042. Unweighted variance would cut where the numbers are large,
// not where the difference is seen. The colors are averaged in linear light: 0 and 255 give 188.
TRIAL("Mosaic: the cut follows what is seen, not the raw numbers") {
    const Drawn got = one(held({2, 3}, {0, 0, 0, 255, 0, 0, 255, 255, 0, 255, 0, 255, 0, 255, 255, 255, 0, 255, 0, 255, 0, 255, 255, 255}),
                          Blocks::sextants);

    SAME(got.cell.cluster, letter(0x1FB02));
    OUGHT(got.cell.look.ink == (Color{0, 0, 188}));
    OUGHT(got.cell.look.paper == (Color{0, 255, 188}));
}

// --- Cells and places ----------------------------------------------------------------------------

TRIAL("Mosaic: each cell takes its own piece of the image, from the place given") {
    std::vector<std::uint8_t> halves = flat({4, 3}, 0, 0, 0);

    for (int y = 0; y < 3; ++y)
        for (int x = 2; x < 4; ++x) halves[(static_cast<std::size_t>(y) * 4 + static_cast<std::size_t>(x)) * 4] = 255;

    Surface two = laid({2, 1});
    Surface three = laid({3, 3});
    tal::Canvas on_two = two.canvas({{0, 0}, {2, 1}});
    tal::Canvas on_three = three.canvas({{0, 0}, {3, 3}});

    drawn(on_two, {0, 0}, held({4, 3}, halves), 0, Blocks::sextants);
    drawn(on_three, {1, 2}, held({2, 3}, flat({2, 3}, 10, 20, 30)), 0, Blocks::sextants);

    OUGHT(two.at({0, 0}).cluster == "█" && two.at({0, 0}).look.ink == (Color{0, 0, 0}));
    OUGHT(two.at({1, 0}).cluster == "█" && two.at({1, 0}).look.ink == (Color{255, 0, 0}));
    SAME(three.at({1, 2}).cluster, std::string{"█"});
    SAME(three.damaged().size(), std::size_t{1});
    SAME(three.damaged()[0], (tal::Area{{1, 2}, {1, 1}}));
}

// An empty image, a frame it does not have and an image smaller than a cell draw nothing.
TRIAL("Mosaic: nothing to draw: an empty image, a frame out of range, less than a cell") {
    OUGHT(!one(Pixels{}, Blocks::sextants).touched);

    Surface made = laid({1, 1});
    tal::Canvas all = made.canvas({{0, 0}, {1, 1}});

    drawn(all, {0, 0}, held({2, 3}, flat({2, 3}, 255, 0, 0)), 7, Blocks::sextants);
    drawn(all, {0, 0}, held({1, 1}, flat({1, 1}, 255, 0, 0)), 0, Blocks::sextants);
    drawn(all, {0, 0}, held({2, 2}, flat({2, 2}, 255, 0, 0)), 0, Blocks::sextants);

    OUGHT(made.damaged().empty());
}

// --- Color depth and characters ------------------------------------------------------------------

// Without Unicode the character comes from an ASCII ladder by coverage, in the image's color; a
// fully covered cell in color is a background in the image's color, and in monochrome the densest
// character.
TRIAL("Mosaic: without Unicode, a ladder of characters by coverage") {
    const std::string ladder = ".:-=+*%#";

    for (int much = 1; much <= 8; ++much)
        SAME(one(covered(much), Blocks::sextants, {}, Colors::mono, false).cell.cluster, std::string(1, ladder[static_cast<std::size_t>(much) - 1]));

    for (int much = 1; much < 8; ++much) {
        const Drawn got = one(covered(much), Blocks::sextants, {}, Colors::ansi, false);

        SAME(got.cell.cluster, std::string(1, ladder[static_cast<std::size_t>(much) - 1]));
        OUGHT(got.cell.look.ink == (Color{200, 0, 0}));
        OUGHT(got.cell.look.paper == below);
    }

    const Drawn full = one(covered(8), Blocks::sextants, inked(Color{1, 2, 3}), Colors::rgb, false);

    SAME(full.cell.cluster, std::string{" "});
    OUGHT(full.cell.look.paper == (Color{200, 0, 0}));
    OUGHT(full.cell.look.ink == (Color{1, 2, 3}));
    OUGHT(!one(covered(0), Blocks::sextants, {}, Colors::rgb, false).touched);
}

// Monochrome: a cell of two colors would lose its background and make a hole, so an image with
// transparency draws only the silhouette of what is seen, and a full cell is a full block even
// when it has two colors.
TRIAL("Mosaic: monochrome with transparency is the silhouette of what is seen") {
    std::vector<std::uint8_t> bytes = flat({4, 3}, 0, 0, 0, 0);

    for (int y = 0; y < 3; ++y)
        for (int x = 2; x < 4; ++x) {
            std::uint8_t* at = bytes.data() + static_cast<std::size_t>(y * 4 + x) * 4;

            at[0] = y == 0 ? 220 : 0;
            at[2] = y == 0 ? 0 : 220;
            at[3] = 255;
        }

    Surface colored = laid({2, 1});
    Surface mono = laid({2, 1});
    tal::Canvas on_colored = colored.canvas({{0, 0}, {2, 1}});
    tal::Canvas on_mono = mono.canvas({{0, 0}, {2, 1}});

    drawn(on_colored, {0, 0}, held({4, 3}, bytes), 0, Blocks::sextants, {}, Colors::rgb, true);
    drawn(on_mono, {0, 0}, held({4, 3}, bytes), 0, Blocks::sextants, {}, Colors::mono, true);

    OUGHT(colored.at({1, 0}).cluster != "█");
    SAME(mono.at({1, 0}).cluster, std::string{"█"});
    SAME(mono.damaged()[0], (tal::Area{{1, 0}, {1, 1}}));

    std::vector<std::uint8_t> big = flat({8, 8}, 0, 0, 200, 255);

    for (std::size_t at = 0; at < 32 * 4; at += 4) big[at] = 200;

    big[3] = 254;

    SAME(one(held({8, 8}, big), Blocks::basic, {}, Colors::mono, true).cell.cluster, std::string{"█"});
}

// Monochrome without transparency (a photo): the ink is whatever is brighter than the image's
// average, weighed as the eye weighs the channels, so pure red is darker than the average green.
TRIAL("Mosaic: monochrome without transparency takes what is brighter than average") {
    std::vector<std::uint8_t> top = flat({2, 3}, 20, 20, 20);
    std::vector<std::uint8_t> tall = flat({2, 4}, 20, 20, 20);
    std::vector<std::uint8_t> hues = flat({2, 3}, 0, 160, 0);

    for (std::size_t x = 0; x < 2; ++x) {
        top[x * 4] = top[x * 4 + 1] = top[x * 4 + 2] = 240;
        tall[x * 4] = tall[x * 4 + 1] = tall[x * 4 + 2] = 240;
        hues[x * 4] = 255;
        hues[x * 4 + 1] = 0;
    }

    SAME(one(held({2, 3}, top), Blocks::sextants, {}, Colors::mono, true).cell.cluster, letter(0x1FB02));
    SAME(one(held({2, 4}, tall), Blocks::sextants, {}, Colors::mono, false).cell.cluster, std::string{":"});
    SAME(one(held({2, 3}, hues), Blocks::sextants, {}, Colors::mono, true).cell.cluster, letter(0x1FB39));
    SAME(one(held({2, 3}, flat({2, 3}, 90, 90, 90)), Blocks::sextants, {}, Colors::mono, true).cell.cluster, std::string{"█"});
}

// Palettes (256 and 16 colors) draw like full color; colors are reduced to the terminal's palette
// only when written out.
TRIAL("Mosaic: palettes draw like full color") {
    std::vector<std::uint8_t> bytes = flat({2, 3}, 0, 0, 200);

    for (std::size_t x = 0; x < 2; ++x) {
        bytes[x * 4] = 200;
        bytes[x * 4 + 2] = 0;
    }

    const Drawn full = one(held({2, 3}, bytes), Blocks::sextants, {}, Colors::rgb, true);

    for (const Colors colors : {Colors::palette, Colors::ansi}) SAME(one(held({2, 3}, bytes), Blocks::sextants, {}, colors, true).cell, full.cell);
}
