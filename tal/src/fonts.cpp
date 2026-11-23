#include "tal/fonts.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tal/text.hpp"
#include "tal/trouble.hpp"

// Fonts (E7b): our own reader of TrueType, whose glyphs come out as paths. A font is a file of
// other people, so every offset and length is checked against the file before it is read: a font
// that is broken where it starts (its directory, its head, its metrics, its map of characters) is
// Trouble malformed when read, and a glyph broken on its own is drawn as nothing, as is a glyph
// that nests its parts more than eight deep or holds more than 65 536 points. A file over 64 MB is
// Trouble full; outlines in CFF (an .otf of PostScript curves) are Trouble unsupported. A font
// collection (.ttc) gives its first font.
//
// The glyphs of TrueType are contours of quadratic curves: points on the curve and between them
// control points off it, two control points in a row standing for the on-curve point midway. A
// composite glyph places other glyphs, each moved, scaled or turned by its own matrix, or moved so
// that one of its points falls on a point of the glyph so far. Coordinates are font units, y up;
// a path takes them scaled to the size (in the path's units per em) and y down.
//
// Text is laid out a character at a time: the map (cmap 12, else 4) gives each its glyph,
// .notdef where the font has none, the advances come from hmtx, and pairs are kerned by GPOS
// (pair adjustment of the feature kern, formats 1 and 2) or, without it, by the old kern table.
// No ligatures and no shaping of scripts that need it. Control characters are spaces, and a
// sequence that is not UTF-8 is U+FFFD, as everywhere in tal. A baseline at the top or bottom is
// the font's ascender or descender (hhea).
namespace tal::drawing {

namespace {

constexpr std::size_t most_file = std::size_t{64} << 20;
constexpr std::size_t most_points = 65536;
constexpr int most_depth = 8;

// The file of a font, read big-endian with every read checked; a read beyond it is Trouble
// malformed.
struct Reading {
    const Bytes* file = nullptr;

    // The byte at `at`.
    [[nodiscard]] std::uint32_t u8(std::size_t at) const {
        if (at >= file->size()) throw Trouble{Kind::malformed, "A font read beyond its end."};

        return std::to_integer<std::uint32_t>((*file)[at]);
    }

    // The unsigned 16 bits at `at`.
    [[nodiscard]] std::uint32_t u16(std::size_t at) const { return (u8(at) << 8) | u8(at + 1); }

    // The signed 16 bits at `at`.
    [[nodiscard]] int i16(std::size_t at) const { return static_cast<std::int16_t>(static_cast<std::uint16_t>(u16(at))); }

    // The unsigned 32 bits at `at`.
    [[nodiscard]] std::uint32_t u32(std::size_t at) const { return (u16(at) << 16) | u16(at + 2); }

    // Whether [`at`, `at` + `much`) lies within the file.
    [[nodiscard]] bool holds(std::size_t at, std::size_t much) const { return at <= file->size() && much <= file->size() - at; }
};

// A table of the font: where it starts and how long it is.
struct Table {
    std::size_t at = 0;
    std::size_t length = 0;
};

// A point of a contour, in font units, on the curve or off it.
struct Dot {
    double x = 0;
    double y = 0;
    bool on = true;
};

using Contour = std::vector<Dot>;

}

// What a font holds once read: the file and where its tables are.
struct Font::Data {
    Bytes file;
    std::string name;
    double units = 2048;
    double ascender = 0;
    double descender = 0;
    std::uint32_t glyphs = 0;
    std::uint32_t metrics = 0;
    bool long_offsets = false;
    Table loca;
    Table glyf;
    Table hmtx;
    std::size_t map = 0;
    std::uint32_t format = 0;
    std::vector<std::size_t> pairs;
    std::map<std::uint32_t, int> kerned;

    // The reading of the file.
    [[nodiscard]] Reading reading() const { return Reading{&file}; }

    // The glyph of `code`, 0 (.notdef) when the font has none.
    [[nodiscard]] std::uint32_t glyph(char32_t code) const;

    // How far a glyph moves the pen, in font units.
    [[nodiscard]] double advance(std::uint32_t glyph) const;

