#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "tal/document.hpp"
#include "tal/executor.hpp"
#include "tal/image.hpp"
#include "tal/pixels.hpp"
#include "tal/scene.hpp"
#include "tal/trouble.hpp"

#include "tal/testing.hpp"

// The widget Image on a scene with a loop: bytes decoded on the pool at the size of its room,
// pixels made in code scaled at once, characters where the terminal shows no pixels, pictures in
// the cells where it does, and animations stepping on the scene's clock.

namespace {

using namespace std::chrono_literals;

using tal::Blocks;
using tal::Bytes;
using tal::Display;
using tal::Document;
using tal::Graphics;
using tal::Image;
using tal::Size;
using tal::media::Format;
using tal::media::Pixels;
using tal::media::Still;
using tal::testing::Scene;

// Bytes from a string of lower-case hexadecimal digits.
Bytes lump(std::string_view hex) {
    Bytes made;

    const auto value = [](char one) { return one >= '0' && one <= '9' ? one - '0' : one - 'a' + 10; };

    for (std::size_t at = 0; at + 1 < hex.size(); at += 2) made.push_back(static_cast<std::byte>(value(hex[at]) * 16 + value(hex[at + 1])));

    return made;
}

// A GIF of 16 by 12 with four frames of 40, 100, 100 and 120 milliseconds (see gifting.cpp).
constexpr std::string_view leaving =
    "47494638396110000c00810000000000ff000000000000000021ff0b4e45545343415045322e300301000000"
    "21f90404040000002c0000000010000c000008220003080c00a0a0c18305071244c85021c386031f4a9c48b1"
    "a2c58b18336adc2831200021f90405000002002c00000000070006008100000000ff00000000000000081900"
    "01080420a0e0408206050658186020c30005053c8cf830200021f90405010002002c03000200070006008100"
    "00000000ff00000000000008190001080420a0e0408206050658186020c30005053c8cf830200021f904050c"
    "0002002c060004000700060081000000ff000000000000000008190001080420a0e0408206050658186020c3"
    "0005053c8cf83020003b";

// The header of a PNG of 40 by 40, which no decoder here reads unless a trial sets one.
constexpr std::string_view png = "89504e470d0a1a0a0000000d49484452000000280000002808060000";

// Opaque pixels of `width` by `height` in one red, with `stills` frames of `after` each.
Pixels square(std::uint32_t width, std::uint32_t height, std::uint8_t red, int stills = 1, std::chrono::milliseconds after = 0ms) {
    Bytes one;

    for (std::uint32_t at = 0; at < width * height; ++at)
        for (const std::uint8_t band : {red, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{255}}) one.push_back(std::byte{band});

    return Pixels{{width, height}, std::vector<Still>(static_cast<std::size_t>(stills), Still{one, after})};
}

// A decoder that gives one frame of 8 by 8 in red, or throws `fails` when it is set.
class Faking : public tal::media::Decoding {
public:
    explicit Faking(std::optional<tal::Trouble> fails = std::nullopt) : fails_(std::move(fails)) {}

    std::string_view name() const override { return "fake"; }

    void decoded(std::span<const std::byte>, const tal::media::Taking& each) override {
        if (fails_) throw *fails_;

        const Pixels made = square(8, 8, 200);

        (void)each(made.stills().front().rgba, made.size(), 0ms);
    }

private:
    std::optional<tal::Trouble> fails_;
};

// Sets a decoder for the trial and takes it away after it.
struct Decoding {
    explicit Decoding(std::shared_ptr<tal::media::Decoding> given) { tal::defaults().set<tal::media::Decoding>(std::move(given)); }

    ~Decoding() { tal::defaults().clear<tal::media::Decoding>(); }

    Decoding(const Decoding&) = delete;
    Decoding& operator=(const Decoding&) = delete;
};

// A terminal of 20 by 6 cells drawing sextants, with `graphics` and a cell of `cell` pixels.
Display shown_as(Graphics graphics = Graphics::none, Size cell = {}) {
    return Display{.screen = {20, 6}, .graphics = graphics, .blocks = Blocks::sextants, .cell = cell};
}

// A scene and a loop with a document holding only an image. The first loop made is the program's,
// on which an image's work comes back, so a trial has one of these at a time.
struct Shown {
    Scene scene;
    tal::Loop loop{scene.time()};
    Document document{scene.time()};
    Image& image = document.children().add<Image>();

