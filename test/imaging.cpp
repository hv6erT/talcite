#include "proving.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "talcite/frame.hpp"
#include "talcite/image.hpp"
#include "talcite/loop.hpp"
#include "talcite/picture.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "faking.hpp"

using namespace talcite;

namespace {

// Turns a lowercase hexadecimal string into bytes, two digits per byte.
[[nodiscard]] Bytes lump(std::string_view hex) {
    Bytes made;

    made.reserve(hex.size() / 2);

    const auto value = [](char one) -> int {
        if (one >= '0' && one <= '9') return one - '0';

        return one - 'a' + 10;
    };

    for (std::size_t at = 0; at + 1 < hex.size(); at += 2)
        made.push_back(std::byte{static_cast<std::uint8_t>(value(hex[at]) * 16 +
                                                            value(hex[at + 1]))});

    return made;
}

// The grid density, the cell size, the color depth and the character set live on the root, because
// they are facts about the surface, not about a widget. Here the trial plays the role of `Term`: it
// sets them, by default like a terminal that can do everything.
class Sheet : public ui::Root {
public:
    // Sets the cell size and the block kind, full colors and Unicode.
    explicit Sheet(ui::Size given = {10, 20}, ui::Blocks kind = ui::Blocks::sextants) {
        cell(given);
        blocks(kind);
        colors(Colors::rgb);
        unicode(true);
    }
};

// How long until the root's nearest appointment, by the time of the root's schedule, or nothing
// when it has none: what an animated picture asked for when it was last drawn.
std::optional<std::chrono::milliseconds> waiting(ui::Root& top) {
    const std::optional<Clock::Moment> next = top.schedule().next();

    if (!next) return std::nullopt;

    return std::chrono::ceil<std::chrono::milliseconds>(*next - top.schedule().now());
}

// A 16x12 GIF, four frames of 40, 100, 100 and 120 ms.
constexpr std::string_view leaping =
    "47494638396110000c00810000000000ff000000000000000021ff0b4e45545343415045322e300301000000"
    "21f90404040000002c0000000010000c000008220003080c00a0a0c18305071244c85021c386031f4a9c48b1"
    "a2c58b18336adc2831200021f90405000002002c00000000070006008100000000ff00000000000000081900"
    "01080420a0e0408206050658186020c30005053c8cf830200021f90405010002002c03000200070006008100"
    "00000000ff00000000000008190001080420a0e0408206050658186020c30005053c8cf830200021f904050c"
    "0002002c060004000700060081000000ff000000000000000008190001080420a0e0408206050658186020c3"
    "0005053c8cf83020003b";

// A 4x3 GIF with two frames: the first twenty milliseconds, the second three seconds. Short at the
// start and long after it means that after a hundred milliseconds we are certainly in the second,
// with almost three seconds to spare, so the trial does not depend on the machine's load.
constexpr std::string_view quick =
    "47494638396104000300810000ff000000000000000000000021ff0b4e45545343415045322e300301000000"
    "21f90400020000002c00000000040003000008080001081c485060400021f904012c0101002c000000000400"
    "03008100ff0000000000000000000008080001081c48506040003b";

// The widget on a fake: the trials check the widget (aspect ratio, clock, scrolling), not the
// decoder, which lives in talcite-adapters together with its own trials. The seam and the bytes go
// into the closure of `figured`; the widget gets the source.
[[nodiscard]] std::shared_ptr<ui::Image> shown(std::string_view hex) {
    const auto one = std::make_shared<ui::Image>();

    one->hold(figured(lump(hex), faking::images()));

    return one;
}

// The file is decoded on the pool after the first paint and comes back through the loop: we turn
// the loop and paint until the request comes back, as with the spool. Returns false if it does not
// settle in time.
[[nodiscard]] bool settled(Loop& loop, ui::Root& top, const ui::Image& one, ui::Size room) {
    for (int tries = 0; tries < 500; ++tries) {
        (void)top.render({{0, 0}, room});
        (void)loop.turn();

        if (!one.busy() && (one.frames() > 0 || !one.held())) return true;

        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }

    return false;
}

}

// --- What the widget knows before layout ---------------------------------------------------------