    // The kerning of the pair `first`, `second`, in font units.
    [[nodiscard]] double kerning(std::uint32_t first, std::uint32_t second) const;

    // The contours of `glyph`, none when it is broken.
    [[nodiscard]] std::vector<Contour> contours(std::uint32_t glyph) const;

private:
    void gathered(std::uint32_t glyph, int depth, std::vector<Contour>& into, std::size_t& points) const;

    [[nodiscard]] std::optional<int> paired(std::size_t subtable, std::uint32_t first, std::uint32_t second) const;
};

namespace {

// The index of `glyph` in the coverage table at `at`, none when it is not covered.
std::optional<std::uint32_t> covered(const Reading& read, std::size_t at, std::uint32_t glyph) {
    const std::uint32_t format = read.u16(at);
    const std::uint32_t count = read.u16(at + 2);

    if (format == 1) {
        std::uint32_t low = 0;
        std::uint32_t high = count;

        while (low < high) {
            const std::uint32_t middle = (low + high) / 2;
            const std::uint32_t one = read.u16(at + 4 + 2 * std::size_t{middle});

            if (one == glyph) return middle;

            if (one < glyph) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }

        return std::nullopt;
    }

    if (format == 2) {
        std::uint32_t low = 0;
        std::uint32_t high = count;

        while (low < high) {
            const std::uint32_t middle = (low + high) / 2;
            const std::size_t record = at + 4 + 6 * std::size_t{middle};
            const std::uint32_t start = read.u16(record);
            const std::uint32_t end = read.u16(record + 2);

            if (glyph < start) {
                high = middle;
            } else if (glyph > end) {
                low = middle + 1;
            } else {
                return read.u16(record + 4) + glyph - start;
            }
        }
    }

    return std::nullopt;
}

// The class of `glyph` in the class definition at `at`, 0 when it has none.
std::uint32_t classed(const Reading& read, std::size_t at, std::uint32_t glyph) {
    const std::uint32_t format = read.u16(at);

    if (format == 1) {
        const std::uint32_t start = read.u16(at + 2);
        const std::uint32_t count = read.u16(at + 4);

        return glyph >= start && glyph - start < count ? read.u16(at + 6 + 2 * std::size_t{glyph - start}) : 0;
    }

    if (format == 2) {
        const std::uint32_t count = read.u16(at + 2);
        std::uint32_t low = 0;
        std::uint32_t high = count;

        while (low < high) {
            const std::uint32_t middle = (low + high) / 2;
            const std::size_t record = at + 4 + 6 * std::size_t{middle};
            const std::uint32_t start = read.u16(record);
            const std::uint32_t end = read.u16(record + 2);

            if (glyph < start) {
                high = middle;
            } else if (glyph > end) {
                low = middle + 1;
            } else {
                return read.u16(record + 4);
            }
        }
    }

    return 0;
}

// The size in bytes of a value record of `format`.
std::size_t valued(std::uint32_t format) { return 2 * static_cast<std::size_t>(std::popcount(format & 0xFF)); }

// The advance along x in the value record at `at` of `format`, 0 when it has none.
int advanced(const Reading& read, std::size_t at, std::uint32_t format) {
    if ((format & 0x0004) == 0) return 0;

    const std::size_t before = 2 * static_cast<std::size_t>(std::popcount(format & 0x0003));

    return read.i16(at + before);
}

// The tables of the font file at `start` (the offset table), by their tags.
std::map<std::string, Table> tabled(const Reading& read, std::size_t start) {
    const std::uint32_t count = read.u16(start + 4);
    std::map<std::string, Table> made;

    for (std::uint32_t one = 0; one < count; ++one) {
        const std::size_t record = start + 12 + 16 * std::size_t{one};
        std::string tag;

        for (std::size_t letter = 0; letter < 4; ++letter) tag += static_cast<char>(read.u8(record + letter));

        const Table table{read.u32(record + 8), read.u32(record + 12)};

        if (!read.holds(table.at, table.length)) throw Trouble{Kind::malformed, "A font table beyond the end of the file."};

        made.emplace(tag, table);
    }

    return made;
}

// The full name of the font from its name table, empty when it has none that can be read.
std::string named(const Reading& read, const Table& table) {
    const std::uint32_t count = read.u16(table.at + 2);
    const std::size_t strings = table.at + read.u16(table.at + 4);
    std::string mac;

    for (std::uint32_t one = 0; one < count; ++one) {
        const std::size_t record = table.at + 6 + 12 * std::size_t{one};
        const std::uint32_t platform = read.u16(record);
        const std::uint32_t name = read.u16(record + 6);
        const std::size_t length = read.u16(record + 8);
        const std::size_t at = strings + read.u16(record + 10);

        if (name != 4 || !read.holds(at, length)) continue;

        if (platform == 3 || platform == 0) {
            std::string made;

            for (std::size_t unit = 0; unit + 1 < length; unit += 2) {
                const std::uint32_t code = read.u16(at + unit);

                text::encoded(made, code >= 0xD800 && code < 0xE000 ? char32_t{0xFFFD} : static_cast<char32_t>(code));
            }

            return made;
        }

        if (platform == 1 && mac.empty())
            for (std::size_t letter = 0; letter < length; ++letter) {
                const std::uint32_t byte = read.u8(at + letter);

                mac += byte < 0x80 ? static_cast<char>(byte) : '?';
            }
    }

    return mac;
}

// The lookups of GPOS of the feature kern, as the offsets of their pair adjustment subtables.
std::vector<std::size_t> kerns(const Reading& read, const Table& table) {
    std::vector<std::size_t> made;

    if (table.length < 10) return made;

    const std::size_t features = table.at + read.u16(table.at + 6);
    const std::size_t lookups = table.at + read.u16(table.at + 8);
    const std::uint32_t feature_count = read.u16(features);
    std::vector<std::uint32_t> wanted;

    for (std::uint32_t one = 0; one < feature_count; ++one) {
        const std::size_t record = features + 2 + 6 * std::size_t{one};

        if (read.u8(record) != 'k' || read.u8(record + 1) != 'e' || read.u8(record + 2) != 'r' || read.u8(record + 3) != 'n') continue;

        const std::size_t feature = features + read.u16(record + 4);
        const std::uint32_t count = read.u16(feature + 2);

        for (std::uint32_t index = 0; index < count; ++index) wanted.push_back(read.u16(feature + 4 + 2 * std::size_t{index}));
    }

    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());