    // On a terminal as `given` describes it.
    explicit Shown(Display given = shown_as()) : scene(given) {}

    // Waits for the image's work on the pool to come back before the loop it comes back to goes.
    ~Shown() { UNTIL(loop, !image.busy()); }

    Shown(const Shown&) = delete;
    Shown& operator=(const Shown&) = delete;

    // Shows the document.
    void show() { scene.show(document); }

    // Lays out what changed, lets the pool finish and draws the frames.
    void settled() {
        scene.wait(0ms);
        UNTIL(loop, !image.busy());
        scene.wait(0ms);
    }

    // The columns of row `y` that show something other than a blank.
    std::set<int> drawn(int y) const {
        std::set<int> made;

        for (int x = 0; x < scene.screen().size().wide; ++x)
            if (scene.at({x, y}).cluster != " " || scene.at({x, y}).look.paper.has_value()) made.insert(x);

        return made;
    }

    // The picture marks on row `y`.
    std::set<std::uint32_t> marks(int y) const {
        std::set<std::uint32_t> made;

        for (int x = 0; x < scene.screen().size().wide; ++x)
            if (scene.at({x, y}).picture != 0) made.insert(scene.at({x, y}).picture);

        return made;
    }
};

// The columns from `from` up to `upto`.
std::set<int> columns(int from, int upto) {
    std::set<int> made;

    for (int x = from; x < upto; ++x) made.insert(x);

    return made;
}

}

// --- Holding -------------------------------------------------------------------------------------

TRIAL("Image: an empty one holds nothing, asks for no room and draws nothing") {
    Shown shown;

    shown.show();

    OUGHT(!shown.image.held());
    OUGHT(!shown.image.busy());
    SAME(shown.image.frames(), std::size_t{0});
    SAME(static_cast<int>(shown.image.format()), static_cast<int>(Format::none));
    SAME(shown.image.measured({{0, 0}, {20, 6}}), (Size{0, 0}));
    OUGHT(shown.drawn(2).empty());
}

// Bytes give the format and the size from the header at once, so that layout can ask for room;
// the frames come decoded at the size of the room, and the number of frames with them. Clearing
// lets go of everything.
TRIAL("Image: bytes give the format and size at once, the frames once decoded") {
    Shown shown;

    shown.image.hold(lump(leaving));
    shown.show();

    OUGHT(shown.image.held());
    SAME(static_cast<int>(shown.image.format()), static_cast<int>(Format::gif));
    OUGHT(shown.image.size() == (tal::media::Size{16, 12}));
    OUGHT(shown.image.busy());
    SAME(shown.image.frames(), std::size_t{0});

    shown.settled();

    OUGHT(!shown.image.busy());
    SAME(shown.image.frames(), std::size_t{4});
    SAME(shown.drawn(2), columns(2, 18));

    shown.image.clear();
    shown.scene.wait(0ms);

    OUGHT(!shown.image.held());
    SAME(static_cast<int>(shown.image.format()), static_cast<int>(Format::none));
    OUGHT(shown.drawn(2).empty());
}

// A format with no decoder set leaves the image without a size, its format known; with one, the
// image decodes through it; a decoder's refusal empties the image and leaves the format.
TRIAL("Image: without a decoder a PNG stays empty, with one it decodes, a refusal empties it") {
    {
        Shown shown;

        shown.image.hold(lump(png));
        shown.show();

        OUGHT(!shown.image.held());
        SAME(static_cast<int>(shown.image.format()), static_cast<int>(Format::png));
        OUGHT(!shown.image.busy());
    }

    {
        const Decoding set{std::make_shared<Faking>()};
        Shown shown;

        shown.image.hold(lump(png));
        shown.show();
        shown.settled();

        OUGHT(shown.image.held());
        SAME(shown.image.frames(), std::size_t{1});
        OUGHT(!shown.drawn(2).empty());
    }

    {
        const Decoding set{std::make_shared<Faking>(tal::Trouble{tal::Kind::malformed, "Torn."})};
        Shown shown;

        shown.image.hold(lump(png));
        shown.show();
        shown.settled();

        OUGHT(!shown.image.held());
        SAME(shown.image.frames(), std::size_t{0});
        SAME(static_cast<int>(shown.image.format()), static_cast<int>(Format::png));
        OUGHT(shown.drawn(2).empty());
    }
}

