#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/crypto.hpp"
#include "tal/drawing.hpp"
#include "tal/document.hpp"
#include "tal/drawn.hpp"
#include "tal/executor.hpp"
#include "tal/fonts.hpp"
#include "tal/pixels.hpp"
#include "tal/trouble.hpp"
#include "tal/scene.hpp"
#include "tal/xml.hpp"

#include "tal/testing.hpp"

// Recordings (E13): what a pen draws into a recording, played on a raster, is what the pen would
// have drawn there, pixel for pixel, every kind of step and state among it (clips within clips,
// matrices, opacity, gradients, patterns, images sharp and smooth, dashes, arcs, text), also on a
// pen scaled twice as large; the pen's state stands after a play, also one that throws. Written as
// SVG it is a document of its size whose elements carry what was drawn: paths with their transform
// and rules, strokes with their style, gradients and patterns, clips as groups, images as PNG that
// read back as the image. A drawing records one frame of its painter at the time it last drew.

namespace {

using namespace std::chrono_literals;

using tal::Kind;
using tal::drawing::Align;
using tal::drawing::Baseline;
using tal::drawing::Cap;
using tal::drawing::Color;
using tal::drawing::Font;
using tal::drawing::Gradient;
using tal::drawing::Join;
using tal::drawing::Matrix;
using tal::drawing::Path;
using tal::drawing::Pattern;
using tal::drawing::Pen;
using tal::drawing::Point;
using tal::drawing::Raster;
using tal::drawing::Recording;
using tal::drawing::Rect;
using tal::drawing::Rule;
using tal::drawing::Stroke;
using tal::media::Pixels;
using tal::media::Size;
using tal::media::Still;

// The kind of the trouble `work` throws, none when it throws nothing.
std::optional<Kind> trouble_of(const std::function<void()>& work) {
    try {
        work();
    } catch (const tal::Trouble& why) {
        return why.kind();
    }

    return std::nullopt;
}

// An image of `size` whose pixels are numbered by their place, opaque.
Pixels numbered(Size size) {
    tal::Bytes rgba;

    for (std::uint32_t y = 0; y < size.height; ++y)
        for (std::uint32_t x = 0; x < size.width; ++x) rgba.insert(rgba.end(), {std::byte{static_cast<std::uint8_t>(x * 40)}, std::byte{static_cast<std::uint8_t>(y * 60)}, std::byte{static_cast<std::uint8_t>((x + y) * 25)}, std::byte{255}});

    return Pixels{size, {Still{std::move(rgba), {}}}};
}

// A scene of every kind of step on `pen`: a ground, a clip of even-odd within a clip, a turned
// and scaled square, translucent shapes, a linear and a radial gradient, a pattern tiled and one
// that is not, an image drawn sharp from a part of it, dashed and round strokes, arcs (a whole
// circle among them), a quadratic and a cubic curve, and a word.
void scene(Pen& pen) {
    Path ground;

    ground.rectangle(Rect{Point{0, 0}, 120, 90});
    pen.fill(ground, Color{0.1F, 0.12F, 0.15F, 1});
    pen.save();

    Path frame;

    frame.rectangle(Rect{Point{5, 5}, 110, 80});
    pen.clip(frame);

    Path holed;

    holed.rectangle(Rect{Point{10, 10}, 60, 50});
    holed.rectangle(Rect{Point{25, 20}, 20, 20});
    pen.save();
    pen.clip(holed, Rule::evenodd);
    pen.transform(Matrix::turned(0.4).then(Matrix::scaled(1.5, 0.8)).then(Matrix::moved(30, 20)));

    Path square;

    square.rectangle(Rect{Point{-10, -10}, 40, 40});

    Gradient across = Gradient::linear(Point{-10, 0}, Point{30, 0});

    across.stop(0, Color{1, 0, 0, 1});
    across.stop(1, Color{0, 0, 1, 0.5F});
    pen.fill(square, across);
    pen.restore();

    Gradient round = Gradient::radial(Point{90, 30}, 2, Point{92, 32}, 20);

    round.stop(0, Color{1, 1, 0, 1});
    round.stop(0.6F, Color{0, 1, 0, 1});
    round.stop(1, Color{0, 0, 0, 0});

    Path disc;

    disc.arc(Point{92, 32}, 20, 0, 2 * std::numbers::pi);
    disc.close();
    pen.fill(disc, round);
    pen.opacity(0.5F);

    Path pie;

    pie.move(Point{60, 70});
    pie.ellipse(Point{60, 70}, 25, 12, 0.3, 0.2, 2.5);
    pie.close();
    pen.fill(pie, Color{0.9F, 0.5F, 0.1F, 0.8F});
    pen.opacity(1);

    Pattern tiled{numbered(Size{4, 3})};
    Pattern once{numbered(Size{4, 3}), false, false};

    tiled.place(Matrix::scaled(2, 2).then(Matrix::moved(3, 1)));
    once.place(Matrix::moved(100, 60));

    Path left;
    Path right;

    left.rectangle(Rect{Point{5, 60}, 30, 25});
    right.rectangle(Rect{Point{95, 55}, 20, 20});
    pen.fill(left, tiled);
    pen.fill(right, once);
    pen.smoothing(false);
    pen.draw(numbered(Size{5, 4}), Rect{Point{1, 1}, 3, 2}, Rect{Point{40, 5}, 18, 12});
    pen.smoothing(true);
    pen.draw(numbered(Size{5, 4}), Rect{Point{60, 5}, 15, 12});

    Path curve;

    curve.move(Point{10, 40});
    curve.quad(Point{30, 10}, Point{50, 40});
    curve.cubic(Point{60, 60}, Point{80, 20}, Point{110, 45});

    Stroke dashed;

    dashed.width = 2.5;
    dashed.dashes = {6, 3, 1, 3};
    dashed.offset = 2;
    dashed.cap = Cap::round;
    dashed.join = Join::bevel;
    pen.stroke(curve, Color{0.8F, 0.9F, 1, 1}, dashed);

    Stroke bold;

    bold.width = 4;
    bold.join = Join::round;
    pen.stroke(pie, Color{0, 0.6F, 0.3F, 0.7F}, bold);

    Path corner;
    Stroke sharp;
    Stroke whole;

    corner.move(Point{100, 80});
    corner.line(Point{110, 70});
    corner.line(Point{115, 85});
    sharp.width = 3;
    sharp.miter = 0.5;
    whole.dashes = {0, 0};
    pen.stroke(corner, Color{1, 0, 1, 1}, sharp);
    pen.stroke(corner, Color{0, 1, 1, 1}, whole);
    pen.fill(holed, Color{0.5F, 0.5F, 0.5F, 0.3F}, Rule::evenodd);
    pen.fill(Font::builtin().outlined("Ab", 14, Point{70, 80}, Align::left, Baseline::alphabetic), Color{1, 1, 1, 1});
    pen.restore();
}

// What `draw` gives on a raster of `size`, through a pen placed first by `first`.
Pixels drawn(Size size, const Matrix& first, const std::function<void(Pen&)>& draw) {
    Raster raster{size};
    Pen pen{raster};

    pen.place(first);
    draw(pen);

    return raster.pixels();
}

// The value of the attribute `name` of `one`, empty without it.
std::string valued(const tal::xml::Node& one, std::string_view name) {
    const std::string* found = tal::xml::attribute(one, name);

    return found != nullptr ? *found : std::string{};
}

// Every element of the tree under `one` (itself among them) named `name`.
void gathered(const tal::xml::Node& one, std::string_view name, std::vector<const tal::xml::Node*>& into) {
    if (one.kind == tal::xml::Kind::element && one.name == name) into.push_back(&one);

    for (const tal::xml::Node& child : one.children) gathered(child, name, into);
}

// The elements of `document` named `name`.
std::vector<const tal::xml::Node*> all_of(const tal::xml::Document& document, std::string_view name) {
    std::vector<const tal::xml::Node*> made;

    for (const tal::xml::Node& one : document.nodes) gathered(one, name, made);

    return made;
}

}