    const std::uint32_t lookup_count = read.u16(lookups);

    for (const std::uint32_t index : wanted) {
        if (index >= lookup_count) continue;

        const std::size_t lookup = lookups + read.u16(lookups + 2 + 2 * std::size_t{index});
        const std::uint32_t type = read.u16(lookup);
        const std::uint32_t count = read.u16(lookup + 4);

        for (std::uint32_t one = 0; one < count && made.size() < 4096; ++one) {
            std::size_t subtable = lookup + read.u16(lookup + 6 + 2 * std::size_t{one});

            if (type == 9) {
                if (read.u16(subtable + 2) != 2) continue;

                subtable += read.u32(subtable + 4);
            } else if (type != 2) {
                continue;
            }

            made.push_back(subtable);
        }
    }

    return made;
}

// The pairs of the old kern table, horizontal subtables of format 0 only.
std::map<std::uint32_t, int> kerned(const Reading& read, const Table& table) {
    std::map<std::uint32_t, int> made;

    if (table.length < 4 || read.u16(table.at) != 0) return made;

    const std::uint32_t count = read.u16(table.at + 2);
    std::size_t at = table.at + 4;

    for (std::uint32_t one = 0; one < count && at < table.at + table.length; ++one) {
        const std::size_t length = read.u16(at + 2);
        const std::uint32_t coverage = read.u16(at + 4);

        if ((coverage >> 8) == 0 && (coverage & 0x0F) == 0x01) {
            const std::uint32_t pairs = read.u16(at + 6);

            for (std::uint32_t pair = 0; pair < pairs && made.size() < most_points; ++pair) {
                const std::size_t record = at + 14 + 6 * std::size_t{pair};

                made[(read.u16(record) << 16) | read.u16(record + 2)] = read.i16(record + 4);
            }
        }

        if (length < 6) break;

        at += length;
    }

    return made;
}

// The subtable of the character map to use and its format: 12 of Unicode first, 4 after.
std::pair<std::size_t, std::uint32_t> mapped(const Reading& read, const Table& table) {
    const std::uint32_t count = read.u16(table.at + 2);
    std::size_t four = 0;

    for (std::uint32_t one = 0; one < count; ++one) {
        const std::size_t record = table.at + 4 + 8 * std::size_t{one};
        const std::uint32_t platform = read.u16(record);
        const std::uint32_t encoding = read.u16(record + 2);
        const std::size_t at = table.at + read.u32(record + 4);
        const std::uint32_t format = read.u16(at);
        const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));

