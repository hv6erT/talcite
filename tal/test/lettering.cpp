#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "tal/drawing.hpp"
#include "tal/fonts.hpp"
#include "tal/pixels.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

// Text in pixels (E7b): DejaVu Sans built in (its name, what it covers, advances and kerning by
// GPOS, the outline of I as its four corners, an accent made of parts), alignments and baselines,
// control characters, broken UTF-8 and characters without glyphs; a small font made here for the
// rest (contours of points off the curve, composites scaled and placed by matching points, a glyph
// nesting itself, nested too deep or cut short, maps of format 4 and 12, the old kern table), what
// is no font, and text filled on a raster.

namespace {

using tal::Bytes;
using tal::Kind;
using tal::drawing::Align;
using tal::drawing::Baseline;
using tal::drawing::Font;
using tal::drawing::Path;
using tal::drawing::Point;

// The kind of the trouble `work` throws, none when it throws nothing.
std::optional<Kind> trouble_of(const std::function<void()>& work) {
    try {
        work();
    } catch (const tal::Trouble& why) {
        return why.kind();
    }

    return std::nullopt;
}

// Bytes written big-endian.
struct Written {
    std::string bytes;

    // `value` as one byte.
    Written& u8(std::uint64_t value) {
        bytes += static_cast<char>(static_cast<unsigned char>(value & 0xFF));

        return *this;
    }

    // `value` as two bytes.
    Written& u16(std::uint64_t value) { return u8(value >> 8).u8(value); }

    // `value` as four bytes.
    Written& u32(std::uint64_t value) { return u16(value >> 16).u16(value); }