// --- Room ----------------------------------------------------------------------------------------

// The picture keeps its proportions by the cell's: a square in cells of 10 by 20 takes half as
// many rows as columns, in square cells as many, and in a cell of unknown size 1:2 is assumed. It
// stands in the middle of its room, and a room without bounds takes it at its own resolution.
TRIAL("Image: the proportions of the cell, the middle of the room, a room without bounds") {
    for (const auto& [cell, from, upto] : std::vector<std::tuple<Size, int, int>>{{{10, 20}, 4, 16}, {{}, 4, 16}, {{10, 10}, 7, 13}}) {
        Shown shown{shown_as(Graphics::none, cell)};

        shown.image.hold(square(40, 40, 200));
        shown.show();

        SAME(shown.drawn(0), columns(from, upto));
    }

    Shown cells{shown_as(Graphics::none, {10, 20})};
    Shown grain;

    cells.image.hold(square(40, 40, 200));
    grain.image.hold(square(40, 40, 200));
    cells.show();
    grain.show();

    SAME(cells.image.measured({{0, 0}, {tal::endless, tal::endless}}), (Size{4, 2}));
    SAME(grain.image.measured({{0, 0}, {tal::endless, tal::endless}}), (Size{20, 10}));
    SAME(cells.image.measured({{0, 0}, {8, 8}}), (Size{8, 4}));
    SAME(cells.image.measured({{0, 0}, {8, tal::endless}}), (Size{4, 2}));
    SAME(cells.image.measured({{0, 0}, {20, 0}}), (Size{0, 0}));

    // Rounding to the nearest cell both ways, in an odd cell of 9 by 21: 16 by 12 pixels in 21
    // columns would take 6.75 rows, so 7, and in 6 rows 18.7 columns, so all 20 of a room of 20; 12
    // by 16 in 5 rows takes 8.75 columns, so 9. Pixels not filling a cell take it whole, and a cell
    // known only by one side is not known.
    Shown odd{shown_as(Graphics::none, {9, 21})};
    Shown tall{shown_as(Graphics::none, {9, 21})};
    Shown ragged{shown_as(Graphics::none, {10, 20})};
    Shown half{shown_as(Graphics::none, {10, 0})};

    odd.image.hold(square(16, 12, 200));
    tall.image.hold(square(12, 16, 200));
    ragged.image.hold(square(41, 41, 200));
    half.image.hold(square(41, 41, 200));

    for (Shown* one : {&odd, &tall, &ragged, &half}) one->show();

    SAME(odd.image.measured({{0, 0}, {21, 8}}), (Size{21, 7}));
    SAME(odd.image.measured({{0, 0}, {20, 6}}), (Size{20, 6}));
    SAME(tall.image.measured({{0, 0}, {20, 5}}), (Size{9, 5}));
    SAME(ragged.image.measured({{0, 0}, {tal::endless, tal::endless}}), (Size{5, 3}));
    SAME(ragged.image.measured({{0, 0}, {5, tal::endless}}), (Size{5, 3}));
    SAME(half.image.measured({{0, 0}, {tal::endless, tal::endless}}), (Size{21, 11}));
}

// A room of nothing orders nothing; a room changed while the frames are decoded orders them again,
// and only the last order's frames are taken.
TRIAL("Image: a room of nothing orders nothing, a new room orders anew and takes the last") {
    {
        Shown none{Display{.screen = {20, 0}, .blocks = Blocks::sextants}};

        none.image.hold(lump(leaving));
        none.show();
        none.settled();

        OUGHT(!none.image.busy());
        SAME(none.image.frames(), std::size_t{0});
    }

    Shown moved;

    moved.image.hold(lump(leaving));
    moved.show();
    moved.scene.resize({10, 6});
    moved.settled();

    SAME(moved.image.frames(), std::size_t{4});
    SAME(moved.drawn(2), columns(0, 10));
    OUGHT(moved.drawn(0).empty());
}