// Decoding computes, so it runs on the `cpu` lane. The trial fills the whole limit of the computing
// lane with waiting tasks: the picture's source must then wait in the queue and start only after
// room is freed. On the ordinary lane it would start at once, next to the busy ones.
//
// The state of the occupying tasks lives behind a shared pointer, not on the trial's stack: a task
// that sleeps at the moment of release wakes up after the trial has ended, and reading a dead stack
// frame could return "not yet" and hang the worker forever.
TRIAL("figured: decoding runs on the cpu lane") {
    struct Hold {
        std::atomic<bool> release{false};
        std::atomic<int> held{0};
        std::atomic<int> left{0};
    };

    const auto hold = std::make_shared<Hold>();

    const int most = static_cast<int>(pooling().most());

    for (int at = 0; at < most; ++at)
        (void)pooling().post(
            [hold] {
                ++hold->held;
                while (!hold->release.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds{2});
                ++hold->left;
            },
            Lane::cpu);

    for (int at = 0; at < 500 && hold->held.load() < most; ++at)
        std::this_thread::sleep_for(std::chrono::milliseconds{2});

    Promise<Pixels> made =
        figured(lump(leaping), faking::images()).fetch(ui::Size{4, 4});

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    const bool early = made.progress().fraction() == 1.0;

    hold->release.store(true);

    (void)std::move(made).get();

    for (int at = 0; at < 500 && hold->left.load() < most; ++at)
        std::this_thread::sleep_for(std::chrono::milliseconds{2});

    ought(!early);
    same(hold->left.load(), most);
}

TRIAL("Image: an empty one holds nothing and asks for nothing") {
    Sheet sheet;

    const auto one = std::make_shared<ui::Image>();

    sheet.children().add(one);
    (void)sheet.render({{0, 0}, {40, 20}});

    ought(!one->held());
    same(one->wants({40, 20}).wide, 0);
    ought(!waiting(sheet).has_value());
}

// The size must be known before painting, because the layout asks for it, so `hold` reads it from
// the header (`shaped`), without a decoder and at once. The number of frames is known only when the
// request after the first layout comes back, because GIF does not state it in the header and
// counting it means going through the whole file.
TRIAL("Image: after loading it knows the format and size, the frames only after the request") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(leaping);

    ought(one->held());
    ought(!one->busy());
    same(static_cast<int>(one->format()), static_cast<int>(Format::gif));
    same(one->size().wide, 16);
    same(one->size().high, 12);
    same(one->frames(), std::size_t{0});

    sheet.children().add(one);
    (void)sheet.render({{0, 0}, {40, 20}});

    ought(one->busy());
    same(one->frames(), std::size_t{0});

    ought(settled(loop, sheet, *one, {40, 20}));
    ought(!one->busy());
    same(one->frames(), std::size_t{4});
}

// A refusal by the provider and no provider at all end the same way: an empty widget, with the
// format from the header. This is the only place where the seam's exception does not go further up:
// the widget is to show what it can, and the legend, through `kind()`, will say "JPEG, no
// provider".
TRIAL("Image: a provider's refusal holds nothing, the format stays from the header") {
    Loop loop;
    Sheet sheet;

    const auto one = std::make_shared<ui::Image>();

    one->hold(figured(lump("ffd8ffe000104a464946000102000000"), faking::images()));

    same(static_cast<int>(one->format()), static_cast<int>(Format::jpeg));
    ought(!one->held());

    // A header with dimensions, but the provider refuses: the size is there until the first
    // request, then the widget is empty. It shows what it can, and the legend, through `kind()`,
    // will say what it cannot.
    const auto other = std::make_shared<ui::Image>();

    other->hold(figured(lump("ffd8ffe100104578696600000000000000000000ffc0001108001000200301220002110103110100"),
                        faking::images()));

    ought(other->held());

    sheet.children().add(other);

    ought(settled(loop, sheet, *other, {40, 20}));
    ought(!other->held());
    same(static_cast<int>(other->format()), static_cast<int>(Format::jpeg));
}

TRIAL("Image: without a provider a file stays empty, while a picture made in code works") {
    const auto one = std::make_shared<ui::Image>();

    one->hold(figured(lump(leaping), {}));

    same(static_cast<int>(one->format()), static_cast<int>(Format::gif));
    ought(!one->held());

    one->hold(Pixels{{2, 2}, {Still{Bytes(16, std::byte{200}), std::chrono::milliseconds{0}}}});

    ought(one->held());

    one->clear();

    ought(!one->held());
    same(one->frames(), std::size_t{0});
    same(static_cast<int>(one->format()), static_cast<int>(Format::none));
}

