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

// The eye weighs the channels very differently: green seven times more than blue. Summing the
// variances of raw RGB can cut a cell where the numbers are large rather than where the difference
// is visible. These three coefficients are the whole cost of doing it right.
constexpr double weights[3] = {0.2126, 0.7152, 0.0722};

// A subcell counts as covered when it is more visible than transparent. There is no point in a
// finer threshold: a cell carries two colors and cannot express any translucency.
constexpr std::uint8_t solid = 128;

// Breaks ties in favor of a single color. It is not a regularizer: the variance runs in the
// hundreds of thousands, so half a unit means exactly what it should: when two cuts are equally
// good, the one without a second color wins.
constexpr double lonely = 0.5;

enum class Family : std::uint8_t { quarters, rows, columns };

struct Shape {
    char32_t glyph;
    Family family;
    std::uint8_t keys;
};

// Block characters from Unicode 1.1, which every terminal font has. The quadrants describe all
// sixteen 2x2 patterns; the eighths cut the cell horizontally and vertically at eight places.
//
// The table is asymmetric and has to be: apart from the halves, there are no "upper" and "right"
// eighths in Block Elements; they live only at U+1FB00, where the sextants already are. The
// complementary patterns come out anyway, because the complement of a lower eighth is a lower
// eighth of another height, and ink and paper swap roles by themselves.
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

// The sextant for six bits, counted from the top left. Four patterns live outside their block:
// empty and full are the space and █, and the bare columns are ▌ and ▐. That is why the block at
// U+1FB00 has sixty characters, not sixty-four, and why the holes on the way have to be skipped.
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

// A heap of statistics. It keeps two sums at once, deliberately: the mean is computed in linear
// light, because that is where brightness lives, and the variance on sRGB bytes, because that is
// where perception lives. Each is right for its own job and neither for both.
struct Heap {
    double straightly[3] = {};
    double plain[3] = {};
    double squares[3] = {};
    int taken = 0;

    // Adds the statistics of another heap to this one.
    void operator+=(const Heap& other) {
        for (std::size_t band = 0; band < 3; ++band) {
            straightly[band] += other.straightly[band];
            plain[band] += other.plain[band];
            squares[band] += other.squares[band];
        }

        taken += other.taken;
    }

    // The perceptually weighted variance of the heap: how far it is from a single color.
    [[nodiscard]] double cost() const {
        if (taken == 0) return 0.0;

        double made = 0.0;

        for (std::size_t band = 0; band < 3; ++band) {
            const double sum = plain[band];

            made += weights[band] * (squares[band] - sum * sum / taken);
        }

        return made;
    }

    // The mean color of the heap, averaged in linear light and bent back to sRGB.
    [[nodiscard]] Color tone() const {
        if (taken == 0) return {};

        return Color{false, bent(straightly[0] / taken), bent(straightly[1] / taken),
                     bent(straightly[2] / taken)};
    }
};

// The heap of a single pixel.
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

// A style with only the given foreground and background.
[[nodiscard]] Style dressed(Color foreground, Color background) {
    Style made{};

    made.foreground = foreground;
    made.background = background;

    return made;
}

// A fully covered cell: we look for the cut after which both parts are most uniform. A cell
// carries two colors, so this is exactly the right question.
struct Split {
    char32_t glyph = U' ';
    Heap kept;
    Heap rest;
};

// The best block-character cut of a fully covered 8x8 cell, with the statistics of both parts.
[[nodiscard]] Split blocked(const std::array<Heap, 64>& parts, const Heap& whole) {
    // Four quadrants, eight rows and eight columns, gathered once per cell, not once per shape.
    // Otherwise each of the twenty-eight shapes would walk over all sixty-four subcells.
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

// The best sextant cut of a fully covered 2x3 cell. Sextants can afford an exhaustive search:
// sixty-three patterns of six subcells are about three hundred eighty operations per cell.
// Exhaustive means optimal, so no heuristic has anything to improve here; a cleverer choice of
// candidates by sorting on brightness would lose a red-green edge of equal brightness and save
// nothing.
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

// The glyph for a partly covered cell: the shape follows the silhouette, not the color. The cut
// edge has to come out where it really is, and there is only one color anyway, because the
// visible subcells are a handful.
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

        // The symmetric difference: how many subcells the shape covers in excess plus how many
        // covered ones it misses.
        const int wrong = held + filled - 2 * shared;

        if (wrong >= wrongest) continue;

        wrongest = wrong;
        made = shape.glyph;
    }

    return made;
}

// ASCII characters from the sparsest to the densest, by how many of the eight subcells the ink
// covers.
constexpr std::string_view ramp = ".:-=+*%#";

// Ink in monochrome: the terminal has no colors, so only the silhouette remains. An image with
// transparency (a logo, an icon) has it in the alpha channel, and the ink is what is visible. An
// opaque image (a photo) has no silhouette, so the ink is what is brighter than the image's mean
// lightness.
struct Ink {
    bool clear = false;
    double middle = 0.0;
};

// The perceptual lightness of a pixel, in linear light.
[[nodiscard]] double lightness(const std::byte* at) {
    const std::array<double, 256>& tone = straight();

    return weights[0] * tone[sample(at[0])] + weights[1] * tone[sample(at[1])] + weights[2] * tone[sample(at[2])];
}

// Decides what counts as ink in a monochrome picture: whether the image has any transparency,
// and its mean lightness.
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

// How many subcells make up one cell. Block characters need eight by eight, because an eighth
// cuts at that many places; sextants two by three; without Unicode two by four, as many as the
// ramp of ASCII characters counts.
//
// Octants are cut down to sextants deliberately. There are two hundred thirty characters, not two
// hundred fifty-six, because twenty-six patterns live in Block Elements and at U+1FB00, so the
// mapping is a table, not a formula. The table must not be guessed: a mistake means garbage on
// the screen, not a worse picture. Octants can come separately, with a verified table.
ui::Size grain(ui::Blocks kind, bool unicode) {
    if (!unicode) return {2, 4};

    return kind >= ui::Blocks::sextants ? ui::Size{2, 3} : ui::Size{8, 8};
}

// Draws one still of a picture onto the canvas as a mosaic of characters, starting at the given
// spot. The picture must already be scaled to the grid of subcells: the number of cells is derived
// from its size, so one cannot be given while the other is meant.
//
// Transparency fits straight into the library's model: a cell without a single covered subcell is
// left as it is, so the parent's background shows through it. A partly covered cell gets
// the paper from the caller when one is given (the background of the image role in the skin);
// without it, it goes as foreground only (`Canvas::foreground`) and takes its background from what
// lies underneath. Left transparent all the way to the terminal, it would get
// the terminal's own background: black cells around a logo in a window.
//
// As well as possible: in full color and in a palette a cell carries two colors, and only the
// output reduces them to the terminal's palette. In monochrome there are no colors, so a
// two-color cell would lose its background and make a hole; only the ink silhouette (`Ink`) is
// drawn. Without Unicode there are no block characters: a cell is a character from the ASCII ramp
// by coverage, and in color a full cell is a background in the image's color.
void drawn(Canvas& into, ui::Spot at, const Pixels& one, std::size_t still, ui::Blocks kind,
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

            if (filled == 0) continue;

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