// A recording played on a raster of its size gives the raster the scene would have given; played
// on a pen scaled by two on a raster twice as large, what the scene gives there; a recording kept
// on one pen and played again later on another gives it again. A play leaves the pen as it was, a
// play that throws (a step the matrix carries beyond the numbers) too. A recording of no pixels is
// refused as a raster is; one moved from has no steps and plays nothing.
TRIAL("Recording: played on a raster, every kind of step comes out as drawn directly, at any scale") {
    const Size size{120, 90};
    Recording recording{size};

    {
        Pen pen{recording};

        scene(pen);
    }

    SAME(recording.steps(), std::size_t{20});
    SAME(recording.size(), size);

    const Pixels direct = drawn(size, Matrix{}, scene);
    const Pixels played = drawn(size, Matrix{}, [&recording](Pen& pen) { recording.played(pen); });

    OUGHT(played.stills().front().rgba == direct.stills().front().rgba);

    const Size twice{240, 180};

    OUGHT(drawn(twice, Matrix::scaled(2, 2), [&recording](Pen& pen) { recording.played(pen); }).stills().front().rgba == drawn(twice, Matrix::scaled(2, 2), scene).stills().front().rgba);

    Raster raster{size};
    Pen pen{raster};
    const Matrix moved = Matrix::moved(3, 4);

    pen.place(moved);
    pen.opacity(0.25F);
    recording.played(pen);
    SAME(pen.placed(), moved);
    SAME(trouble_of([&pen] { pen.restore(); }), std::optional<Kind>{Kind::other});

    Recording far{size};

    {
        Pen recorder{far};
        Path box;

        box.rectangle(Rect{Point{0, 0}, 10, 10});
        recorder.save();
        recorder.transform(Matrix::scaled(1e308, 1e308));
        recorder.fill(box, Color{1, 0, 0, 1});
    }

    SAME(trouble_of([&far, &pen] { far.played(pen); }), std::optional<Kind>{Kind::malformed});
    SAME(pen.placed(), moved);
    SAME(trouble_of([&pen] { pen.restore(); }), std::optional<Kind>{Kind::other});
    SAME(trouble_of([] { (void)Recording{Size{0, 5}}; }), std::optional<Kind>{Kind::other});
    SAME(trouble_of([] { (void)Recording{Size{4000, 2001}}; }), std::optional<Kind>{Kind::other});

    Recording taken = std::move(recording);

    SAME(recording.steps(), std::size_t{0});
    SAME(taken.steps(), std::size_t{20});
    OUGHT(drawn(size, Matrix{}, [&recording](Pen& one) { recording.played(one); }).stills().front().rgba == Raster{size}.pixels().stills().front().rgba);
}