    // `text` as it is.
    Written& raw(const std::string& text) {
        bytes += text;

        return *this;
    }
};

// A simple glyph of `contours`, each a list of points (x, y, on the curve).
std::string simple(const std::vector<std::vector<std::array<int, 3>>>& contours) {
    Written made;
    std::size_t count = 0;

    made.u16(contours.size()).u16(0).u16(0).u16(0).u16(0);

    for (const auto& contour : contours) {
        count += contour.size();
        made.u16(count - 1);
    }

    made.u16(0);

    for (const auto& contour : contours)
        for (const auto& point : contour) made.u8(point[2] != 0 ? 1 : 0);

    int x = 0;

    for (const auto& contour : contours)
        for (const auto& point : contour) {
            made.u16(static_cast<std::uint16_t>(point[0] - x));
            x = point[0];
        }

    int y = 0;

    for (const auto& contour : contours)
        for (const auto& point : contour) {
            made.u16(static_cast<std::uint16_t>(point[1] - y));
            y = point[1];
        }

    return made.bytes;
}

// The tables of a small font, in their order: head, hhea, maxp, cmap, loca, glyf, hmtx, kern. Its
// glyphs: 0 .notdef, 1 'a' a rectangle, 2 'b' a contour of points off the curve and one of both,
// 3 'c' 'a' at half its size moved 50 right, 4 'd' a glyph made of itself, 5 'e' cut short, 6 'f'
// two 'a' joined by matching points, 7 to 18 each the one before it ('a' for 7), of which 10 is
// 'x' (four deep) and 18 'y' (twelve deep); 'a' kerned before 'b' by -100. Its map of characters
// is of format 4, or of format 12 when `twelve`.
std::vector<std::pair<std::string, std::string>> tables(bool unicode, bool twelve = false) {
    std::vector<std::string> glyphs;

    glyphs.push_back(simple({{{0, 0, 1}, {0, 500, 1}, {500, 500, 1}, {500, 0, 1}}}));
    glyphs.push_back(simple({{{100, 0, 1}, {400, 0, 1}, {400, 700, 1}, {100, 700, 1}}}));
    glyphs.push_back(simple({{{250, 0, 0}, {500, 250, 0}, {250, 500, 0}, {0, 250, 0}}, {{600, 0, 1}, {700, 100, 0}, {800, 0, 0}, {900, 100, 1}}}));
    glyphs.push_back(Written{}.u16(0xFFFF).u16(0).u16(0).u16(0).u16(0).u16(0x0001 | 0x0002 | 0x0008).u16(1).u16(50).u16(0).u16(8192).bytes);
    glyphs.push_back(Written{}.u16(0xFFFF).u16(0).u16(0).u16(0).u16(0).u16(0x0001 | 0x0002).u16(4).u16(0).u16(0).bytes);
    glyphs.push_back(Written{}.u16(1).u16(0).u16(0).u16(0).u16(0).u16(9).bytes);
    glyphs.push_back(Written{}.u16(0xFFFF).u16(0).u16(0).u16(0).u16(0).u16(0x0001 | 0x0002 | 0x0020).u16(1).u16(0).u16(0).u16(0x0001).u16(1).u16(2).u16(0).bytes);

    for (std::uint16_t one = 7; one <= 18; ++one) glyphs.push_back(Written{}.u16(0xFFFF).u16(0).u16(0).u16(0).u16(0).u16(0x0001 | 0x0002).u16(one == 7 ? 1 : one - 1).u16(0).u16(0).bytes);

    Written glyf;
    Written loca;

    for (std::size_t one = 0; one < glyphs.size(); ++one) {
        loca.u32(glyf.bytes.size());
        glyf.raw(glyphs[one]);

        if (glyf.bytes.size() % 2 == 1) glyf.u8(0);
    }

    loca.u32(glyf.bytes.size());

    Written head;

    head.u32(0x00010000).u32(0).u32(0).u32(0x5F0F3CF5).u16(0).u16(1000).u32(0).u32(0).u32(0).u32(0).u16(0).u16(0).u16(0).u16(0).u16(0).u16(0).u16(0).u16(1).u16(0);

    Written hhea;

    hhea.u32(0x00010000).u16(800).u16(static_cast<std::uint16_t>(-200)).u16(0).u16(500);

    for (int zero = 0; zero < 11; ++zero) hhea.u16(0);

    hhea.u16(19);

    Written maxp;

    maxp.u32(0x00005000).u16(19);

    Written cmap;

    if (twelve) {
        cmap.u16(0).u16(1).u16(3).u16(10).u32(12);
        cmap.u16(12).u16(0).u32(16 + 12 * 3).u32(0).u32(3);
        cmap.u32('a').u32('f').u32(1).u32('x').u32('x').u32(10).u32('y').u32('y').u32(18);
    } else {
        cmap.u16(0).u16(1).u16(unicode ? 3 : 1).u16(unicode ? 1 : 0).u32(12);
        cmap.u16(4).u16(16 + 4 * 4 * 2).u16(0).u16(8).u16(0).u16(0).u16(0);
        cmap.u16('f').u16('x').u16('y').u16(0xFFFF).u16(0).u16('a').u16('x').u16('y').u16(0xFFFF);
        cmap.u16(static_cast<std::uint16_t>(1 - 'a')).u16(static_cast<std::uint16_t>(10 - 'x')).u16(static_cast<std::uint16_t>(18 - 'y')).u16(1).u16(0).u16(0).u16(0).u16(0);
    }

    Written hmtx;

    for (int one = 0; one < 19; ++one) hmtx.u16(500).u16(0);

    Written kern;

    kern.u16(0).u16(1).u16(0).u16(20).u16(0x0001).u16(1).u16(0).u16(0).u16(0).u16(1).u16(2).u16(static_cast<std::uint16_t>(-100));

    return {{"head", head.bytes}, {"hhea", hhea.bytes}, {"maxp", maxp.bytes}, {"cmap", cmap.bytes}, {"loca", loca.bytes}, {"glyf", glyf.bytes}, {"hmtx", hmtx.bytes}, {"kern", kern.bytes}};
}

// The small font as a file of `kind` (TrueType, or another tag), its tables placed after `base`
// bytes of something else, its map of characters of format 12 when `twelve`.
std::string font_file(std::size_t base = 0, std::uint32_t kind = 0x00010000, bool unicode = true, bool twelve = false) {
    const auto all = tables(unicode, twelve);
    Written made;
    std::size_t at = base + 12 + 16 * all.size();

    made.u32(kind).u16(all.size()).u16(0).u16(0).u16(0);

    for (const auto& [tag, body] : all) {
        made.raw(tag).u32(0).u32(at).u32(body.size());
        at += body.size() + (4 - body.size() % 4) % 4;
    }

    for (const auto& [tag, body] : all) {
        made.raw(body);
        made.bytes.append((4 - body.size() % 4) % 4, '\0');
    }

    return made.bytes;
}

// `text` as bytes.
Bytes bytes_of(const std::string& text) { return Bytes{reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data()) + text.size()}; }

// The kinds of the pieces of `path`.
std::vector<Path::Kind> kinds(const Path& path) {
    std::vector<Path::Kind> made;

    for (const Path::Piece& one : path.pieces()) made.push_back(one.kind);

    return made;
}

// The points the pieces of `path` end at, in order (a move's and a line's point, a curve's end).
std::vector<Point> ends(const Path& path) {
    std::vector<Point> made;

    for (const Path::Piece& one : path.pieces()) {
        if (one.kind == Path::Kind::move || one.kind == Path::Kind::line) made.push_back(one.points[0]);

        if (one.kind == Path::Kind::quad) made.push_back(one.points[1]);
    }

    return made;
}

}