// --- Animation -----------------------------------------------------------------------------------

// Each frame stands for its own delay on the document's clock; pausing freezes the frame on show,
// and playing again gives it its whole delay; setting a frame seeks, the last one when there are
// fewer.
TRIAL("Image: frames step on the clock, pausing freezes, seeking sets the frame") {
    Shown shown;

    shown.image.hold(lump(leaving));
    shown.show();
    shown.settled();

    SAME(shown.image.still(), std::size_t{0});

    shown.scene.wait(30ms);
    shown.image.play(true);
    shown.scene.wait(10ms);

    SAME(shown.image.still(), std::size_t{1});

    shown.scene.wait(100ms);
    shown.scene.wait(99ms);

    SAME(shown.image.still(), std::size_t{2});

    shown.scene.wait(1ms);

    SAME(shown.image.still(), std::size_t{3});

    shown.scene.wait(120ms);

    SAME(shown.image.still(), std::size_t{0});

    shown.scene.wait(40ms);
    shown.image.play(false);
    shown.scene.wait(1000ms);

    OUGHT(!shown.image.play());
    SAME(shown.image.still(), std::size_t{1});

    shown.image.play(true);
    shown.scene.wait(99ms);

    SAME(shown.image.still(), std::size_t{1});

    shown.scene.wait(1ms);

    SAME(shown.image.still(), std::size_t{2});

    shown.image.still(9);

    SAME(shown.image.still(), std::size_t{3});

    shown.scene.wait(119ms);

    SAME(shown.image.still(), std::size_t{3});

    shown.scene.wait(1ms);

    SAME(shown.image.still(), std::size_t{0});
}

// Frames without delays are no animation: nothing steps, whatever the clock does. A frame laid out
// again, for a change of the terminal that does not touch the picture, keeps its appointment.
TRIAL("Image: frames without delays do not step, and laying out again does not delay the next") {
    Shown shown;

    shown.image.hold(square(8, 8, 200, 3));
    shown.show();
    shown.scene.wait(1000ms);

    SAME(shown.image.frames(), std::size_t{3});
    SAME(shown.image.still(), std::size_t{0});

    shown.image.still(2);

    SAME(shown.image.still(), std::size_t{2});

    Shown moving;

    moving.image.hold(square(8, 8, 200, 2, 100ms));
    moving.show();
    moving.scene.wait(60ms);
    moving.scene.display(Display{.screen = {20, 6}, .rendering = tal::text::Rendering{false, false}, .blocks = Blocks::sextants});
    moving.scene.wait(40ms);

    SAME(moving.image.still(), std::size_t{1});
}

// --- Pictures ------------------------------------------------------------------------------------

// With sixel and a cell of known size the image orders the room's full resolution (12 by 6 cells
// of 10 by 20 pixels: 120 by 120) and, once its sixel is encoded, marks its cells with the
// picture; before that it stands in characters made from the same pixels. A new image is a new
// picture under a new mark.
TRIAL("Image: with sixel the picture marks its cells once encoded, a new one under a new mark") {
    Shown shown{shown_as(Graphics::sixel, {10, 20})};

    shown.image.hold(square(40, 40, 200));
    shown.show();

    OUGHT(shown.marks(0).empty());
    SAME(shown.drawn(0), columns(4, 16));

    shown.settled();

    const std::set<std::uint32_t> first = shown.marks(0);

    SAME(first.size(), std::size_t{1});

    const tal::Picture* const one = shown.scene.screen().picture(*first.begin());

    OUGHT(one != nullptr && one->size == (Size{12, 6}));
    OUGHT(one != nullptr && one->sixel != nullptr && one->sixel->find("\"1;1;120;120") != std::string::npos);
    SAME(shown.scene.at({3, 0}).picture, std::uint32_t{0});
    SAME(shown.scene.at({15, 5}).picture, *first.begin());

    shown.image.hold(square(40, 40, 90));
    shown.settled();

    SAME(shown.marks(0).size(), std::size_t{1});
    OUGHT(*shown.marks(0).begin() != *first.begin());
}