// The SVG of a recording: a document of its size in pixels whose elements carry the steps. Fills
// and strokes as paths with their transforms (the matrix of the step), even-odd where it was the
// rule, the stroke's width, joins, caps, miter limit, dashes and offset; a whole circle as two
// arcs; the two gradients and the two patterns (one repeating, one in a tile out of reach) as their
// definitions; clips as clip paths whose groups close at the restore; translucency as opacity; the
// images as PNG that decode to the image, the sharp one marked so. A recording of nothing is an
// empty document of its size.
TRIAL("Recording: written as SVG, a document whose elements carry every step") {
    const Size size{120, 90};
    Recording recording{size};

    {
        Pen pen{recording};

        scene(pen);
    }

    const std::string text = tal::drawing::svg(recording);
    const tal::xml::Document document = tal::xml::parsed(text);
    const tal::xml::Node* root = tal::xml::root(document);

    OUGHT(root != nullptr);

    if (root == nullptr) return;

    SAME(root->name, std::string{"svg"});
    SAME(valued(*root, "width"), std::string{"120"});
    SAME(valued(*root, "height"), std::string{"90"});
    SAME(valued(*root, "viewBox"), std::string{"0 0 120 90"});

    const std::vector<const tal::xml::Node*> paths = all_of(document, "path");
    const std::vector<const tal::xml::Node*> clips = all_of(document, "clipPath");

    SAME(clips.size(), std::size_t{2});
    OUGHT(valued(clips[1]->children.front(), "clip-rule") == "evenodd");
    OUGHT(std::ranges::any_of(paths, [](const tal::xml::Node* one) { return valued(*one, "transform") == "matrix(1 0 0 1 0 0)" && valued(*one, "fill") == "#1a1f26"; }));

    const auto dashed = std::ranges::find_if(paths, [](const tal::xml::Node* one) { return !valued(*one, "stroke-dasharray").empty(); });

    OUGHT(dashed != paths.end());

    if (dashed != paths.end()) {
        SAME(valued(**dashed, "stroke-dasharray"), std::string{"6,3,1,3"});
        SAME(valued(**dashed, "stroke-dashoffset"), std::string{"2"});
        SAME(valued(**dashed, "stroke-width"), std::string{"2.5"});
        SAME(valued(**dashed, "stroke-linecap"), std::string{"round"});
        SAME(valued(**dashed, "stroke-linejoin"), std::string{"bevel"});
        SAME(valued(**dashed, "fill"), std::string{"none"});
        OUGHT(valued(**dashed, "d").find('Q') != std::string::npos && valued(**dashed, "d").find('C') != std::string::npos);
    }

    const auto circle = std::ranges::find_if(paths, [](const tal::xml::Node* one) { return valued(*one, "fill").starts_with("url(#") && valued(*one, "d").find('A') != std::string::npos; });

    OUGHT(circle != paths.end());

    if (circle != paths.end()) SAME(std::ranges::count(valued(**circle, "d"), 'A'), std::ptrdiff_t{2});

    const auto pie = std::ranges::find_if(paths, [](const tal::xml::Node* one) { return valued(*one, "opacity") == "0.5" && valued(*one, "fill-opacity") == "0.8"; });

    OUGHT(pie != paths.end());

    // The pie's arc of 2.3 radians on the screen's way round: not the larger, the positive sweep.
    if (pie != paths.end()) {
        const std::string d = valued(**pie, "d");
        std::vector<std::string> words;
        std::string word;

        for (const char one : d.substr(std::min(d.find('A'), d.size()))) {
            if (one != ' ') {
                word += one;
            } else if (!word.empty()) {
                words.push_back(std::exchange(word, {}));
            }
        }

        OUGHT(words.size() > 5 && words[0] == "A25" && words[1] == "12" && words[3] == "0" && words[4] == "1");
    }

    SAME(std::ranges::count_if(paths, [](const tal::xml::Node* one) { return !valued(*one, "stroke-dasharray").empty(); }), std::ptrdiff_t{1});
    OUGHT(std::ranges::any_of(paths, [](const tal::xml::Node* one) { return valued(*one, "stroke-miterlimit") == "1"; }));
    OUGHT(std::ranges::any_of(paths, [](const tal::xml::Node* one) { return valued(*one, "fill-rule") == "evenodd"; }));

    const std::vector<const tal::xml::Node*> linear = all_of(document, "linearGradient");
    const std::vector<const tal::xml::Node*> radial = all_of(document, "radialGradient");

    SAME(linear.size(), std::size_t{1});
    SAME(radial.size(), std::size_t{1});

    if (linear.size() == 1) {
        SAME(valued(*linear.front(), "x1"), std::string{"-10"});
        SAME(valued(*linear.front(), "x2"), std::string{"30"});
        SAME(valued(*linear.front(), "color-interpolation"), std::string{"linearRGB"});
        SAME(linear.front()->children.size(), std::size_t{2});
    }

    if (radial.size() == 1) {
        SAME(valued(*radial.front(), "fr"), std::string{"2"});
        SAME(valued(*radial.front(), "r"), std::string{"20"});
        SAME(radial.front()->children.size(), std::size_t{3});
    }

    const std::vector<const tal::xml::Node*> patterns = all_of(document, "pattern");

    SAME(patterns.size(), std::size_t{2});

    if (patterns.size() == 2) {
        SAME(valued(*patterns[0], "width"), std::string{"4"});
        SAME(valued(*patterns[0], "patternTransform"), std::string{"matrix(2 0 0 2 3 1)"});
        SAME(valued(*patterns[1], "width"), std::string{"1e+07"});
    }

    const std::vector<const tal::xml::Node*> images = all_of(document, "image");

    SAME(images.size(), std::size_t{4});

    for (const tal::xml::Node* one : images) {
        const std::string href = valued(*one, "xlink:href");
        const std::string_view head = "data:image/png;base64,";

        OUGHT(href.starts_with(head));

        const std::optional<tal::Bytes> bytes = tal::crypto::unbase64(std::string_view{href}.substr(head.size()));
        Pixels back;

        OUGHT(bytes.has_value());

        if (bytes)
            tal::media::decoded(*bytes, [&back](std::span<const std::byte> rgba, Size seen, std::chrono::milliseconds) {
                back = Pixels{seen, {Still{tal::Bytes(rgba.begin(), rgba.end()), {}}}};

                return true;
            }, tal::Optional<tal::media::Decoding>{tal::media::Decoding::png()});

        OUGHT(back.held() && (back.stills().front().rgba == numbered(back.size()).stills().front().rgba));
    }

    SAME(std::ranges::count_if(images, [](const tal::xml::Node* one) { return valued(*one, "image-rendering") == "optimizeSpeed"; }), std::ptrdiff_t{1});

    Recording signed_zero{Size{7, 3}};

    {
        Pen pen{signed_zero};
        Path dot;

        dot.move(Point{-0.0, 1});
        dot.line(Point{2, -0.0});
        pen.stroke(dot, Color{1, 1, 1, 1});
    }

    OUGHT(tal::drawing::svg(signed_zero).find("d=\"M0 1 L2 0\"") != std::string::npos);

    const std::string nothing = tal::drawing::svg(Recording{Size{7, 3}});

    OUGHT(nothing.find("width=\"7\" height=\"3\"") != std::string::npos);
    SAME(all_of(tal::xml::parsed(nothing), "path").size(), std::size_t{0});
}