TRIAL("Fonts: DejaVu Sans built in: its name, what it covers, advances and kerning, the outline of I as its corners, an accent of parts") {
    const Font font = Font::builtin();
    using K = Path::Kind;

    SAME(font.name(), std::string{"DejaVu Sans"});
    OUGHT(font.covers(U'A') && font.covers(U'ż') && font.covers(U'∫') && font.covers(U'�'));
    OUGHT(!font.covers(U'\U0010FFFD'));
    SAME(font.measured("A", 2048).wide, 1401.0);
    SAME(font.measured("A", 2048).ascent, 1901.0);
    SAME(font.measured("A", 2048).descent, 483.0);

    const double pair = font.measured("AV", 2048).wide;

    OUGHT(pair < 2802 && pair > 2802 - 400);
    SAME(font.measured("", 12).wide, 0.0);

    const Path stem = font.outlined("I", 2048, Point{0, 0});

    SAME(kinds(stem), (std::vector<K>{K::move, K::line, K::line, K::line, K::line, K::close}));
    SAME(ends(stem), (std::vector<Point>{{201, -1493}, {403, -1493}, {403, 0}, {201, 0}, {201, -1493}}));

    std::size_t plain = 0;
    std::size_t accented = 0;

    const Path e = font.outlined("e", 100, Point{});
    const Path eacute = font.outlined("\xC3\xA9", 100, Point{});

    for (const Path::Piece& one : e.pieces()) plain += one.kind == K::move ? 1 : 0;

    for (const Path::Piece& one : eacute.pieces()) accented += one.kind == K::move ? 1 : 0;

    SAME(plain, std::size_t{2});
    SAME(accented, std::size_t{3});
}

