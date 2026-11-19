#include <cstdint>
#include <optional>
#include <stdexcept>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tal/geometry.hpp"
#include "tal/style.hpp"
#include "tal/surface.hpp"
#include "tal/text.hpp"

#include "tal/testing.hpp"

// The surface of tal::ui: cells holding a cluster with its look, a picture and a link, canvases
// that clip and move the origin, wide characters that are never left in half, and the rectangles
// of the cells that changed.

namespace {

using tal::Area;
using tal::Canvas;
using tal::Cell;
using tal::Color;
using tal::Lines;
using tal::Look;
using tal::Size;
using tal::Spot;
using tal::Surface;

constexpr Color red{128, 0, 0};
constexpr Color blue{0, 0, 128};

// The letters of row `y`, a continuation of a wide character as nothing, so that "日" reads as
// itself.
std::string row(const Surface& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) made += one.at({x, y}).cluster;

    return made;
}

// The rectangles of the cells that changed, in order.
std::vector<Area> damage(const Surface& one) { return {one.damaged().begin(), one.damaged().end()}; }

// A look with only the ink `ink`.
Look inked(Color ink) {
    Look made;

    made.ink = ink;

    return made;
}

TRIAL("Surface: a new surface is blank, without colors, and nothing has changed on it") {
    const Surface one{{3, 2}};

    SAME(one.size(), (Size{3, 2}));
    SAME(one.at({2, 1}), Cell{});
    SAME(one.at({0, 0}).cluster, std::string{" "});
    OUGHT(one.damaged().empty());
    SAME((Surface{{-4, 2}}.size()), (Size{0, 2}));
    THREW((void)one.at({3, 0}));
    THREW((void)one.at({0, -1}));
}

TRIAL("Surface: put writes cluster by cluster and returns the columns the text takes") {
    Surface one{{10, 1}};
    Canvas all = one.canvas({{0, 0}, {10, 1}});

    SAME(all.put({1, 0}, "ab\u65e5e\u0301", inked(red)), 5);
    SAME(row(one, 0), std::string{" ab\u65e5e\u0301    "});
    SAME(one.at({3, 0}).cluster, std::string{"\u65e5"});
    SAME(one.at({4, 0}).cluster, std::string{});
    SAME(one.at({4, 0}).look, inked(red));
    SAME(one.at({5, 0}).cluster, std::string{"e\u0301"});
    SAME(damage(one), (std::vector<Area>{{{1, 0}, {5, 1}}}));
}

TRIAL("Surface: a control shows as its picture, a lone mark on a dotted circle, a lone format character not at all") {
    Surface one{{5, 1}};
    Canvas all = one.canvas({{0, 0}, {5, 1}});
    std::string bell;

    tal::text::encoded(bell, tal::text::shown(U'\a'));

    SAME(all.put({0, 0}, "\a\u0301\u200bx", Look{}), 3);
    SAME(one.at({0, 0}).cluster, bell);
    OUGHT(bell != "\a");
    SAME(one.at({1, 0}).cluster, std::string{"\u25cc\u0301"});
    SAME(one.at({2, 0}).cluster, std::string{"x"});
}

TRIAL("Surface: writing over half of a wide character leaves no half behind") {
    Surface one{{6, 1}};
    Canvas all = one.canvas({{0, 0}, {6, 1}});

    (void)all.put({0, 0}, "\u65e5\u672c\u8a9e", inked(red));
    one.forget();

    (void)all.put({1, 0}, "x", Look{});
    SAME(row(one, 0), std::string{" x\u672c\u8a9e"});
    SAME(one.at({0, 0}).look, inked(red));
    SAME(damage(one), (std::vector<Area>{{{0, 0}, {2, 1}}}));

    one.forget();

    (void)all.put({2, 0}, "y", Look{});
    SAME(row(one, 0), std::string{" xy \u8a9e"});
    SAME(damage(one), (std::vector<Area>{{{2, 0}, {2, 1}}}));

    (void)all.put({3, 0}, "\u65e5", Look{});
    SAME(row(one, 0), std::string{" xy\u65e5 "});
}

