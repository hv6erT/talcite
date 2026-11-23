#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tal/controls.hpp"
#include "tal/crypto.hpp"
#include "tal/document.hpp"
#include "tal/drawing.hpp"
#include "tal/drawn.hpp"
#include "tal/event.hpp"
#include "tal/executor.hpp"
#include "tal/layout.hpp"
#include "tal/scene.hpp"
#include "tal/trouble.hpp"

#include "inflating.hpp"
#include "sixeling.hpp"
#include "quieting.hpp"
#include "tal/testing.hpp"

// Drawings in the tree (E7c, E7d): the size in pixels of the room on a terminal of pictures and in
// characters, bands of four rows of cells, each its own picture, coded for kitty as zlib in base64
// and for sixel, a band that did not change kept with its mark; frames at a rate on the document's
// clock, a frame asked for while one is being coded made after it, the time since the first frame;
// the way forced; a painter's trouble in place of the drawing; and the point under the mouse, in
// the middle of its cell or at the very pixel the terminal tells.

namespace {

using namespace std::chrono_literals;

using tal::Display;
using tal::Document;
using tal::Drawing;
using tal::Graphics;
using tal::MouseButton;
using tal::MouseEvent;
using tal::Press;
using tal::Size;
using tal::Spot;
using tal::drawing::Color;
using tal::drawing::Path;
using tal::drawing::Pen;
using tal::drawing::Point;
using tal::drawing::Rect;
using tal::testing::Scene;

// A terminal of `graphics`, cells of 10 by 20 pixels, of `screen` cells.
Display shown(Graphics graphics, Size screen = {20, 6}) { return Display{.screen = screen, .graphics = graphics, .cell = {10, 20}}; }

// A drawing of `painter` alone on a scene as `display`, with the events it heard.
struct Sketched {
    Scene scene;
    tal::Loop loop{scene.time()};
    Document document{scene.time()};
    Drawing& drawing;
    std::vector<std::optional<Point>> pointed;
    tal::Hooks hooks;

    // The drawing of `painter` on a terminal as `display`, below a line of text when `below`.
    Sketched(Drawing::Painter painter, Display display, bool below = false) : scene(display), drawing(placed(document, std::move(painter), below)) {
        hooks.add(drawing.on<MouseEvent>([this](MouseEvent& one) { pointed.push_back(drawing.pointed(one)); }));
        scene.show(document);
        settled();
    }

    // The drawing of `painter` added to `document`, after a line of text when `below`.
    static Drawing& placed(Document& document, Drawing::Painter painter, bool below) {
        if (!below) return document.children().add<Drawing>(std::move(painter), tal::drawing::Blending::srgb);

        tal::Column& column = document.children().add<tal::Column>();

        column.children().add<tal::Label>("above");

        return column.children().add<Drawing>(std::move(painter), tal::drawing::Blending::srgb);
    }

    // Lets the coding come back and be drawn.
    ~Sketched() { UNTIL(loop, !drawing.busy()); }

    Sketched(const Sketched&) = delete;
    Sketched& operator=(const Sketched&) = delete;

    // Lets everything asked come back and be drawn.
    void settled() {
        for (int round = 0; round < 10; ++round) {
            scene.wait(0ms);
            UNTIL(loop, !drawing.busy());
            QUIETED(loop);
        }
    }

    // The picture on the cell `at`, none where there is none.
    [[nodiscard]] const tal::Picture* picture(Spot at) const { return scene.screen().picture(scene.at(at).picture); }
};

// A painter filling the whole raster with `color`.
Drawing::Painter filling(Color color) {
    return [color](Pen& pen, tal::media::Size size, std::chrono::nanoseconds) {
        Path all;

        all.rectangle(Rect{Point{0, 0}, static_cast<double>(size.width), static_cast<double>(size.height)});
        pen.fill(all, color);
    };
}

// The bytes the base64 of zlib `coded` holds.
tal::Bytes unpacked(const std::string& coded) {
    const std::optional<tal::Bytes> packed = tal::crypto::unbase64(coded);

    if (!packed) return {};

    tal::inflating::Inflater inflater{tal::inflating::Wrapping::zlib};

    return inflater.fed(*packed);
}

}