        if (!unicode) continue;

        if (format == 12) return {at, 12};

        if (format == 4 && four == 0) four = at;
    }

    if (four == 0) throw Trouble{Kind::malformed, "A font without a Unicode map of its characters."};

    return {four, 4};
}

// `contour` as pieces of `path`, mapped by `place` (font units to the path's).
void traced(const Contour& contour, const Matrix& place, Path& path) {
    const std::size_t count = contour.size();

    if (count < 2) return;

    const auto at = [&](const Dot& one) { return place.applied(Point{one.x, one.y}); };
    const auto between = [](const Dot& one, const Dot& other) { return Dot{(one.x + other.x) / 2, (one.y + other.y) / 2, true}; };
    std::size_t first = 0;
    Dot start;

    while (first < count && !contour[first].on) ++first;

    if (first == count) {
        start = between(contour[0], contour[1]);
        first = 0;
    } else {
        start = contour[first];
    }

    path.move(at(start));

    std::optional<Dot> control;

    for (std::size_t step = 1; step <= count; ++step) {
        const Dot& one = contour[(first + step) % count];

        if (one.on) {
            if (control) {
                path.quad(at(*control), at(one));
                control.reset();
            } else {
                path.line(at(one));
            }

            continue;
        }

        if (control) {
            const Dot middle = between(*control, one);

            path.quad(at(*control), at(middle));
        }

        control = one;
    }

    if (control) path.quad(at(*control), at(start));

    path.close();
}

}

// The glyph of `code` by the map of characters, 0 when the font has none or the map is broken.
std::uint32_t Font::Data::glyph(char32_t code) const {
    const Reading read = reading();
    std::uint32_t made = 0;

    try {
        if (format == 12) {
            const std::uint32_t groups = read.u32(map + 12);
            std::uint32_t low = 0;
            std::uint32_t high = groups;

            while (low < high) {
                const std::uint32_t middle = low + (high - low) / 2;
                const std::size_t record = map + 16 + 12 * std::size_t{middle};
                const std::uint32_t start = read.u32(record);
                const std::uint32_t end = read.u32(record + 4);

                if (code < start) {
                    high = middle;
                } else if (code > end) {
                    low = middle + 1;
                } else {
                    made = read.u32(record + 8) + (code - start);
                    break;
                }
            }
        } else if (code <= 0xFFFF) {
            const std::uint32_t segments = read.u16(map + 6) / 2;
            const std::size_t ends = map + 14;
            const std::size_t starts = ends + 2 * std::size_t{segments} + 2;
            const std::size_t deltas = starts + 2 * std::size_t{segments};
            const std::size_t ranges = deltas + 2 * std::size_t{segments};
            std::uint32_t low = 0;
            std::uint32_t high = segments;

            while (low < high) {
                const std::uint32_t middle = (low + high) / 2;

                if (read.u16(ends + 2 * std::size_t{middle}) < code) {
                    low = middle + 1;
                } else {
                    high = middle;
                }
            }

            if (low < segments && read.u16(starts + 2 * std::size_t{low}) <= code) {
                const std::uint32_t start = read.u16(starts + 2 * std::size_t{low});
                const std::uint32_t delta = read.u16(deltas + 2 * std::size_t{low});
                const std::size_t range_at = ranges + 2 * std::size_t{low};
                const std::uint32_t range = read.u16(range_at);

                if (range == 0) {
                    made = (code + delta) & 0xFFFF;
                } else {
                    const std::uint32_t found = read.u16(range_at + range + 2 * std::size_t{code - start});

                    made = found == 0 ? 0 : (found + delta) & 0xFFFF;
                }
            }
        }
    } catch (const Trouble&) {
        return 0;
    }

    return made < glyphs ? made : 0;
}