// The widget knows only the source. Bytes, the seam, the decoder: it needs none of them; a
// hand-made `Figure` with a function that returns pixels in the requested size is enough. The size
// it asks for is the size in cells multiplied by the grid's grain (1×2 per cell with block
// characters), and after a change of place it requests again, with the new size; until then it
// paints the old one.
TRIAL("Image: the source is called with the size from the layout, and again after a change of place") {
    Loop loop;
    Sheet sheet;

    const auto asked = std::make_shared<std::vector<ui::Size>>();

    Figure made;

    made.format = Format::png;
    made.size = {16, 12};
    made.fetch = [asked](ui::Size want) {
        asked->push_back(want);

        return ready(Pixels{want, {Still{Bytes(static_cast<std::size_t>(want.wide * want.high) * 4, std::byte{7}),
                                    std::chrono::milliseconds{0}}}});
    };

    const auto one = std::make_shared<ui::Image>();

    one->hold(made);

    same(static_cast<int>(one->format()), static_cast<int>(Format::png));
    ought(one->held());
    same(one->frames(), std::size_t{0});
    ought(asked->empty());

    sheet.children().add(one);

    ought(settled(loop, sheet, *one, {40, 20}));
    same(one->frames(), std::size_t{1});
    same(asked->size(), std::size_t{1});
    ought(asked->front().wide > 0);
    ought(asked->front().high > 0);

    // The same place: nothing new is requested.
    (void)sheet.render({{0, 0}, {40, 20}});
    (void)loop.turn();

    same(asked->size(), std::size_t{1});

    // A smaller place: it requests again, with a smaller size.
    ought(settled(loop, sheet, *one, {20, 10}));
    same(asked->size(), std::size_t{2});
    ought(asked->back().wide < asked->front().wide);

    // Without Unicode the grain is different, so it requests again: once, not on every frame.
    sheet.unicode(false);

    ought(settled(loop, sheet, *one, {20, 10}));
    same(asked->size(), std::size_t{3});

    (void)sheet.render({{0, 0}, {20, 10}});
    (void)loop.turn();
    (void)sheet.render({{0, 0}, {20, 10}});

    same(asked->size(), std::size_t{3});
}

// A refusal by the source (a rejected promise, not an empty result) empties the widget the same
// way: the kind stays, the shape goes, so that the layout stops asking for room.
TRIAL("Image: a rejected promise from the source empties the widget, the kind stays") {
    Loop loop;
    Sheet sheet;

    Figure made;

    made.format = Format::bmp;
    made.size = {16, 12};
    made.fetch = [](ui::Size) {
        return failed<Pixels>(std::make_exception_ptr(std::runtime_error{"no"}));
    };

    const auto one = std::make_shared<ui::Image>();

    one->hold(made);
    ought(one->held());

    sheet.children().add(one);

    ought(settled(loop, sheet, *one, {40, 20}));
    ought(!one->held());
    ought(!one->busy());
    same(static_cast<int>(one->format()), static_cast<int>(Format::bmp));
}

// --- Aspect ratio --------------------------------------------------------------------------------

// A cell is not a square. A 16 by 12 picture with a 10 by 20 cell takes forty cells across and
// fifteen down, because the height is computed from the cell's true proportions.
TRIAL("Image: the aspect ratio is computed from the cell size") {
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);

    same(one->wants({40, 20}).wide, 40);
    same(one->wants({40, 20}).high, 15);
}

// Outside a tree there is nobody to ask about the cell, so the widget assumes one to two, the same
// as when the terminal does not answer `16t`.
TRIAL("Image: without a cell size we assume one to two") {
    const auto one = shown(leaping);

    same(one->wants({40, 20}).wide, 40);
    same(one->wants({40, 20}).high, 15);
}

// --- Placement in the area -----------------------------------------------------------------------

// The preview stands in the middle, as in Midnight Commander, and the rest of the area is left
// unpainted, so the parent's background shows through it.
TRIAL("Image: the picture stands in the middle of the area") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);

    ought(settled(loop, sheet, *one, {40, 20}));

    const Snapshot made = sheet.render({{0, 0}, {40, 20}});

    // Fifteen rows in an area of twenty: two empty at the top, three at the bottom.
    same(static_cast<int>(made.at({0, 0}).shown), static_cast<int>(Shown::none));
    same(static_cast<int>(made.at({0, 1}).shown), static_cast<int>(Shown::none));
    ought(made.at({0, 2}).shown != Shown::none);
    ought(made.at({0, 16}).shown != Shown::none);
    same(static_cast<int>(made.at({0, 17}).shown), static_cast<int>(Shown::none));
    same(static_cast<int>(made.at({0, 19}).shown), static_cast<int>(Shown::none));
}