TRIAL("Drawing: on kitty the room's pixels in bands of four rows, each a picture coded as zlib in base64; a band that did not change keeps its mark") {
    int frame = 0;
    Sketched sketched{[&frame](Pen& pen, tal::media::Size size, std::chrono::nanoseconds) {
                          Path all;
                          Path dot;

                          all.rectangle(Rect{Point{0, 0}, static_cast<double>(size.width), static_cast<double>(size.height)});
                          pen.fill(all, Color{1, 0, 0, 1});
                          dot.rectangle(Rect{Point{10.0 * frame, 100}, 10, 10});
                          pen.fill(dot, Color{0, 0, 1, 1});
                      },
                      shown(Graphics::kitty)};

    SAME(sketched.drawing.pixels(), (tal::media::Size{200, 120}));
    SAME(sketched.drawing.graphics(), Graphics::kitty);
    SAME(sketched.drawing.frames(), std::uint64_t{1});

    const tal::Picture* top = sketched.picture(Spot{0, 0});
    const tal::Picture* bottom = sketched.picture(Spot{0, 5});

    OUGHT(top != nullptr && bottom != nullptr && top->mark != bottom->mark);
    SAME(sketched.picture(Spot{19, 3})->mark, top->mark);
    SAME(top->size, (Size{20, 4}));
    SAME(bottom->size, (Size{20, 2}));
    OUGHT(top->coded != nullptr && top->pixels != nullptr);
    SAME(unpacked(*top->coded), top->pixels->stills().front().rgba);
    SAME(top->pixels->size(), (tal::media::Size{200, 80}));
    SAME(std::to_integer<int>(top->pixels->stills().front().rgba[0]), 255);

    const std::uint32_t kept = top->mark;
    const std::uint32_t moved = bottom->mark;

    frame = 3;
    sketched.drawing.redraw();
    sketched.settled();
    SAME(sketched.picture(Spot{0, 0})->mark, kept);
    OUGHT(sketched.picture(Spot{0, 5})->mark != moved);
    SAME(sketched.drawing.frames(), std::uint64_t{2});
}

TRIAL("Drawing: frames at a rate by the document's clock and the time since the first; a frame asked for while one is coded comes after it; the rate malformed") {
    std::vector<std::chrono::nanoseconds> times;
    Sketched sketched{[&times](Pen&, tal::media::Size, std::chrono::nanoseconds since) { times.push_back(since); }, shown(Graphics::kitty)};

    SAME(times.size(), std::size_t{1});
    SAME(times.front(), std::chrono::nanoseconds{0});

    sketched.drawing.rate(10);

    for (int one = 0; one < 3; ++one) {
        sketched.scene.wait(100ms);
        sketched.settled();
    }

    SAME(times.size(), std::size_t{4});
    SAME(times.back(), std::chrono::nanoseconds{300ms});

    sketched.drawing.rate(0);
    sketched.scene.wait(500ms);
    sketched.settled();
    SAME(times.size(), std::size_t{4});

    sketched.drawing.redraw();
    sketched.drawing.redraw();
    OUGHT(sketched.drawing.busy());
    sketched.settled();
    SAME(times.size(), std::size_t{6});
    THREW(sketched.drawing.rate(-1));
}