// The advance of `glyph`: its own metric, or the last one for glyphs past the metrics.
double Font::Data::advance(std::uint32_t glyph) const {
    const Reading read = reading();
    const std::uint32_t index = std::min(glyph, metrics - 1);

    return read.u16(hmtx.at + 4 * std::size_t{index});
}

// The kerning of `first` followed by `second`: GPOS where the font has its kern feature, the old
// kern table otherwise; a broken subtable kerns nothing.
double Font::Data::kerning(std::uint32_t first, std::uint32_t second) const {
    if (!pairs.empty()) {
        for (const std::size_t subtable : pairs) {
            try {
                if (const std::optional<int> found = paired(subtable, first, second)) return *found;
            } catch (const Trouble&) {
            }
        }

        return 0;
    }

    const auto found = kerned.find((first << 16) | second);

    return found == kerned.end() ? 0 : found->second;
}

// The advance the pair adjustment subtable at `subtable` gives `first` before `second`, none when
// it does not hold the pair.
std::optional<int> Font::Data::paired(std::size_t subtable, std::uint32_t first, std::uint32_t second) const {
    const Reading read = reading();
    const std::uint32_t kind = read.u16(subtable);
    const std::optional<std::uint32_t> index = covered(read, subtable + read.u16(subtable + 2), first);

    if (!index) return std::nullopt;

    const std::uint32_t one = read.u16(subtable + 4);
    const std::uint32_t two = read.u16(subtable + 6);
    const std::size_t record = valued(one) + valued(two);

    if (kind == 1) {
        const std::uint32_t sets = read.u16(subtable + 8);

        if (*index >= sets) return std::nullopt;

        const std::size_t set = subtable + read.u16(subtable + 10 + 2 * std::size_t{*index});
        const std::uint32_t count = read.u16(set);
        std::uint32_t low = 0;
        std::uint32_t high = count;

        while (low < high) {
            const std::uint32_t middle = (low + high) / 2;
            const std::size_t at = set + 2 + (2 + record) * std::size_t{middle};
            const std::uint32_t glyph = read.u16(at);

            if (glyph == second) return advanced(read, at + 2, one);

            if (glyph < second) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }

        return std::nullopt;
    }

    if (kind == 2) {
        const std::uint32_t left = classed(read, subtable + read.u16(subtable + 8), first);
        const std::uint32_t right = classed(read, subtable + read.u16(subtable + 10), second);
        const std::uint32_t lefts = read.u16(subtable + 12);
        const std::uint32_t rights = read.u16(subtable + 14);

        if (left >= lefts || right >= rights) return std::nullopt;

        return advanced(read, subtable + 16 + record * (std::size_t{left} * rights + right), one);
    }

    return std::nullopt;
}

// The contours of `glyph` in font units; a broken glyph has none.
std::vector<Contour> Font::Data::contours(std::uint32_t glyph) const {
    std::vector<Contour> made;
    std::size_t points = 0;

    try {
        gathered(glyph, 0, made, points);
    } catch (const Trouble&) {
        return {};
    }

    return made;
}