// The edge of a picture in a window has the window's background. A partly covered cell has no paper
// from the picture's role, so it goes as ink and takes the background of the frame it stands in.
// Left transparent all the way to the terminal, it would show the terminal's own background
// instead.
TRIAL("Image: the picture's edge takes the background of the window it stands in") {
    Sheet sheet;

    Style frame{};

    frame.background = Color{false, 0, 0, 160};

    sheet.skin(ui::Skin{{{"talcite.frame", frame}}});

    // A 4x6 picture: the left column red, the rest transparent; in sextants, two cells with a
    // silhouette.
    Bytes bytes(4 * 6 * 4, std::byte{0});

    for (int y = 0; y < 6; ++y) {
        const std::size_t at = static_cast<std::size_t>(y * 4) * 4;

        bytes[at] = std::byte{200};
        bytes[at + 3] = std::byte{255};
    }

    const auto one = std::make_shared<ui::Image>();

    one->hold(Pixels{{4, 6}, {Still{bytes, std::chrono::milliseconds{0}}}});

    const auto box = std::make_shared<ui::Frame>(std::string{});

    box->children().add(one);
    sheet.children().add(box);

    const Snapshot made = sheet.render({{0, 0}, {6, 4}});

    int seen = 0;

    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 6; ++x) {
            const Cell cell = made.at({x, y});

            if (cell.style.foreground != (Color{false, 200, 0, 0})) continue;

            ++seen;

            same(cell.style.background, (Color{false, 0, 0, 160}));
        }

    ought(seen > 0);
}

// --- Clock ---------------------------------------------------------------------------------------

TRIAL("Image: before it is drawn there is nothing to ask for") {
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);

    ought(!waiting(sheet).has_value());
}

TRIAL("Image: a decoded animation asks to be woken up") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);
    ought(settled(loop, sheet, *one, {40, 20}));

    (void)sheet.render({{0, 0}, {40, 20}});

    ought(waiting(sheet).has_value());
    ought(*waiting(sheet) > std::chrono::milliseconds{0});
    ought(*waiting(sheet) <= std::chrono::milliseconds{40});
}

// Setting the frame is seeking: it moves the start of the countdown as if the animation had just
// reached that frame. Thanks to that, the timing is checked without waiting: after seeking, exactly
// as much time is left as the frame lasts.
TRIAL("Image: seeking sets the frame together with the time") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);
    ought(settled(loop, sheet, *one, {40, 20}));

    same(one->frames(), std::size_t{4});

    const int these[4] = {40, 100, 100, 120};

    bool held = true;

    // What is left of the frame is its length less the time since the seek, which the trial
    // measures rather than assumes small: under a sanitizer a frame's drawing alone takes tens of
    // milliseconds.
    for (std::size_t at = 0; at < 4; ++at) {
        const auto began = std::chrono::steady_clock::now();

        one->still(at);

        (void)sheet.render({{0, 0}, {40, 20}});

        const std::optional<std::chrono::milliseconds> left = waiting(sheet);
        const auto spent = std::chrono::ceil<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();

        held = held && one->still() == at;
        held = held && left.has_value() && left->count() <= these[at];
        held = held && left.has_value() && left->count() >= these[at] - spent - 1;
    }

    ought(held);
}

TRIAL("Image: seeking past the end stops at the last frame") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);
    ought(settled(loop, sheet, *one, {40, 20}));

    one->still(9);

    same(one->still(), std::size_t{3});
}

// Pausing freezes the frame that is visible, not frame zero; otherwise a pause would make the
// picture jump.
TRIAL("Image: pausing freezes the visible frame") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);
    ought(settled(loop, sheet, *one, {40, 20}));

    (void)sheet.render({{0, 0}, {40, 20}});

    ought(waiting(sheet).has_value());

    one->still(2);
    one->play(false);

    ought(!one->play());
    same(one->still(), std::size_t{2});
    ought(!waiting(sheet).has_value());
}

// Resuming starts from the frozen frame, not from the one the clock started at `hold` would point
// to; otherwise the picture would jump ahead after a pause by as many frames as time had passed.
// The first frame lasts a hundred milliseconds, which is the margin the check right after resuming
// has.
TRIAL("Image: resuming starts from the frozen frame") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(leaping);

    sheet.children().add(one);
    ought(settled(loop, sheet, *one, {40, 20}));

    one->still(1);
    one->play(false);

    std::this_thread::sleep_for(std::chrono::milliseconds{250});

    one->play(true);

    (void)sheet.render({{0, 0}, {40, 20}});

    ought(one->play());
    same(one->still(), std::size_t{1});
    ought(waiting(sheet).has_value());
}