TRIAL("Drawing: sixel pictures in bands down to whole sixels, the last row alone, characters without pixels, the way forced; a painter's trouble in its place, gone with the next frame") {
    {
        Sketched sixel{filling(Color{0, 1, 0, 1}), shown(Graphics::sixel)};

        SAME(sixel.drawing.graphics(), Graphics::sixel);
        OUGHT(sixel.picture(Spot{0, 0}) != nullptr && sixel.picture(Spot{0, 0})->sixel != nullptr);
        SAME(sixel.picture(Spot{0, 0})->sixel->substr(0, 2), std::string{"\x1bP"});
        SAME(sixel.picture(Spot{0, 0})->size, (Size{20, 4}));
        OUGHT(sixel.picture(Spot{0, 0})->sixel->find("\"1;1;200;84") != std::string::npos);
    }

    {
        Sketched short_cells{filling(Color{0, 1, 0, 1}), Display{.screen = {20, 10}, .graphics = Graphics::sixel, .cell = {10, 28}}};
        const tal::Picture* first = short_cells.picture(Spot{0, 0});
        const tal::Picture* second = short_cells.picture(Spot{0, 4});
        const tal::Picture* last = short_cells.picture(Spot{0, 8});

        OUGHT(first != nullptr && second != nullptr && last != nullptr);
        SAME(first->size, (Size{20, 4}));
        OUGHT(first->sixel->find("\"1;1;200;114") != std::string::npos);
        SAME(second->size, (Size{20, 4}));
        SAME(last->size, (Size{20, 1}));
        OUGHT(last->sixel->find("\"1;1;200;30") != std::string::npos);
        OUGHT(short_cells.picture(Spot{0, 9}) != nullptr && short_cells.picture(Spot{0, 9}) != last);
        SAME(short_cells.picture(Spot{0, 9})->size, (Size{20, 1}));
    }

    {
        Sketched plain{filling(Color{1, 1, 1, 1}), Display{.screen = {20, 6}}};

        SAME(plain.drawing.graphics(), Graphics::none);
        SAME(plain.drawing.pixels(), (tal::media::Size{20 * 8, 6 * 8}));
        OUGHT(!(plain.scene.at(Spot{0, 0}) == tal::Cell{}));
        OUGHT(plain.picture(Spot{0, 0}) == nullptr);

        plain.drawing.force(Graphics::kitty);
        plain.settled();
        SAME(plain.drawing.graphics(), Graphics::none);
    }

    {
        Sketched forced{filling(Color{1, 1, 1, 1}), shown(Graphics::sixel)};

        forced.drawing.force(Graphics::kitty);
        forced.settled();
        SAME(forced.drawing.graphics(), Graphics::kitty);
        OUGHT(forced.picture(Spot{0, 0})->coded != nullptr);

        forced.drawing.force(Graphics::none);
        forced.settled();
        SAME(forced.drawing.graphics(), Graphics::none);
        OUGHT(forced.picture(Spot{0, 0}) == nullptr);
    }

    bool failing = true;
    Sketched troubled{[&failing](Pen& pen, tal::media::Size, std::chrono::nanoseconds) {
                          if (failing) throw tal::Trouble{tal::Kind::malformed, "The simulation went astray."};

                          (void)pen;
                      },
                      shown(Graphics::kitty, Size{40, 6})};

    OUGHT(troubled.drawing.trouble().has_value());
    SAME(troubled.scene.row(0).substr(0, 27), std::string{"The simulation went astray."});

    failing = false;
    troubled.drawing.redraw();
    troubled.settled();
    OUGHT(!troubled.drawing.trouble().has_value());
    SAME(troubled.scene.row(0).substr(0, 3), std::string{"   "});
}

// A painter of stripes and a ramp, the same at every frame, so that each band differs.
Drawing::Painter striped() {
    return [](Pen& pen, tal::media::Size size, std::chrono::nanoseconds) {
        for (std::uint32_t y = 0; y < size.height; y += 7) {
            Path stripe;

            stripe.rectangle(Rect{Point{0, static_cast<double>(y)}, static_cast<double>(size.width) * (y % 3 + 1) / 3, 3});
            pen.fill(stripe, Color{static_cast<float>(y % 255) / 255.0F, 0.5F, 1 - static_cast<float>(y % 255) / 255.0F, 1});
        }
    };
}