// The contours of `glyph` added to `into`, a composite's parts `depth` deep. Throws Trouble
// malformed for a glyph out of its table, too deep, of too many points or cut short.
void Font::Data::gathered(std::uint32_t glyph, int depth, std::vector<Contour>& into, std::size_t& points) const {
    if (depth > most_depth) throw Trouble{Kind::malformed, "A glyph of parts nested too deep."};

    if (glyph >= glyphs) throw Trouble{Kind::malformed, "A glyph the font does not have."};

    const Reading read = reading();
    const std::size_t from = long_offsets ? read.u32(loca.at + 4 * std::size_t{glyph}) : 2 * std::size_t{read.u16(loca.at + 2 * std::size_t{glyph})};
    const std::size_t to = long_offsets ? read.u32(loca.at + 4 * std::size_t{glyph} + 4) : 2 * std::size_t{read.u16(loca.at + 2 * std::size_t{glyph} + 2)};

    if (to <= from) return;

    if (to > glyf.length) throw Trouble{Kind::malformed, "A glyph beyond its table."};

    const std::size_t at = glyf.at + from;
    const std::size_t end = glyf.at + to;
    const int contours = read.i16(at);

    if (contours >= 0) {
        std::vector<std::size_t> ends;

        for (int one = 0; one < contours; ++one) ends.push_back(read.u16(at + 10 + 2 * static_cast<std::size_t>(one)));

        if (ends.empty()) return;

        for (std::size_t one = 1; one < ends.size(); ++one)
            if (ends[one] < ends[one - 1]) throw Trouble{Kind::malformed, "A glyph whose contours end out of order."};

        const std::size_t count = ends.back() + 1;

        points += count;

        if (points > most_points) throw Trouble{Kind::malformed, "A glyph of too many points."};

        std::size_t cursor = at + 10 + 2 * static_cast<std::size_t>(contours);
        const std::size_t instructions = read.u16(cursor);

        cursor += 2 + instructions;

        std::vector<std::uint32_t> flags;

        flags.reserve(count);

        while (flags.size() < count) {
            if (cursor >= end) throw Trouble{Kind::malformed, "A glyph cut short."};

            const std::uint32_t flag = read.u8(cursor++);

            flags.push_back(flag);

            if ((flag & 0x08) != 0) {
                const std::uint32_t repeat = read.u8(cursor++);

                for (std::uint32_t one = 0; one < repeat && flags.size() < count; ++one) flags.push_back(flag);
            }
        }

        std::vector<Dot> dots(count);
        int x = 0;
        int y = 0;

        for (std::size_t one = 0; one < count; ++one) {
            const std::uint32_t flag = flags[one];

            if ((flag & 0x02) != 0) {
                const int step = static_cast<int>(read.u8(cursor++));

                x += (flag & 0x10) != 0 ? step : -step;
            } else if ((flag & 0x10) == 0) {
                x += read.i16(cursor);
                cursor += 2;
            }

            dots[one].x = x;
            dots[one].on = (flag & 0x01) != 0;
        }

        for (std::size_t one = 0; one < count; ++one) {
            const std::uint32_t flag = flags[one];

            if ((flag & 0x04) != 0) {
                const int step = static_cast<int>(read.u8(cursor++));

                y += (flag & 0x20) != 0 ? step : -step;
            } else if ((flag & 0x20) == 0) {
                y += read.i16(cursor);
                cursor += 2;
            }

            dots[one].y = y;
        }

        if (cursor > end) throw Trouble{Kind::malformed, "A glyph cut short."};

        std::size_t start = 0;

        for (const std::size_t last : ends) {
            into.emplace_back(dots.begin() + static_cast<std::ptrdiff_t>(start), dots.begin() + static_cast<std::ptrdiff_t>(last) + 1);
            start = last + 1;
        }

        return;
    }

    std::size_t cursor = at + 10;

    for (bool more = true; more;) {
        const std::uint32_t flags = read.u16(cursor);
        const std::uint32_t part = read.u16(cursor + 2);

        cursor += 4;

        double first = 0;
        double second = 0;
        const bool words = (flags & 0x0001) != 0;
        const bool offsets = (flags & 0x0002) != 0;

        if (words) {
            first = offsets ? read.i16(cursor) : static_cast<double>(read.u16(cursor));
            second = offsets ? read.i16(cursor + 2) : static_cast<double>(read.u16(cursor + 2));
            cursor += 4;
        } else {
            first = offsets ? static_cast<std::int8_t>(read.u8(cursor)) : static_cast<double>(read.u8(cursor));
            second = offsets ? static_cast<std::int8_t>(read.u8(cursor + 1)) : static_cast<double>(read.u8(cursor + 1));
            cursor += 2;
        }

        const auto fixed = [&](std::size_t where) { return read.i16(where) / 16384.0; };
        double a = 1;
        double b = 0;
        double c = 0;
        double d = 1;

        if ((flags & 0x0008) != 0) {
            a = d = fixed(cursor);
            cursor += 2;
        } else if ((flags & 0x0040) != 0) {
            a = fixed(cursor);
            d = fixed(cursor + 2);
            cursor += 4;
        } else if ((flags & 0x0080) != 0) {
            a = fixed(cursor);
            b = fixed(cursor + 2);
            c = fixed(cursor + 4);
            d = fixed(cursor + 6);
            cursor += 8;
        }

        if (cursor > end) throw Trouble{Kind::malformed, "A composite glyph cut short."};

        std::vector<Contour> parts;

        gathered(part, depth + 1, parts, points);

        for (Contour& contour : parts)
            for (Dot& dot : contour) dot = Dot{a * dot.x + c * dot.y, b * dot.x + d * dot.y, dot.on};

        double dx = first;
        double dy = second;

        if (!offsets) {
            const auto nth = [](const std::vector<Contour>& all, std::size_t index) -> std::optional<Dot> {
                for (const Contour& contour : all) {
                    if (index < contour.size()) return contour[index];

                    index -= contour.size();
                }

                return std::nullopt;
            };
            const std::optional<Dot> theirs = nth(into, static_cast<std::size_t>(first));
            const std::optional<Dot> ours = nth(parts, static_cast<std::size_t>(second));

            if (!theirs || !ours) throw Trouble{Kind::malformed, "A composite glyph matching points it does not have."};

            dx = theirs->x - ours->x;
            dy = theirs->y - ours->y;
        } else if ((flags & 0x0800) != 0) {
            const double tx = dx;

            dx = a * tx + c * dy;
            dy = b * tx + d * dy;
        }

        for (Contour& contour : parts) {
            for (Dot& dot : contour) {
                dot.x += dx;
                dot.y += dy;
            }

            into.push_back(std::move(contour));
        }

        more = (flags & 0x0020) != 0;
    }
}