// A sixel still being encoded for the image before, or for the cell before, is dropped when it
// comes; the smaller sixel of the cell before comes first.
TRIAL("Image: a late sixel of the image or the room before is dropped") {
    {
        Shown shown{shown_as(Graphics::sixel, {10, 20})};

        shown.image.hold(square(40, 40, 255));
        shown.show();
        shown.image.hold(square(40, 40, 51));
        shown.settled();

        const std::set<std::uint32_t> marks = shown.marks(0);

        SAME(marks.size(), std::size_t{1});
        OUGHT(shown.scene.screen().picture(*marks.begin())->sixel->find("#0;2;20;0;0") != std::string::npos);
    }

    Shown changed{shown_as(Graphics::sixel, {10, 10})};

    changed.image.hold(square(40, 40, 200));
    changed.show();
    changed.scene.display(shown_as(Graphics::sixel, {10, 20}));
    changed.settled();

    const std::set<std::uint32_t> now = changed.marks(0);

    SAME(now.size(), std::size_t{1});
    OUGHT(changed.scene.screen().picture(*now.begin())->size == (Size{12, 6}));
    OUGHT(changed.scene.screen().picture(*now.begin())->sixel->find("\"1;1;120;120") != std::string::npos);
}

// Without graphics, or without the cell's size, the image stays in characters; switching the
// terminal to sixel while it runs makes the picture, and switching back takes it away.
TRIAL("Image: characters without graphics or a cell's size; a switch of the terminal redoes it") {
    Shown shown{shown_as(Graphics::none, {10, 20})};

    shown.image.hold(square(40, 40, 200));
    shown.show();
    shown.settled();

    OUGHT(shown.marks(0).empty());
    SAME(shown.drawn(0), columns(4, 16));

    shown.scene.display(shown_as(Graphics::sixel, {10, 20}));
    shown.settled();

    SAME(shown.marks(0).size(), std::size_t{1});

    shown.scene.display(shown_as(Graphics::sixel, {10, 10}));
    shown.settled();

    SAME(shown.drawn(0), columns(7, 13));
    SAME(shown.marks(0).size(), std::size_t{1});

    shown.scene.display(shown_as(Graphics::none, {10, 20}));
    shown.settled();

    OUGHT(shown.marks(0).empty());

    for (const auto& [kind, cell] : std::vector<std::pair<Graphics, Size>>{{Graphics::sixel, {0, 20}}, {Graphics::kitty, {0, 20}}, {Graphics::sixel, {10, 0}}}) {
        Shown blind{shown_as(kind, cell)};

        blind.image.hold(square(40, 40, 200));
        blind.show();
        blind.settled();

        OUGHT(blind.marks(0).empty());
        OUGHT(!blind.drawn(2).empty());
    }
}

// With kitty the pictures are ready at once, one for every frame, on pixels at the room's full
// resolution that the frames share; the animation switches between them.
TRIAL("Image: with kitty a picture for every frame at once, on the room's pixels") {
    Shown shown{shown_as(Graphics::kitty, {10, 20})};

    shown.image.hold(lump(leaving));
    shown.show();
    shown.settled();

    const std::set<std::uint32_t> first = shown.marks(2);

    SAME(first.size(), std::size_t{1});

    const tal::Picture* const one = shown.scene.screen().picture(*first.begin());

    OUGHT(one != nullptr && one->pixels != nullptr && one->sixel == nullptr);
    OUGHT(one != nullptr && one->pixels->size() == (tal::media::Size{160, 120}) && one->pixels->stills().size() == 4 && one->still == 0);
    OUGHT(one != nullptr && one->size == (Size{16, 6}));

    shown.scene.wait(40ms);

    const std::set<std::uint32_t> second = shown.marks(2);

    SAME(second.size(), std::size_t{1});
    OUGHT(*second.begin() != *first.begin());
    SAME(shown.scene.screen().picture(*second.begin())->still, std::size_t{1});
}