// The first frame of a new size shows its mosaic at once, made on the loop, while the pictures are
// coded on the pool; a resize does not leave the drawing blank until they come. The bands, coded
// side by side on the pool, are those coded one by one from the same frame (the last row of a
// sixel drawing its own band), and new bands take their marks in order down the drawing.
TRIAL("Drawing: a new size shows its mosaic at once, the pictures after it; bands coded side by side as one by one, marked in order") {
    Sketched sketched{striped(), shown(Graphics::sixel, Size{20, 30})};

    OUGHT(sketched.picture(Spot{0, 0}) != nullptr);

    sketched.document.display(shown(Graphics::sixel, Size{24, 30}));

    const tal::Surface& first = sketched.document.frame();

    OUGHT(!(first.at(Spot{23, 0}) == tal::Cell{}));
    SAME(first.at(Spot{23, 0}).picture, std::uint32_t{0});

    sketched.settled();
    sketched.scene.resize(Size{24, 30});

    const tal::media::Size size{240, 600};
    tal::drawing::Raster raster{size, tal::drawing::Blending::srgb};
    Pen pen{raster};

    striped()(pen, size, std::chrono::nanoseconds{0});

    const tal::media::Pixels whole = raster.pixels();
    std::uint32_t last = 0;

    for (int top = 0; top < 30;) {
        const int rows = top < 29 ? std::min(4, 29 - top) : 1;
        const std::uint32_t high = static_cast<std::uint32_t>(rows * 20 + 5) / 6 * 6;
        tal::Bytes rgba(std::size_t{240} * high * 4, std::byte{0});
        const tal::Bytes& all = whole.stills().front().rgba;
        const std::size_t from = std::size_t{240} * 4 * static_cast<std::size_t>(top * 20);
        const std::size_t much = std::min(rgba.size(), all.size() - from);

        std::copy(all.begin() + static_cast<std::ptrdiff_t>(from), all.begin() + static_cast<std::ptrdiff_t>(from + much), rgba.begin());

        std::vector<tal::media::Still> stills;

        stills.push_back(tal::media::Still{std::move(rgba), std::chrono::milliseconds{0}});

        const tal::Picture* shown_there = sketched.picture(Spot{0, top});

        OUGHT(shown_there != nullptr && shown_there->sixel != nullptr);
        SAME(*shown_there->sixel, tal::sixeling::sixel(tal::media::Pixels{tal::media::Size{240, high}, std::move(stills)}, 0));
        OUGHT(shown_there->mark > last);

        last = shown_there->mark;
        top += rows;
    }
}

TRIAL("Drawing: the point under the mouse, in the middle of its cell or at the pixel the terminal tells; a new size draws anew") {
    Sketched sketched{filling(Color{1, 1, 1, 1}), shown(Graphics::kitty)};

    sketched.scene.click(Spot{3, 2});
    OUGHT(!sketched.pointed.empty());
    SAME(sketched.pointed.front(), (std::optional<Point>{Point{35, 50}}));

    sketched.pointed.clear();
    sketched.document.mouse(Press::down, MouseButton::left, Spot{3, 2}, {}, 0, tal::Axis::vertical, Spot{37, 41});
    sketched.document.mouse(Press::up, MouseButton::left, Spot{3, 2}, {}, 0, tal::Axis::vertical, Spot{37, 41});
    SAME(sketched.pointed.front(), (std::optional<Point>{Point{37.5, 41.5}}));

    const std::uint64_t before = sketched.drawing.frames();

    sketched.scene.resize(Size{30, 8});
    sketched.settled();
    SAME(sketched.drawing.pixels(), (tal::media::Size{300, 160}));
    OUGHT(sketched.drawing.frames() > before);
}

TRIAL("Drawing: the point under the mouse counts from where the drawing stands, below a line of text") {
    Sketched lower{filling(Color{1, 1, 1, 1}), shown(Graphics::kitty), true};

    lower.scene.click(Spot{3, 2});
    OUGHT(!lower.pointed.empty());
    SAME(lower.pointed.front(), (std::optional<Point>{Point{35, 30}}));

    lower.pointed.clear();
    lower.document.mouse(Press::down, MouseButton::left, Spot{3, 2}, {}, 0, tal::Axis::vertical, Spot{37, 41});
    lower.document.mouse(Press::up, MouseButton::left, Spot{3, 2}, {}, 0, tal::Axis::vertical, Spot{37, 41});
    SAME(lower.pointed.front(), (std::optional<Point>{Point{37.5, 21.5}}));
}