// The only trial that really waits. The first frame lasts twenty milliseconds, the second three
// seconds: after a hundred milliseconds we are in the second with almost three seconds to spare.
TRIAL("Image: the clock advances the frame by itself") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(quick);

    sheet.children().add(one);
    ought(settled(loop, sheet, *one, {20, 10}));

    one->still(0);

    same(one->still(), std::size_t{0});

    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    same(one->still(), std::size_t{1});

    // Pausing freezes the frame the clock has reached, not the one set last.
    one->play(false);

    same(one->still(), std::size_t{1});
}

// Frames without a delay are not an animation, just a picture. The GIF decoder never produces such
// frames, but a picture made in code can, and then "which frame now" has no answer.
TRIAL("Image: frames without delays are not an animation") {
    Sheet sheet;

    const auto one = std::make_shared<ui::Image>();

    Bytes first(4 * 4, std::byte{200});
    Bytes second(4 * 4, std::byte{40});

    one->hold(Pixels{{2, 2},
                     {Still{std::move(first), std::chrono::milliseconds{0}},
                      Still{std::move(second), std::chrono::milliseconds{0}}}});

    sheet.children().add(one);
    (void)sheet.render({{0, 0}, {20, 10}});

    ought(one->held());
    ought(!waiting(sheet).has_value());
}

// --- Color depth and characters from the root ----------------------------------------------------

// A picture made in code: red at the top, transparent at the bottom, 8×8, as much as one cell of
// blocks holds.
Pixels halved() {
    Bytes made(8 * 8 * 4, std::byte{0});

    for (std::size_t at = 0; at < 8 * 4 * 4; at += 4) {
        made[at] = std::byte{220};
        made[at + 3] = std::byte{255};
    }

    return Pixels{{8, 8}, {Still{std::move(made), std::chrono::milliseconds{0}}}};
}

// The widget takes the character set from the root and recomputes the picture when the root changes
// it: without Unicode a cell has a 2×4 grid and a character from the ASCII ladder, with Unicode
// blocks. The color depth comes from the root too: in full colors the half block has the picture's
// color.
TRIAL("Image: characters and color depth come from the root, a change of characters recomputes the picture") {
    Sheet sheet{{0, 0}, ui::Blocks::basic};

    const auto one = std::make_shared<ui::Image>();

    one->hold(halved());

    sheet.children().add(one);

    const Snapshot drawn_blocks = sheet.render({{0, 0}, {1, 1}});

    same(drawn_blocks.at({0, 0}).letter, U'▀');
    same(drawn_blocks.at({0, 0}).style.foreground, (Color{false, 220, 0, 0}));

    sheet.unicode(false);

    const Snapshot drawn_ascii = sheet.render({{0, 0}, {1, 1}});

    same(drawn_ascii.at({0, 0}).letter, U'=');

    sheet.colors(Colors::mono);
    sheet.unicode(true);

    same(sheet.render({{0, 0}, {1, 1}}).at({0, 0}).letter, U'▀');

    // Color depth from the root: two colors in full color give a half block, while in black and
    // white a picture with transparency (one pixel) is a silhouette only: a full block.
    Bytes two(8 * 8 * 4, std::byte{0});

    for (std::size_t at = 0; at < two.size(); at += 4) {
        two[at + (at < two.size() / 2 ? 0 : 2)] = std::byte{220};
        two[at + 3] = std::byte{255};
    }

    two[3] = std::byte{254};

    one->hold(Pixels{{8, 8}, {Still{std::move(two), std::chrono::milliseconds{0}}}});

    same(sheet.render({{0, 0}, {1, 1}}).at({0, 0}).letter, U'█');

    sheet.colors(Colors::rgb);

    same(sheet.render({{0, 0}, {1, 1}}).at({0, 0}).letter, U'▀');

    one->hold(halved());

    // Outside a tree: full colors and Unicode, so two colors give a half block, not a silhouette.
    const auto alone = std::make_shared<ui::Image>();

    alone->hold(halved());

    same(alone->render({{0, 0}, {1, 1}}).at({0, 0}).letter, U'▀');

    Bytes pair(8 * 8 * 4, std::byte{0});

    for (std::size_t at = 0; at < pair.size(); at += 4) {
        pair[at + (at < pair.size() / 2 ? 0 : 2)] = std::byte{220};
        pair[at + 3] = std::byte{255};
    }

    pair[3] = std::byte{254};

    alone->hold(Pixels{{8, 8}, {Still{std::move(pair), std::chrono::milliseconds{0}}}});

    same(alone->render({{0, 0}, {1, 1}}).at({0, 0}).letter, U'▀');
}

