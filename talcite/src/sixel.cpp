#include "talcite/sixel.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

#include "lighting.hpp"

namespace talcite {

// Sixel: an image as a string of characters that the terminal draws as pixels. Six rows of pixels
// make one band; within a band each color goes in a separate pass, and a character from ? to ~
// carries six bits: which of the column's six pixels have that color. The encoder is our own,
// without an adapter: it is under two hundred lines, no library would do it better, and each would
// add a seam.
//
// Three choices worth knowing:
//
// - Transparency through P2 = 1. A transparent pixel belongs to no color, and the terminal leaves
//   what was under it: the background of the cell that `Term` has just painted. The edge of a logo
//   on a window's background therefore comes out as it does with characters, with the background
//   underneath.
// - Height rounded down to full bands. A terminal that receives an incomplete band draws it as six
//   rows and may scroll the screen when the image stands at the bottom. So the height is cut to a
//   multiple of six (an image shorter than six stays as it is), losing at most five pixel rows.
// - The palette comes from median cut, weighted by pixel count, down to `colors` colors. An image
//   with fewer colors (a logo, an icon) gets them exactly. A photo goes through 5x5x5-bit buckets:
//   the buckets are cut, not the colors, and the nearest palette color is computed once per
//   bucket. An 800x1200 poster encodes this way in a few tens of milliseconds instead of almost a
//   second.
//
// The raster header "1;1;width;height declares square pixels and the image size, so the terminal
// can reserve the space at once.
namespace {

// The alpha from which a pixel counts as opaque.
constexpr std::uint8_t solid = 128;

struct Tone {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;