TRIAL("Surface: a wide character with one column of the clip left becomes a space") {
    Surface one{{6, 1}};
    Canvas part = one.canvas({{1, 0}, {3, 1}});

    SAME(part.put({1, 0}, "\u65e5\u65e5", inked(blue)), 4);
    SAME(row(one, 0), std::string{"  \u65e5  "});
    SAME(part.put({-1, 0}, "\u65e5x", Look{}), 3);
    SAME(row(one, 0), std::string{"  x   "});
    SAME(one.at({1, 0}).cluster, std::string{" "});
    SAME(one.at({2, 0}).cluster, std::string{"x"});
    SAME(one.at({3, 0}).cluster, std::string{" "});

    (void)part.put({0, 0}, "abc", Look{});
    one.forget();

    SAME(part.put({2, 0}, "\u65e5", Look{}), 2);
    SAME(row(one, 0), std::string{" ab   "});
    SAME(damage(one), (std::vector<Area>{{{3, 0}, {1, 1}}}));
}

TRIAL("Surface: a canvas draws only inside itself and the surface, in its own coordinates") {
    Surface one{{5, 3}};
    Canvas edge = one.canvas({{3, 1}, {4, 4}});
    Canvas inner = edge.within({{-1, -1}, {2, 2}});

    SAME(edge.size(), (Size{4, 4}));
    SAME(edge.put({0, 0}, "abcd", Look{}), 4);
    SAME(edge.put({0, 5}, "zz", Look{}), 2);
    SAME(row(one, 1), std::string{"   ab"});
    SAME(row(one, 2), std::string{"     "});

    SAME(inner.size(), (Size{2, 2}));
    (void)inner.put({0, 1}, "pq", Look{});
    SAME(row(one, 1), std::string{"   qb"});
    (void)inner.put({0, 0}, "rs", Look{});
    SAME(row(one, 0), std::string{"     "});
}

TRIAL("Surface: a look without colors keeps the colors of the cell, its attributes are its own") {
    Surface one{{4, 1}};
    Canvas all = one.canvas({{0, 0}, {4, 1}});
    Look paper;

    paper.paper = blue;
    paper.bold = true;

    all.fill({{0, 0}, {4, 1}}, paper);

    Look text = inked(red);

    text.under = true;

    (void)all.put({1, 0}, "ab", text);

    SAME(one.at({1, 0}).look.paper, std::optional<Color>{blue});
    SAME(one.at({1, 0}).look.ink, std::optional<Color>{red});
    OUGHT(one.at({1, 0}).look.under);
    OUGHT(!one.at({1, 0}).look.bold);
    OUGHT(one.at({0, 0}).look.bold);
}

TRIAL("Surface: fill clears the area and mends the wide characters at its edges") {
    Surface one{{6, 2}};
    Canvas all = one.canvas({{0, 0}, {6, 2}});

    (void)all.put({0, 0}, "\u65e5\u672c\u8a9e", Look{});
    (void)all.put({0, 1}, "abcdef", Look{});
    one.forget();

    all.fill({{1, 0}, {2, 2}}, inked(red));

    SAME(row(one, 0), std::string{"    \u8a9e"});
    SAME(row(one, 1), std::string{"a  def"});
    SAME(damage(one), (std::vector<Area>{{{0, 0}, {4, 1}}, {{1, 1}, {2, 1}}}));
}

TRIAL("Surface: only cells that really change are damaged, and rectangles that make one are joined") {
    Surface one{{20, 10}};
    Canvas all = one.canvas({{0, 0}, {20, 10}});

    (void)all.put({0, 0}, "    ", Look{});
    OUGHT(one.damaged().empty());

    (void)all.put({2, 1}, "ab", Look{});
    (void)all.put({2, 2}, "cd", Look{});
    (void)all.put({4, 1}, "ef", Look{});
    SAME(damage(one), (std::vector<Area>{{{2, 1}, {2, 2}}, {{4, 1}, {2, 1}}}));

    (void)all.put({4, 2}, "gh", Look{});
    SAME(damage(one), (std::vector<Area>{{{2, 1}, {4, 2}}}));

    (void)all.put({3, 1}, "X", Look{});
    SAME(damage(one), (std::vector<Area>{{{2, 1}, {4, 2}}}));

    one.forget();
    OUGHT(one.damaged().empty());

    (void)all.put({5, 5}, "z", Look{});
    all.fill({{4, 4}, {3, 3}}, inked(red));
    SAME(damage(one), (std::vector<Area>{{{4, 4}, {3, 3}}}));
}