// A font around what it holds.
Font::Font(std::shared_ptr<const Data> data) : data_(std::move(data)) {}

// The font in `file` (TrueType, or the first font of a collection). Throws Trouble malformed for a
// font broken where it starts, Trouble unsupported for outlines in CFF and Trouble full for a file
// over 64 MB.
Font Font::read(Bytes file) {
    if (file.size() > most_file) throw Trouble{Kind::full, "A font file over 64 MB."};

    auto data = std::make_shared<Data>();

    data->file = std::move(file);

    const Reading read = data->reading();
    std::size_t start = 0;
    const std::uint32_t version = read.u32(0);

    if (version == 0x74746366) {
        if (read.u32(8) == 0) throw Trouble{Kind::malformed, "A font collection of no fonts."};

        start = read.u32(12);
    }

    const std::uint32_t kind = read.u32(start);

    if (kind == 0x4F54544F) throw Trouble{Kind::unsupported, "A font of CFF outlines, which tal does not read."};

    if (kind != 0x00010000 && kind != 0x74727565) throw Trouble{Kind::malformed, "Not a TrueType font."};

    const std::map<std::string, Table> tables = tabled(read, start);
    const auto table = [&](const char* tag) {
        const auto found = tables.find(tag);

        if (found == tables.end()) throw Trouble{Kind::malformed, std::string{"A font without its table "} + tag + "."};

        return found->second;
    };
    const Table head = table("head");
    const Table hhea = table("hhea");
    const Table maxp = table("maxp");

    data->units = read.u16(head.at + 18);

    if (data->units < 16 || data->units > 16384) throw Trouble{Kind::malformed, "A font of an impossible number of units to the em."};

    data->long_offsets = read.i16(head.at + 50) == 1;
    data->glyphs = read.u16(maxp.at + 4);
    data->ascender = read.i16(hhea.at + 4);
    data->descender = read.i16(hhea.at + 6);
    data->metrics = read.u16(hhea.at + 34);
    data->loca = table("loca");
    data->glyf = table("glyf");
    data->hmtx = table("hmtx");

    if (data->glyphs == 0 || data->metrics == 0 || data->metrics > data->glyphs) throw Trouble{Kind::malformed, "A font of no glyphs or of more metrics than glyphs."};

    if (data->hmtx.length < 4 * std::size_t{data->metrics}) throw Trouble{Kind::malformed, "A font whose metrics are cut short."};

    if (data->loca.length < (std::size_t{data->glyphs} + 1) * (data->long_offsets ? 4 : 2)) throw Trouble{Kind::malformed, "A font whose glyph locations are cut short."};

    const auto [map, format] = mapped(read, table("cmap"));

    data->map = map;
    data->format = format;

    if (const auto found = tables.find("name"); found != tables.end()) {
        try {
            data->name = named(read, found->second);
        } catch (const Trouble&) {
        }
    }

    if (const auto found = tables.find("GPOS"); found != tables.end()) {
        try {
            data->pairs = kerns(read, found->second);
        } catch (const Trouble&) {
            data->pairs.clear();
        }
    }

    if (const auto found = tables.find("kern"); data->pairs.empty() && found != tables.end()) {
        try {
            data->kerned = kerned(read, found->second);
        } catch (const Trouble&) {
            data->kerned.clear();
        }
    }

    return Font{std::move(data)};
}