// A drawing records one frame of its painter at the size asked for, or its own, and at the time of
// the frame it last drew (naught before any): the same steps as a pen of that size would get. A
// drawing of no size, asked for none, cannot be recorded (no pixels); a painter's trouble comes
// out.
TRIAL("Recording: a drawing records a frame of its painter at the time it last drew") {
    std::optional<std::chrono::nanoseconds> heard;
    Size asked;
    tal::Drawing drawing{[&](Pen& pen, Size size, std::chrono::nanoseconds since) {
        heard = since;
        asked = size;

        Path box;

        box.rectangle(Rect{Point{1, 1}, static_cast<double>(size.width) - 2, static_cast<double>(size.height) - 2});
        pen.fill(box, Color{0.2F, 0.4F, 0.6F, 1});
    }};

    SAME(trouble_of([&drawing] { (void)drawing.recorded(); }), std::optional<Kind>{Kind::other});

    const Recording recording = drawing.recorded(Size{30, 20});

    SAME(asked, (Size{30, 20}));
    SAME(heard, std::optional<std::chrono::nanoseconds>{std::chrono::nanoseconds{0}});
    SAME(recording.steps(), std::size_t{1});
    OUGHT(drawn(Size{30, 20}, Matrix{}, [&recording](Pen& pen) { recording.played(pen); }).stills().front().rgba ==
          drawn(Size{30, 20}, Matrix{}, [](Pen& pen) {
              Path box;

              box.rectangle(Rect{Point{1, 1}, 28, 18});
              pen.fill(box, Color{0.2F, 0.4F, 0.6F, 1});
          }).stills().front().rgba);

    tal::Drawing failing{[](Pen&, Size, std::chrono::nanoseconds) { throw tal::Trouble{Kind::broken, "The painter failed."}; }};

    SAME(trouble_of([&failing] { (void)failing.recorded(Size{4, 4}); }), std::optional<Kind>{Kind::broken});

    tal::testing::Scene scene{tal::Display{.screen = {20, 6}, .graphics = tal::Graphics::sixel, .cell = {10, 20}}};
    tal::Loop loop{scene.time()};
    tal::Document document{scene.time()};
    std::vector<std::chrono::nanoseconds> times;
    std::vector<Size> sizes;
    tal::Drawing& shown = document.children().add<tal::Drawing>([&times, &sizes](Pen&, Size size, std::chrono::nanoseconds since) {
        times.push_back(since);
        sizes.push_back(size);
    });

    shown.rate(10);
    scene.show(document);

    for (int round = 0; round < 4; ++round) {
        scene.wait(100ms);
        UNTIL(loop, !shown.busy());
    }

    OUGHT(times.size() > 2);

    const std::chrono::nanoseconds last = times.back();
    const Recording frame = shown.recorded();

    OUGHT(last > 0ns);
    SAME(times.back(), last);
    SAME(sizes.back(), shown.pixels());
    SAME(frame.size(), shown.pixels());
}