TRIAL("Surface: past sixty-four rectangles the damage becomes the one that holds them") {
    Surface one{{200, 3}};
    Canvas all = one.canvas({{0, 0}, {200, 3}});

    for (int at = 0; at < 64; ++at) (void)all.put({at * 3, 1}, "x", Look{});

    SAME(damage(one).size(), std::size_t{64});

    (void)all.put({199, 2}, "y", Look{});

    SAME(damage(one), (std::vector<Area>{{{0, 1}, {200, 2}}}));
}

TRIAL("Surface: a link numbers its address, which goes when no cell holds it") {
    Surface one{{8, 1}};
    Canvas all = one.canvas({{0, 0}, {8, 1}});

    (void)all.put({0, 0}, "go here", Look{});
    one.forget();

    all.link({{3, 0}, {4, 1}}, "https://example.org/a");

    const std::uint32_t first = one.at({3, 0}).link;

    OUGHT(first != 0);
    SAME(one.at({6, 0}).link, first);
    SAME(one.at({2, 0}).link, std::uint32_t{0});
    SAME(one.address(first), std::string_view{"https://example.org/a"});
    SAME(damage(one), (std::vector<Area>{{{3, 0}, {4, 1}}}));

    all.link({{0, 0}, {2, 1}}, "https://example.org/a");
    SAME(one.at({0, 0}).link, first);

    (void)all.put({3, 0}, "here", Look{});
    SAME(one.address(first), std::string_view{"https://example.org/a"});

    all.link({{0, 0}, {2, 1}}, "");
    SAME(one.at({0, 0}).link, std::uint32_t{0});
    SAME(one.address(first), std::string_view{});

    all.link({{5, 0}, {1, 1}}, "https://example.org/b");
    SAME(one.at({5, 0}).link, first);
    SAME(one.address(first), std::string_view{"https://example.org/b"});

    all.link({{0, 0}, {0, 0}}, "https://example.org/c");
    SAME(one.address(first + 1), std::string_view{});
}

// A picture is held while a cell shows it: writing over its cells takes them away one by one, and
// with the last one the surface forgets it; a shadow takes it too, since pixels cannot be dimmed.
// The same mark given again takes the new picture into every cell showing it.
TRIAL("Surface: a picture marks its cells, is held while one shows it, and goes under a shadow") {
    Surface one{{4, 2}};
    Canvas all = one.canvas({{0, 0}, {4, 2}});
    const auto sixel = std::make_shared<const std::string>("seven");

    all.picture({{1, 0}, {2, 2}}, tal::Picture{.mark = 7, .size = {2, 2}, .sixel = sixel});

    SAME(one.at({1, 1}).picture, std::uint32_t{7});
    SAME(one.at({0, 1}).picture, std::uint32_t{0});
    SAME(damage(one), (std::vector<Area>{{{1, 0}, {2, 2}}}));
    OUGHT(one.picture(7) != nullptr && one.picture(7)->sixel == sixel && one.picture(7)->size == (Size{2, 2}));
    OUGHT(one.picture(0) == nullptr && one.picture(8) == nullptr);

    (void)all.put({2, 1}, "x", Look{});

    SAME(one.at({2, 1}).picture, std::uint32_t{0});
    SAME(one.at({1, 1}).picture, std::uint32_t{7});

    all.picture({{1, 0}, {1, 1}}, tal::Picture{.mark = 7, .size = {2, 2}});

    OUGHT(one.picture(7) != nullptr && one.picture(7)->sixel == nullptr);

    all.fill({{1, 0}, {2, 1}}, Look{});

    OUGHT(one.picture(7) != nullptr);

    all.shade({{0, 1}, {4, 1}}, Look{});

    SAME(one.at({1, 1}).picture, std::uint32_t{0});
    OUGHT(one.picture(7) == nullptr);

    all.picture({{0, 0}, {1, 1}}, tal::Picture{.mark = 9, .size = {1, 1}});
    all.picture({{0, 0}, {1, 1}}, tal::Picture{});

    SAME(one.at({0, 0}).picture, std::uint32_t{0});
    OUGHT(one.picture(9) == nullptr);

    one.canvas({{3, 1}, {1, 1}}).picture({{1, 0}, {1, 1}}, tal::Picture{.mark = 10, .size = {1, 1}});

    OUGHT(one.picture(10) == nullptr);
}