// The font's full name, as its name table gives it; empty when it gives none.
const std::string& Font::name() const { return data_->name; }

// Whether the font has a glyph of its own for `code`.
bool Font::covers(char32_t code) const { return data_->glyph(code) != 0; }

namespace {

// The glyphs of `utf8` for the font of `data`, control characters as spaces and broken UTF-8 as
// U+FFFD.
std::vector<std::uint32_t> glyphed(const Font::Data& data, std::string_view utf8) {
    std::vector<std::uint32_t> made;

    for (std::size_t at = 0; at < utf8.size();) {
        const std::optional<char32_t> code = text::decoded(utf8, at);
        char32_t one = code.value_or(char32_t{0xFFFD});

        if (one < 0x20 || one == 0x7F) one = U' ';

        made.push_back(data.glyph(one));
    }

    return made;
}

// The places of `glyphs` along the line (font units from its start) and its whole advance.
std::pair<std::vector<double>, double> laid(const Font::Data& data, const std::vector<std::uint32_t>& glyphs) {
    std::vector<double> places;
    double pen = 0;

    places.reserve(glyphs.size());

    for (std::size_t at = 0; at < glyphs.size(); ++at) {
        if (at > 0) pen += data.kerning(glyphs[at - 1], glyphs[at]);

        places.push_back(pen);
        pen += data.advance(glyphs[at]);
    }

    return {std::move(places), pen};
}

// Throws Trouble malformed when `size` is negative or any number is not finite.
void checked(double size, std::initializer_list<double> numbers) {
    for (const double one : numbers)
        if (!std::isfinite(one)) throw Trouble{Kind::malformed, "A size or point of text that is not a finite number."};

    if (size < 0) throw Trouble{Kind::malformed, "Text of a negative size."};
}

}

// The outline of `text` at `size` (the path's units per em), its start, middle or end at `at` by
// `align`, its baseline or the top, middle or bottom of its line there by `baseline`.
Path Font::outlined(std::string_view text, double size, Point at, Align align, Baseline baseline) const {
    checked(size, {size, at.x, at.y});

    const Data& data = *data_;
    const std::vector<std::uint32_t> glyphs = glyphed(data, text);
    const auto [places, whole] = laid(data, glyphs);
    const double scale = size / data.units;
    double x = at.x;
    double y = at.y;

    if (align == Align::center) x -= whole * scale / 2;

    if (align == Align::right) x -= whole * scale;

    if (baseline == Baseline::top) y += data.ascender * scale;

    if (baseline == Baseline::bottom) y += data.descender * scale;

    if (baseline == Baseline::middle) y += (data.ascender + data.descender) * scale / 2;

    Path path;

    for (std::size_t one = 0; one < glyphs.size(); ++one) {
        const Matrix place{scale, 0, 0, -scale, x + places[one] * scale, y};

        for (const Contour& contour : data.contours(glyphs[one])) traced(contour, place, path);
    }

    return path;
}

// The width of `text` at `size` and the font's ascent and descent there.
Measure Font::measured(std::string_view text, double size) const {
    checked(size, {size});

    const Data& data = *data_;
    const double scale = size / data.units;

    return Measure{laid(data, glyphed(data, text)).second * scale, data.ascender * scale, -data.descender * scale};
}

}