    friend constexpr auto operator<=>(Tone, Tone) = default;
};

// A color and how many pixels have it.
struct Counted {
    Tone tone;
    std::size_t many = 0;
};

// Median cut: the box with the largest spread on any channel is split into two halves by pixel
// count, until there are as many boxes as colors allowed (`most`). A box's color is its weighted
// mean. With no more colors than allowed, they are returned as they are.
std::vector<Tone> palette_of(std::vector<Counted> all, std::size_t most) {
    std::vector<Tone> made;

    if (all.empty()) return made;

    if (all.size() <= most) {
        for (const Counted& one : all) made.push_back(one.tone);

        return made;
    }

    struct Box {
        std::size_t from = 0;
        std::size_t upto = 0;
        int widest = 0;
        int channel = 0;
    };

    // A box's spread is computed once, when the box is made; with a photo's hundred thousand
    // colors, recomputing it at every step would be quadratic.
    const auto measured = [&all](std::size_t from, std::size_t upto) {
        Box made{from, upto, -1, 0};

        if (upto - from < 2) {
            made.widest = 0;

            return made;
        }

        for (int band = 0; band < 3; ++band) {
            int least = 255;
            int largest = 0;

            for (std::size_t at = from; at < upto; ++at) {
                const Tone& one = all[at].tone;
                const int value = band == 0 ? one.red : band == 1 ? one.green : one.blue;

                least = std::min(least, value);
                largest = std::max(largest, value);
            }

            if (largest - least > made.widest) {
                made.widest = largest - least;
                made.channel = band;
            }
        }

        return made;
    };

    std::vector<Box> boxes{measured(0, all.size())};

    while (boxes.size() < most) {
        std::size_t chosen = boxes.size();
        int widest = 0;

        for (std::size_t at = 0; at < boxes.size(); ++at)
            if (boxes[at].widest > widest) {
                widest = boxes[at].widest;
                chosen = at;
            }

        if (chosen == boxes.size()) break;

        const Box box = boxes[chosen];
        const int channel = box.channel;

        const auto key = [channel](const Counted& one) {
            return channel == 0 ? one.tone.red : channel == 1 ? one.tone.green : one.tone.blue;
        };

        std::sort(all.begin() + static_cast<std::ptrdiff_t>(box.from), all.begin() + static_cast<std::ptrdiff_t>(box.upto),
                  [&key](const Counted& one, const Counted& two) { return key(one) < key(two); });

        std::size_t total = 0;

        for (std::size_t at = box.from; at < box.upto; ++at) total += all[at].many;

        std::size_t half = box.from;
        std::size_t gathered = 0;

        while (half < box.upto - 1 && gathered + all[half].many <= total / 2) gathered += all[half++].many;

        if (half == box.from) ++half;

        boxes[chosen] = measured(box.from, half);
        boxes.push_back(measured(half, box.upto));
    }

    for (const Box& box : boxes) {
        double sums[3] = {};
        std::size_t many = 0;

        for (std::size_t at = box.from; at < box.upto; ++at) {
            sums[0] += static_cast<double>(all[at].tone.red) * static_cast<double>(all[at].many);
            sums[1] += static_cast<double>(all[at].tone.green) * static_cast<double>(all[at].many);
            sums[2] += static_cast<double>(all[at].tone.blue) * static_cast<double>(all[at].many);
            many += all[at].many;
        }

        const auto mean = [many](double sum) {
            return static_cast<std::uint8_t>(std::clamp(sum / static_cast<double>(many) + 0.5, 0.0, 255.0));
        };

        made.push_back(Tone{mean(sums[0]), mean(sums[1]), mean(sums[2])});
    }

    return made;
}

// The index of the palette color closest to `one`, by squared distance weighted 2:4:3 for red,
// green and blue.
std::size_t nearest(Tone one, const std::vector<Tone>& palette) {
    std::size_t made = 0;
    int least = std::numeric_limits<int>::max();

    for (std::size_t at = 0; at < palette.size(); ++at) {
        const int red = one.red - palette[at].red;
        const int green = one.green - palette[at].green;
        const int blue = one.blue - palette[at].blue;
        const int here = 2 * red * red + 4 * green * green + 3 * blue * blue;

        if (here < least) {
            least = here;
            made = at;
        }
    }

    return made;
}

// Appends the number in decimal.
void number(std::size_t value, std::string& into) { into += std::to_string(value); }

// Writes one run within a color's pass: the character repeated `run` times; from four repetitions on
// the repeat introducer ! is cheaper.
void flushed(char sign, std::size_t run, std::string& into) {
    if (run == 0) return;

    if (run >= 4) {
        into += '!';
        number(run, into);
        into += sign;

        return;
    }

    into.append(run, sign);
}

}

// Encodes frame `still` of the image as a complete sixel sequence (DCS to ST) with at most `colors`
// colors (clamped to 1-256). Returns an empty string when there is no such frame, the image is empty
// or its bytes are too few for its size.
std::string sixel(const Pixels& one, std::size_t still, std::size_t colors) {
    if (!one.held() || still >= one.stills().size()) return {};

    const ui::Size size = one.size();
    const Bytes& bytes = one.stills()[still].bytes;

    const auto wide = static_cast<std::size_t>(size.wide);
    const std::size_t full = static_cast<std::size_t>(size.high);
    const std::size_t high = full >= 6 ? full - full % 6 : full;

    if (wide == 0 || high == 0 || bytes.size() < wide * full * 4) return {};

    colors = std::clamp<std::size_t>(colors, 1, 256);

    const std::size_t count = wide * high;

    const auto seen = [&bytes](std::size_t at) { return sample(bytes[at * 4 + 3]) >= solid; };

    const auto tone_at = [&bytes](std::size_t at) {
        return Tone{sample(bytes[at * 4]), sample(bytes[at * 4 + 1]), sample(bytes[at * 4 + 2])};
    };

    const auto key_of = [](Tone tone) {
        return (static_cast<std::size_t>(tone.red >> 3) << 10) | (static_cast<std::size_t>(tone.green >> 3) << 5) |
               static_cast<std::size_t>(tone.blue >> 3);
    };

    // The color index of every pixel; `none` is a transparent pixel.
    constexpr std::uint16_t none = std::numeric_limits<std::uint16_t>::max();

    std::vector<std::uint16_t> picked(count, none);
    std::vector<Tone> palette;

    // First the exact attempt: colors are counted directly as long as there are no more than
    // allowed. A logo or an icon ends here, with a palette in color order and the index found by
    // binary search.
    std::unordered_map<std::uint32_t, std::size_t> exact;
    bool fits = true;

    exact.reserve(colors * 2);

    for (std::size_t at = 0; at < count && fits; ++at) {
        if (!seen(at)) continue;

        const Tone tone = tone_at(at);

        ++exact[(static_cast<std::uint32_t>(tone.red) << 16) | (static_cast<std::uint32_t>(tone.green) << 8) | tone.blue];
        fits = exact.size() <= colors;
    }

    if (fits) {
        for (const auto& [key, many] : exact)
            palette.push_back(Tone{static_cast<std::uint8_t>(key >> 16), static_cast<std::uint8_t>(key >> 8),
                                   static_cast<std::uint8_t>(key)});

        std::sort(palette.begin(), palette.end());

        for (std::size_t at = 0; at < count; ++at)
            if (seen(at))
                picked[at] = static_cast<std::uint16_t>(std::lower_bound(palette.begin(), palette.end(), tone_at(at)) -
                                                        palette.begin());
    } else {
        // A photo: colors go into 5x5x5-bit buckets with channel sums, so median cut splits at most
        // 32768 buckets rather than hundreds of thousands of colors, and a box's mean still comes out
        // exact. A bucket gets its nearest palette color once, for its own mean.
        struct Bin {
            std::size_t many = 0;
            std::uint64_t sums[3] = {};
        };

        std::vector<Bin> bins(32 * 32 * 32);

        for (std::size_t at = 0; at < count; ++at) {
            if (!seen(at)) continue;

            const Tone tone = tone_at(at);
            Bin& bin = bins[key_of(tone)];

            ++bin.many;
            bin.sums[0] += tone.red;
            bin.sums[1] += tone.green;
            bin.sums[2] += tone.blue;
        }

        std::vector<Counted> all;
        std::vector<std::size_t> keys;

        for (std::size_t key = 0; key < bins.size(); ++key) {
            const Bin& bin = bins[key];

            if (bin.many == 0) continue;

            const auto mean = [&bin](std::uint64_t sum) {
                return static_cast<std::uint8_t>((sum + bin.many / 2) / bin.many);
            };

            all.push_back(Counted{Tone{mean(bin.sums[0]), mean(bin.sums[1]), mean(bin.sums[2])}, bin.many});
            keys.push_back(key);
        }

        std::vector<Tone> means;

        means.reserve(all.size());

        for (const Counted& one : all) means.push_back(one.tone);

        palette = palette_of(std::move(all), colors);

        std::vector<std::uint16_t> chosen(bins.size(), none);

        for (std::size_t at = 0; at < keys.size(); ++at)
            chosen[keys[at]] = static_cast<std::uint16_t>(nearest(means[at], palette));

        for (std::size_t at = 0; at < count; ++at)
            if (seen(at)) picked[at] = chosen[key_of(tone_at(at))];
    }

    std::string made = "\x1bP0;1;0q\"1;1;";

    number(wide, made);
    made += ';';
    number(high, made);

    for (std::size_t at = 0; at < palette.size(); ++at) {
        const auto percent = [](std::uint8_t value) { return static_cast<std::size_t>((value * 100 + 127) / 255); };

        made += '#';
        number(at, made);
        made += ";2;";
        number(percent(palette[at].red), made);
        made += ';';
        number(percent(palette[at].green), made);
        made += ';';
        number(percent(palette[at].blue), made);
    }

    // Each band is walked once: every pixel adds its bit to the column of its color, and each color
    // remembers its first and last occupied column. A color's pass starts with the empty columns as
    // one ! repeat and ends at the last occupied column; empty columns at the end draw nothing, so
    // they are not written.
    std::vector<std::uint8_t> bits(palette.size() * wide);
    std::vector<std::size_t> first(palette.size(), wide);
    std::vector<std::size_t> last(palette.size(), 0);
    std::vector<std::uint16_t> used;

    used.reserve(palette.size());

    for (std::size_t top = 0; top < high; top += 6) {
        const std::size_t rows = std::min<std::size_t>(6, high - top);

        used.clear();

        for (std::size_t row = 0; row < rows; ++row)
            for (std::size_t x = 0; x < wide; ++x) {
                const std::uint16_t which = picked[(top + row) * wide + x];

                if (which == none) continue;

                if (first[which] == wide) used.push_back(which);

                bits[which * wide + x] |= static_cast<std::uint8_t>(1u << row);
                first[which] = std::min(first[which], x);
                last[which] = std::max(last[which], x + 1);
            }

        std::sort(used.begin(), used.end());

        bool begun = false;

        for (const std::uint16_t which : used) {
            std::uint8_t* const line = bits.data() + static_cast<std::size_t>(which) * wide;

            if (begun) made += '$';

            begun = true;

            made += '#';
            number(which, made);

            flushed('?', first[which], made);

            char sign = 0;
            std::size_t run = 0;

            for (std::size_t x = first[which]; x < last[which]; ++x) {
                const char here = static_cast<char>(63 + line[x]);

                line[x] = 0;

                if (here == sign) {
                    ++run;

                    continue;
                }

                flushed(sign, run, made);

                sign = here;
                run = 1;
            }

            flushed(sign, run, made);

            first[which] = wide;
            last[which] = 0;
        }

        if (top + 6 < high) made += '-';
    }

    made += "\x1b\\";

    return made;
}

}