// A shade changes the look of the cells it covers and leaves their letters; only the cells whose
// look changed are damaged, and what lies outside the clip stays.
TRIAL("Surface: a shade gives cells a look and keeps their letters") {
    Surface one{{6, 2}};
    Canvas all = one.canvas({{0, 0}, {6, 2}});

    (void)all.put({0, 0}, "abc", inked(red));
    one.forget();

    Canvas part = one.canvas({{1, 0}, {5, 2}});

    part.shade({{-1, 0}, {3, 1}}, inked(red));

    OUGHT(one.damaged().empty());

    part.shade({{0, 0}, {3, 2}}, Look{});

    SAME(row(one, 0), std::string{"abc   "});
    OUGHT(one.at({0, 0}).look == inked(red));
    OUGHT(one.at({1, 0}).look == Look{});
    OUGHT(one.at({2, 0}).look == Look{});
    SAME(damage(one), (std::vector<Area>{{{1, 0}, {2, 1}}}));
}

TRIAL("Surface: a frame draws its eight glyphs around the edge and leaves the inside") {
    Surface one{{6, 4}};
    Canvas all = one.canvas({{0, 0}, {6, 4}});
    const Lines lines{"1", "2", "3", "4", "-", "|", "<", ">"};

    (void)all.put({2, 1}, "in", Look{});
    all.frame({{0, 0}, {5, 4}}, lines, inked(red));

    SAME(row(one, 0), std::string{"1---2 "});
    SAME(row(one, 1), std::string{"| in| "});
    SAME(row(one, 2), std::string{"|   | "});
    SAME(row(one, 3), std::string{"3---4 "});
    SAME(one.at({4, 2}).look, inked(red));

    one.forget();
    all.frame({{0, 0}, {1, 4}}, lines, Look{});
    OUGHT(one.damaged().empty());

    Canvas half = one.canvas({{0, 0}, {3, 4}});

    half.frame({{1, 1}, {4, 2}}, lines, Look{});
    SAME(row(one, 1), std::string{"|1-n| "});
    SAME(row(one, 2), std::string{"|3- | "});
}

// A clipped frame writes the cells of its edges inside the clip and nothing else, whether the clip
// cuts a band of rows, a band of columns or the corner of a canvas that hangs off the surface.
TRIAL("Surface: a clipped frame writes only the stretches of its edges inside the clip") {
    const Lines lines{"1", "2", "3", "4", "-", "|", "<", ">"};

    Surface rows{{6, 5}};
    Canvas band = rows.canvas({{0, 2}, {6, 1}});

    band.frame({{0, -2}, {5, 5}}, lines, Look{});

    SAME(row(rows, 1), std::string{"      "});
    SAME(row(rows, 2), std::string{"|   | "});
    SAME(row(rows, 3), std::string{"      "});
    SAME(rows.damaged().size(), std::size_t{2});

    Surface columns{{6, 5}};
    Canvas strip = columns.canvas({{2, 0}, {1, 5}});

    strip.frame({{-2, 0}, {5, 5}}, lines, Look{});

    SAME(row(columns, 0), std::string{"  -   "});
    SAME(row(columns, 2), std::string{"      "});
    SAME(row(columns, 4), std::string{"  -   "});

    Surface corner{{4, 4}};
    Canvas hanging = corner.canvas({{-1, -1}, {4, 4}});

    hanging.frame({{0, 0}, {4, 4}}, lines, Look{});

    SAME(row(corner, 0), std::string{"  | "});
    SAME(row(corner, 1), std::string{"  | "});
    SAME(row(corner, 2), std::string{"--4 "});
    SAME(row(corner, 3), std::string{"    "});
}

}