TRIAL("Fonts: alignments and baselines move the outline; control characters are spaces, broken UTF-8 is U+FFFD, a character without a glyph is .notdef") {
    const Font font = Font::builtin();
    const double wide = font.measured("Ab", 100).wide;
    const std::vector<Point> left = ends(font.outlined("Ab", 100, Point{10, 50}));
    const std::vector<Point> center = ends(font.outlined("Ab", 100, Point{10, 50}, Align::center));
    const std::vector<Point> right = ends(font.outlined("Ab", 100, Point{10, 50}, Align::right));
    const std::vector<Point> top = ends(font.outlined("Ab", 100, Point{10, 50}, Align::left, Baseline::top));
    const std::vector<Point> bottom = ends(font.outlined("Ab", 100, Point{10, 50}, Align::left, Baseline::bottom));
    const std::vector<Point> middle = ends(font.outlined("Ab", 100, Point{10, 50}, Align::left, Baseline::middle));
    const double ascent = font.measured("Ab", 100).ascent;
    const double descent = font.measured("Ab", 100).descent;
    const auto near = [](double one, double other) { return std::abs(one - other) < 1e-9; };

    SAME(center.size(), left.size());

    for (std::size_t at = 0; at < left.size(); ++at) {
        OUGHT(near(center[at].x, left[at].x - wide / 2) && near(center[at].y, left[at].y));
        OUGHT(near(right[at].x, left[at].x - wide) && near(right[at].y, left[at].y));
        OUGHT(near(top[at].y, left[at].y + ascent) && near(top[at].x, left[at].x));
        OUGHT(near(bottom[at].y, left[at].y - descent));
        OUGHT(near(middle[at].y, left[at].y + (ascent - descent) / 2));
    }

    SAME(ends(font.outlined("A\nB", 30, Point{})), ends(font.outlined("A B", 30, Point{})));
    SAME(ends(font.outlined("\xFF", 30, Point{})), ends(font.outlined("\xEF\xBF\xBD", 30, Point{})));
    SAME(ends(font.outlined("\U0010FFFD", 30, Point{})), ends(font.outlined("\U0010FFFC", 30, Point{})));
    OUGHT(!font.outlined("\U0010FFFD", 30, Point{}).empty());
    OUGHT(font.outlined(" ", 30, Point{}).empty());
    SAME(trouble_of([&] { (void)font.outlined("A", -1, Point{}); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([&] { (void)font.outlined("A", 10, Point{std::nan(""), 0}); }), std::optional<Kind>{Kind::malformed});
}

TRIAL("Fonts: a small font: contours of points off the curve, composites scaled and placed by matching points, a glyph of itself, nested too deep or cut short draws nothing, maps of format 4 and 12, the old kern table") {
    const Font font = Font::read(bytes_of(font_file()));
    using K = Path::Kind;

    SAME(font.name(), std::string{});
    OUGHT(font.covers(U'a') && font.covers(U'f') && !font.covers(U'g'));
    SAME(font.measured("ab", 1000).wide, 900.0);
    SAME(font.measured("ba", 1000).wide, 1000.0);
    SAME(font.measured("a", 1000).ascent, 800.0);
    SAME(font.measured("a", 1000).descent, 200.0);
    SAME(ends(font.outlined("a", 1000, Point{})), (std::vector<Point>{{100, 0}, {400, 0}, {400, -700}, {100, -700}, {100, 0}}));

    const Path b = font.outlined("b", 1000, Point{});

    SAME(kinds(b), (std::vector<K>{K::move, K::quad, K::quad, K::quad, K::quad, K::close, K::move, K::quad, K::quad, K::line, K::close}));
    SAME(ends(b), (std::vector<Point>{{375, -125}, {375, -375}, {125, -375}, {125, -125}, {375, -125}, {600, 0}, {750, -50}, {900, -100}, {600, 0}}));
    SAME(b.pieces()[1].points[0], (Point{500, -250}));
    SAME(b.pieces()[4].points[0], (Point{250, 0}));
    SAME(ends(font.outlined("c", 1000, Point{})), (std::vector<Point>{{100, 0}, {250, 0}, {250, -350}, {100, -350}, {100, 0}}));
    OUGHT(font.outlined("d", 1000, Point{}).empty());
    OUGHT(font.outlined("e", 1000, Point{}).empty());
    SAME(ends(font.outlined("x", 1000, Point{})), ends(font.outlined("a", 1000, Point{})));
    OUGHT(font.outlined("y", 1000, Point{}).empty());

    const Font twelve = Font::read(bytes_of(font_file(0, 0x00010000, true, true)));

    OUGHT(twelve.covers(U'a') && twelve.covers(U'y') && !twelve.covers(U'g'));
    SAME(ends(twelve.outlined("xa", 1000, Point{})), ends(font.outlined("xa", 1000, Point{})));
    SAME(ends(font.outlined("f", 1000, Point{})), (std::vector<Point>{{100, 0}, {400, 0}, {400, -700}, {100, -700}, {100, 0}, {400, -700}, {700, -700}, {700, -1400}, {400, -1400}, {400, -700}}));
    SAME(ends(font.outlined("g", 1000, Point{})), (std::vector<Point>{{0, 0}, {0, -500}, {500, -500}, {500, 0}, {0, 0}}));
    SAME(ends(font.outlined("a", 1000, Point{})), ends(Font::read(bytes_of(std::string("ttcf") + std::string{"\0\1\0\0\0\0\0\1\0\0\0\x10", 12} + font_file(16))).outlined("a", 1000, Point{})));
}

TRIAL("Fonts: what is no font: nothing, a font cut short, outlines in CFF, no Unicode map, over 64 MB") {
    const std::string whole = font_file();

    SAME(trouble_of([] { (void)Font::read(Bytes{}); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([&] { (void)Font::read(bytes_of(whole.substr(0, 100))); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([&] { (void)Font::read(bytes_of(whole.substr(0, 12))); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([&] { (void)Font::read(bytes_of(font_file(0, 0x4F54544F))); }), std::optional<Kind>{Kind::unsupported});
    SAME(trouble_of([&] { (void)Font::read(bytes_of(font_file(0, 0x12345678))); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([&] { (void)Font::read(bytes_of(font_file(0, 0x00010000, false))); }), std::optional<Kind>{Kind::malformed});
    SAME(trouble_of([] { (void)Font::read(Bytes((std::size_t{64} << 20) + 1)); }), std::optional<Kind>{Kind::full});

    for (std::size_t cut = 0; cut < whole.size(); cut += 7) {
        try {
            const Font some = Font::read(bytes_of(whole.substr(0, cut)));

            (void)some.outlined("abcdefg", 100, Point{});
            (void)some.measured("abcdefg", 100);
        } catch (const tal::Trouble& why) {
            OUGHT(why.kind() == Kind::malformed);
        }
    }
}

TRIAL("Fonts: text filled on a raster covers its stems and leaves its counters") {
    tal::drawing::Raster raster{tal::media::Size{40, 30}, tal::drawing::Blending::srgb};
    tal::drawing::Pen pen{raster};

    pen.fill(Font::builtin().outlined("O", 28, Point{20, 15}, Align::center, Baseline::middle), tal::drawing::Color{1, 1, 1, 1});

    const tal::media::Pixels seen = raster.pixels();
    const auto alpha = [&](int x, int y) { return std::to_integer<int>(seen.stills().front().rgba[(static_cast<std::size_t>(y) * 40 + static_cast<std::size_t>(x)) * 4 + 3]); };

    SAME(alpha(20, 15), 0);
    OUGHT(alpha(11, 15) > 200 || alpha(12, 15) > 200);
    OUGHT(alpha(28, 15) > 200 || alpha(27, 15) > 200);
    SAME(alpha(0, 0), 0);
}
