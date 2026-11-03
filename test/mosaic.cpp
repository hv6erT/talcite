#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "talcite/mosaic.hpp"

using namespace talcite;

namespace {

// Bytes from plain numbers, so that the trials can write pixels as they read.
Bytes lump(const std::vector<std::uint8_t>& bytes) {
    Bytes made;

    made.reserve(bytes.size());

    for (const std::uint8_t one : bytes) made.push_back(std::byte{one});

    return made;
}

// A single-frame image of the given size made of the given RGBA bytes.
Pixels held(ui::Size size, const std::vector<std::uint8_t>& bytes) {
    return Pixels{size, {Still{lump(bytes), std::chrono::milliseconds{0}}}};
}

// One cell, drawn on a sheet where nothing was drawn yet and handed back for inspection.
Cell one(const Pixels& what, ui::Blocks kind, const Style& under = {}) {
    View made{{1, 1}};

    made.clear({{0, 0}, {1, 1}});

    Canvas onto{made};

    drawn(onto, {0, 0}, what, 0, kind, under);

    return made.at({0, 0});
}

// RGBA bytes of an image of the given size filled with one color.
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

// --- Grain ----------------------------------------------------------------------------------

TRIAL("grain: blocks cut a cell into eight by eight, sextants into two by three") {
    same(grain(ui::Blocks::basic).wide, 8);
    same(grain(ui::Blocks::basic).high, 8);
    same(grain(ui::Blocks::sextants).wide, 2);
    same(grain(ui::Blocks::sextants).high, 3);
}

// The octant mapping is a table of two hundred and thirty characters, not a formula; twenty-six of
// the patterns live in Block Elements and at U+1FB00. The table must not be guessed, so until it is
// added, octants are reduced to sextants.
TRIAL("grain: octants are reduced to sextants") {
    same(grain(ui::Blocks::octants).wide, grain(ui::Blocks::sextants).wide);
    same(grain(ui::Blocks::octants).high, grain(ui::Blocks::sextants).high);
}

// --- Uniformity and transparency ------------------------------------------------------------

TRIAL("Mosaic: a uniform color gives a full block") {
    const Cell got = one(held({2, 3}, flat({2, 3}, 30, 160, 90)), ui::Blocks::sextants);

    same(got.letter, U'█');
    same(got.style.foreground, (Color{false, 30, 160, 90}));

    // There is no second color, so the paper stays as the caller gave it, transparent by default,
    // and then the cell goes as foreground only: the background from below shows even under a full
    // block, because a terminal with spacing between rows would show a seam there in its own
    // background color.
    ought(got.style.background.transparent);
    same(static_cast<int>(got.shown), static_cast<int>(Shown::foreground));
}

// Transparency maps straight onto the library's model. A cell without a single visible sub-cell is
// left as it lies, here not drawn, so the parent's background shows through it, just as through a
// container.
TRIAL("Mosaic: a cell without content is not drawn") {
    const Cell got = one(held({2, 3}, flat({2, 3}, 255, 0, 0, 0)), ui::Blocks::sextants);

    same(static_cast<int>(got.shown), static_cast<int>(Shown::none));
}

TRIAL("Mosaic: a partly covered cell follows the silhouette") {
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

    // Without paper the cell goes as foreground only: when pasted, it takes the background from
    // what lies below.
    same(static_cast<int>(got.shown), static_cast<int>(Shown::foreground));
}

// The paper for the uncovered part is given by the caller, because only the caller knows what lies
// beneath it from the skin.
TRIAL("Mosaic: the given paper goes under the silhouette") {
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

// --- Cutting --------------------------------------------------------------------------------

TRIAL("Mosaic: a horizontal split gives the sextant of the top row") {
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

// --- Quality: sextants versus blocks -----------------------------------------------------------
//
// A band through the middle of the cell is the pattern that block characters cannot express with
// one glyph: they have lower eighths and one upper eighth, but no band in the middle. Sextants have
// it directly. The difference shows not in the shape but in the colors: sextants render both
// cleanly, blocks have to mix one of them.

namespace {

// RGBA bytes of an image that is white in rows [`from`, `upto`) and black elsewhere.
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

TRIAL("Mosaic: sextants render a band through the middle with both colors clean") {
    const Cell got = one(held({2, 3}, banded(2, 3, 1, 2)), ui::Blocks::sextants);

    same(static_cast<unsigned>(got.letter), 0x1FB0Bu);
    same(got.style.foreground, (Color{false, 255, 255, 255}));
    same(got.style.background, (Color{false, 0, 0, 0}));
}

TRIAL("Mosaic: blocks cannot render the same band cleanly") {
    const Cell got = one(held({8, 8}, banded(8, 8, 3, 5)), ui::Blocks::basic);

    // One of the two colors must be a mixture, and it is: the paper comes out gray.
    const bool pure = (got.style.foreground == Color{false, 255, 255, 255} ||
                       got.style.foreground == Color{false, 0, 0, 0}) &&
                      (got.style.background == Color{false, 255, 255, 255} ||
                       got.style.background == Color{false, 0, 0, 0});

    ought(!pure);
    same(got.style.background, (Color{false, 170, 170, 170}));
}

// --- Perceptually weighted variance -------------------------------------------------------------
//
// The eye weighs green seven times more than blue. This cell is chosen so that the two criteria
// pick different cuts: the column cut has a raw cost of 86700 against 97538 for the row cut, and
// after weighting 62007 against 7042. Unweighted variance would pick the column and cut the cell
// where the numbers are large, not where the difference is visible.

TRIAL("Mosaic: the cut follows what is visible, not the raw numbers") {
    const std::vector<std::uint8_t> bytes{0, 0,   0,   255, 0, 0,   255, 255,
                                          0, 255, 0,   255, 0, 255, 255, 255,
                                          0, 255, 0,   255, 0, 255, 255, 255};

    const Cell got = one(held({2, 3}, bytes), ui::Blocks::sextants);

    // The top row, not the left column: 🬂, not ▌.
    same(static_cast<unsigned>(got.letter), 0x1FB02u);
    ought(got.letter != U'▌');

    // The average is computed in linear light: zero and 255 give 188.
    same(got.style.foreground, (Color{false, 0, 0, 188}));
    same(got.style.background, (Color{false, 0, 255, 188}));
}

// --- Splitting into cells ----------------------------------------------------------------------

TRIAL("Mosaic: each cell takes its own piece of the image") {
    std::vector<std::uint8_t> bytes = flat({4, 3}, 0, 0, 0);

    for (int y = 0; y < 3; ++y)
        for (int x = 2; x < 4; ++x) {
            std::uint8_t* at = bytes.data() +
                               (static_cast<std::size_t>(y) * 4 + static_cast<std::size_t>(x)) * 4;

            at[0] = 255;
        }

    View made{{2, 1}};

    Canvas onto{made};

    drawn(onto, {0, 0}, held({4, 3}, bytes), 0, ui::Blocks::sextants);

    same(made.at({0, 0}).letter, U'█');
    same(made.at({0, 0}).style.foreground, (Color{false, 0, 0, 0}));
    same(made.at({1, 0}).letter, U'█');
    same(made.at({1, 0}).style.foreground, (Color{false, 255, 0, 0}));
}

TRIAL("Mosaic: draws from the given place") {
    View made{{3, 3}};

    Canvas onto{made};

    drawn(onto, {1, 2}, held({2, 3}, flat({2, 3}, 10, 20, 30)), 0, ui::Blocks::sextants);

    same(made.at({1, 2}).letter, U'█');
    same(made.at({0, 0}).letter, U' ');
    same(static_cast<int>(made.at({0, 0}).shown), static_cast<int>(Shown::own));
}

// --- Refusals -----------------------------------------------------------------------------------

TRIAL("Mosaic: an empty image draws nothing") {
    View made{{2, 2}};

    Canvas onto{made};

    drawn(onto, {0, 0}, Pixels{}, 0, ui::Blocks::sextants);

    same(static_cast<int>(made.at({0, 0}).shown), static_cast<int>(Shown::own));
    same(made.at({0, 0}).letter, U' ');
}

TRIAL("Mosaic: a frame out of range draws nothing") {
    View made{{1, 1}};

    Canvas onto{made};

    drawn(onto, {0, 0}, held({2, 3}, flat({2, 3}, 255, 0, 0)), 7, ui::Blocks::sextants);

    same(static_cast<int>(made.at({0, 0}).shown), static_cast<int>(Shown::own));
}

TRIAL("Mosaic: an image smaller than a cell draws nothing") {
    View made{{1, 1}};

    Canvas onto{made};

    drawn(onto, {0, 0}, held({1, 1}, flat({1, 1}, 255, 0, 0)), 0, ui::Blocks::sextants);

    same(static_cast<int>(made.at({0, 0}).shown), static_cast<int>(Shown::own));
}

// --- Color depth and characters -----------------------------------------------------------------

namespace {

// One cell drawn with the given color depth and character set, on a sheet where nothing was drawn
// yet.
Cell dressed_as(const Pixels& what, ui::Blocks kind, Colors colors, bool unicode, const Style& under = {}) {
    View made{{1, 1}};

    made.clear({{0, 0}, {1, 1}});

    Canvas onto{made};

    drawn(onto, {0, 0}, what, 0, kind, under, colors, unicode);

    return made.at({0, 0});
}

// A 2×4 image (one cell without Unicode) in which the first `much` sub-cells, by rows, are
// covered and the rest is transparent.
Pixels covered(int much, std::uint8_t red = 200) {
    std::vector<std::uint8_t> bytes = flat({2, 4}, 0, 0, 0, 0);

    for (int at = 0; at < much; ++at) {
        bytes[static_cast<std::size_t>(at) * 4] = red;
        bytes[static_cast<std::size_t>(at) * 4 + 3] = 255;
    }

    return held({2, 4}, bytes);
}

}

// Without Unicode there are no block characters: a cell is two by four sub-cells, whatever density
// the terminal could do with Unicode.
TRIAL("grain: without Unicode, two by four sub-cells") {
    for (const ui::Blocks kind : {ui::Blocks::basic, ui::Blocks::sextants, ui::Blocks::octants}) {
        same(grain(kind, false).wide, 2);
        same(grain(kind, false).high, 4);
    }

    same(grain(ui::Blocks::basic, true).wide, 8);
}

// Without Unicode the character comes from an ASCII ladder by coverage, in the image's color; a
// fully covered cell in color is a background in the image's color, and in monochrome the densest
// character.
TRIAL("Mosaic: without Unicode, a ladder of characters by coverage") {
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

// Monochrome: a two-colored cell in full color has two colors, but in monochrome it would lose its
// background and make a hole. So an image with transparency draws only the silhouette of what is
// visible; a full cell is a full block, even if it has two colors.
TRIAL("Mosaic: monochrome with transparency is the silhouette of what is visible") {
    // Two 2×3 cells: the left one transparent, the right one red at the top and blue at the bottom.
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

    colored.clear({{0, 0}, {2, 1}});
    mono.clear({{0, 0}, {2, 1}});

    Canvas on_colored{colored};
    Canvas on_mono{mono};

    drawn(on_colored, {0, 0}, held({4, 3}, bytes), 0, ui::Blocks::sextants, {}, Colors::rgb, true);
    drawn(on_mono, {0, 0}, held({4, 3}, bytes), 0, ui::Blocks::sextants, {}, Colors::mono, true);

    ought(colored.at({1, 0}).letter != U'█');
    same(mono.at({1, 0}).letter, U'█');
    same(static_cast<int>(mono.at({0, 0}).shown), static_cast<int>(Shown::none));

    // Block characters the same way: a full cell is a full block.
    std::vector<std::uint8_t> big = flat({8, 8}, 0, 0, 200, 255);

    for (std::size_t at = 0; at < 32 * 4; at += 4) big[at] = 200;

    big[3] = 254;

    same(dressed_as(held({8, 8}, big), ui::Blocks::basic, Colors::mono, true).letter, U'█');
}

// Monochrome without transparency (a photo): the ink is whatever is brighter than the image's
// average brightness, so a bright upper half gives the upper half block.
TRIAL("Mosaic: monochrome without transparency takes what is brighter than average") {
    std::vector<std::uint8_t> bytes = flat({2, 3}, 20, 20, 20);

    for (int x = 0; x < 2; ++x) {
        std::uint8_t* at = bytes.data() + static_cast<std::size_t>(x) * 4;

        at[0] = at[1] = at[2] = 240;
    }

    same(dressed_as(held({2, 3}, bytes), ui::Blocks::sextants, Colors::mono, true).letter, U'\U0001FB02');

    // A bright third of three rows in a 2×4 grid without Unicode: two sub-cells out of eight.
    std::vector<std::uint8_t> tall = flat({2, 4}, 20, 20, 20);

    for (int x = 0; x < 2; ++x) {
        std::uint8_t* at = tall.data() + static_cast<std::size_t>(x) * 4;

        at[0] = at[1] = at[2] = 240;
    }

    same(dressed_as(held({2, 4}, tall), ui::Blocks::sextants, Colors::mono, false).letter, U':');

    // Brightness weighs the channels as the eye does: pure red is darker than the average green, so
    // the ink is the two lower rows of green, not the upper row of red.
    std::vector<std::uint8_t> hues = flat({2, 3}, 0, 160, 0);

    for (int x = 0; x < 2; ++x) {
        std::uint8_t* at = hues.data() + static_cast<std::size_t>(x) * 4;

        at[0] = 255;
        at[1] = 0;
    }

    same(dressed_as(held({2, 3}, hues), ui::Blocks::sextants, Colors::mono, true).letter, U'\U0001FB39');

    // A uniform color equals the average, so all of it is ink.
    same(dressed_as(held({2, 3}, flat({2, 3}, 90, 90, 90)), ui::Blocks::sextants, Colors::mono, true).letter, U'█');
}

// Palettes (256 and 16 colors) draw like full color; colors are reduced to the terminal's palette
// only when written out.
TRIAL("Mosaic: palettes draw like full color") {
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
