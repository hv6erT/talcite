#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "tal/executor.hpp"
#include "tal/pixels.hpp"
#include "tal/port.hpp"
#include "tal/trouble.hpp"

#include "foreign.hpp"
#include "tal/testing.hpp"

// Pixels: the format and the size from the header, frames and their scaling in linear light, and
// decoding through the port `Decoding` on a fake that hands out the frames it was given, so that
// what tal does around any decoder is checked apart from any decoder.

namespace {

using tal::Bytes;
using tal::media::Format;
using tal::media::Pixels;
using tal::media::Size;
using tal::media::Still;

using namespace std::chrono_literals;

// Bytes from a string of lower-case hexadecimal digits, two digits a byte.
Bytes lump(std::string_view hex) {
    Bytes made;

    made.reserve(hex.size() / 2);

    const auto value = [](char one) { return one >= '0' && one <= '9' ? one - '0' : one - 'a' + 10; };

    for (std::size_t at = 0; at + 1 < hex.size(); at += 2)
        made.push_back(static_cast<std::byte>(value(hex[at]) * 16 + value(hex[at + 1])));

    return made;
}

// Bytes of the numbers `these`, written as numbers because that is how the trials read.
Bytes lump(const std::vector<std::uint8_t>& these) {
    Bytes made;

    made.reserve(these.size());

    for (const std::uint8_t one : these) made.push_back(std::byte{one});

    return made;
}

// An image of `size` made of the pixels `these`, four numbers a pixel, shown for `after`.
Pixels made(Size size, const std::vector<std::uint8_t>& these, std::chrono::milliseconds after = 0ms) {
    return Pixels{size, {Still{lump(these), after}}};
}

// Band `which` (0 red, 1 green, 2 blue, 3 alpha) of the pixel at (`x`, `y`) in frame `still`.
int band(const Pixels& one, std::uint32_t x, std::uint32_t y, std::size_t which, std::size_t still = 0) {
    const std::size_t at = (std::size_t{y} * one.size().width + x) * 4;

    return std::to_integer<int>(one.stills()[still].rgba[at + which]);
}

// The format of `hex` as a number, for comparing.
int format_of(std::string_view hex) { return static_cast<int>(tal::media::format_of(lump(hex))); }

// The size from the header `hex`, or {0, 0} when there is none.
Size size_of(std::string_view hex) { return tal::media::size_of(lump(hex)).value_or(Size{}); }

// What a fake decoder hands out: a frame of `size`, a buffer of `length` bytes (the size's when
// zero) filled with `tone`, shown for `after`.
struct Frame {
    Size size;
    std::uint8_t tone = 0;
    std::chrono::milliseconds after{0};
    std::size_t length = 0;
};

// A decoder that takes every image tal knows and hands out the frames it was given, whatever the
// bytes, or throws `fails` when it is set; it remembers how many frames the caller took and the
// thread it ran on.
class Faking : public tal::media::Decoding {
public:
    explicit Faking(std::vector<Frame> frames, std::optional<tal::Trouble> fails = std::nullopt)
        : frames_(std::move(frames)), fails_(std::move(fails)) {}

    std::string_view name() const override { return "fake"; }

    bool takes(std::span<const std::byte> head) const override { return tal::media::format_of(head) != Format::none; }

    void decoded(std::span<const std::byte>, const tal::media::Taking& each) override {
        ran = std::this_thread::get_id();

        if (fails_) throw *fails_;

        for (const Frame& one : frames_) {
            const std::size_t length = one.length != 0 ? one.length : std::size_t{one.size.width} * one.size.height * 4;
            const Bytes canvas(length, std::byte{one.tone});

            ++handed;

            if (!each(canvas, one.size, one.after)) return;
        }
    }

    std::size_t handed = 0;
    std::thread::id ran;

private:
    std::vector<Frame> frames_;
    std::optional<tal::Trouble> fails_;
};

constexpr std::string_view png = "89504e470d0a1a0a0000000d49484452000002800000016008060000";

// The image `given` decoded by `decoding` at `want`, at most `most` frames.
Pixels pixels(const std::shared_ptr<Faking>& decoding, Size want = {}, std::size_t most = 0, const Bytes& given = lump(png)) {
    tal::Loop loop;

    return tal::wait(loop, tal::media::decoded(given, want, tal::Optional<tal::media::Decoding>{decoding}, most));
}

}

// --- Recognition -------------------------------------------------------------------------------

TRIAL("Pixels: the format from the header, GIF of both ages, PNG, JPEG and BMP, and nothing else") {
    SAME(format_of("474946383961040004000000003b"), static_cast<int>(Format::gif));
    SAME(format_of("474946383761040004000000003b"), static_cast<int>(Format::gif));
    SAME(format_of("89504e470d0a1a0a0000000d49484452"), static_cast<int>(Format::png));
    SAME(format_of("ffd8ffe000104a464946"), static_cast<int>(Format::jpeg));
    SAME(format_of("424d3a0000000000000036000000280000000100000001000000"), static_cast<int>(Format::bmp));
    SAME(format_of("424d"), static_cast<int>(Format::none));
    SAME(format_of("424d3a000000"), static_cast<int>(Format::none));
    SAME(format_of(""), static_cast<int>(Format::none));
    SAME(format_of("47494638"), static_cast<int>(Format::none));
    SAME(format_of("474946383861"), static_cast<int>(Format::none));
    SAME(format_of("474946383962"), static_cast<int>(Format::none));
    SAME(format_of("89504e470d0a1a"), static_cast<int>(Format::none));
    SAME(format_of("ffd8fe"), static_cast<int>(Format::none));
}

// Size from the header, before the decoder: PNG from IHDR, GIF from the logical screen, BMP from
// the bitmap header (a negative height means "top down", the old OS/2 header uses two bytes), JPEG
// from the SOF after the APP blocks, walking the markers by their lengths. A truncated header or a
// SOF beyond the given bytes gives nothing, not a guess; so do sizes of zero or over 65535.
TRIAL("Pixels: the size from the PNG, GIF, BMP and JPEG header, nothing when it is missing") {
    OUGHT(size_of(png) == (Size{640, 352}));
    OUGHT(size_of("474946383961100c00000000003b") == Size{});
    OUGHT(size_of("47494638396110000c000000003b") == (Size{16, 12}));
    OUGHT(size_of("424d3a0000000000000036000000280000000300000005000000") == (Size{3, 5}));
    OUGHT(size_of("424d3a00000000000000360000002800000003000000fbffffff") == (Size{3, 5}));
    OUGHT(size_of("424d3a00000000000000360000000c0000000700090001000100") == (Size{7, 9}));

    // SOI, APP1 (EXIF, length 16), then SOF0: length, depth, height 16, width 32; fill bytes and
    // markers without a length on the way.
    OUGHT(size_of("ffd8ffe100104578696600000000000000000000ffc0001108001000200301220002110103110100") == (Size{32, 16}));
    OUGHT(size_of("ffd8ffffd0ffe100104578696600000000000000000000ffc2001108001000200301") == (Size{32, 16}));

    // DHT, JPG and DAC sit among the SOF markers and are not one; the image data or the end before
    // a SOF means there is none.
    OUGHT(size_of("ffd8ffc40004aaaaffc1001108000300050301") == (Size{5, 3}));
    OUGHT(size_of("ffd8ffda0004aaaaffc0001108000300050301") == Size{});
    OUGHT(size_of("ffd8ffd90004aaaaffc0001108000300050301") == Size{});
    OUGHT(size_of("ffd8ffe1000478ffc0001108000300") == Size{});
    OUGHT(size_of("ffd8ffe100104578696600000000000000000000") == Size{});
    OUGHT(size_of("ffd8ffe1001045786966") == Size{});
    OUGHT(size_of("ffd8ffe1000400ee0000") == Size{});
    OUGHT(size_of("89504e470d0a1a0a0000000d49484452") == Size{});
    OUGHT(size_of("89504e470d0a1a0a0000000d494844520000000000000160") == Size{});
    OUGHT(size_of("89504e470d0a1a0a0000000d494844520001000000000160") == Size{});
    OUGHT(size_of("47494638396110") == Size{});
    OUGHT(size_of("424d3a0000000000000036000000280000") == Size{});
    OUGHT(size_of("41") == Size{});
}

// The header read within the bytes given and nowhere past them: each sample is cut from a longer
// buffer one byte short of what it needs, where the byte after it would complete it. Then the
// JPEG markers: sizes over 255, a byte that is no marker, JPG (0xC8) skipped by its length and
// SOF15 (0xCF) taken; and a PNG one pixel over the largest side.
TRIAL("Pixels: the header within the bytes given, the markers of JPEG, the largest side") {
    const auto format_in = [](std::string_view hex, std::size_t much) {
        const Bytes whole = lump(hex);

        return static_cast<int>(tal::media::format_of(std::span<const std::byte>{whole}.first(much)));
    };
    const auto size_in = [](std::string_view hex, std::size_t much) {
        const Bytes whole = lump(hex);

        return tal::media::size_of(std::span<const std::byte>{whole}.first(much)).value_or(Size{});
    };
    constexpr std::string_view bmp = "424d3a0000000000000036000000280000000300000005000000";
    constexpr std::string_view gif = "47494638396110000c000000003b";
    constexpr std::string_view jpeg = "ffd8ffe000044a46ffc0001108012003000301";

    SAME(format_in(png, 7), static_cast<int>(Format::none));
    SAME(format_in(png, 8), static_cast<int>(Format::png));
    SAME(format_in(bmp, 25), static_cast<int>(Format::none));
    SAME(format_in(bmp, 26), static_cast<int>(Format::bmp));
    SAME(format_in(gif, 5), static_cast<int>(Format::none));
    OUGHT(size_in(png, 23) == Size{});
    OUGHT(size_in(png, 24) == (Size{640, 352}));
    OUGHT(size_in(gif, 9) == Size{});
    OUGHT(size_in(gif, 10) == (Size{16, 12}));
    OUGHT(size_in(jpeg, 16) == Size{});
    OUGHT(size_in(jpeg, 17) == (Size{768, 288}));
    OUGHT(size_of("ffd8ffe00004aaaaab" "c0001108000300050301") == Size{});
    OUGHT(size_of("ffd8ffc80004aaaaffc1001108000300050301") == (Size{5, 3}));
    OUGHT(size_of("ffd8ffcf001108000700090301") == (Size{9, 7}));
    OUGHT(size_of("89504e470d0a1a0a0000000d494844520000ffff0000ffff08060000") == (Size{65535, 65535}));
    OUGHT(size_of("89504e470d0a1a0a0000000d494844520000ffff0001000008060000") == Size{});
}

// --- Construction -------------------------------------------------------------------------------

TRIAL("Pixels: an empty one holds nothing; frames keep their delays; a wrong frame is the caller's fault") {
    const Pixels none;

    OUGHT(!none.held());
    OUGHT(none.size() == Size{});
    OUGHT(none.stills().empty());
    OUGHT(!Pixels(Size{2, 2}, {}).held());
    OUGHT(Pixels(Size{0, 3}, {}).size() == Size{});

    const Pixels two{{1, 1}, {Still{lump({1, 2, 3, 255}), 40ms}, Still{lump({4, 5, 6, 255}), 80ms}}};

    OUGHT(two.held());
    SAME(two.stills().size(), std::size_t{2});
    SAME(two.stills()[1].after.count(), 80LL);

    const auto refused = [](Size size, std::size_t length) {
        try {
            (void)Pixels(size, {Still{Bytes(length), 0ms}});
        } catch (const tal::Trouble& why) {
            return std::string{tal::spoken(why.kind())} + ": " + why.what();
        }

        return std::string{"none"};
    };

    SAME(refused({2, 2}, 8), std::string{"other: A frame whose length is not its size's."});
    SAME(refused({2, 2}, 17), std::string{"other: A frame whose length is not its size's."});
    SAME(refused({0, 0}, 0), std::string{"other: Frames without a size."});
    SAME(refused({3, 0}, 0), std::string{"other: Frames without a size."});
    SAME(refused({2, 2}, 16), std::string{"none"});
}

// --- Linear light and alpha ---------------------------------------------------------------------
//
// A byte in the file is not brightness; it carries a value along the gamma curve. The average
// brightness of black and white is 0.5, and the byte for 0.5 is 188, not 127. A transparent pixel
// carries some color, usually black; counted on a par with the visible ones it outlines every
// cut-out shape in dark, so the color is weighted by alpha and divided by it.

TRIAL("Pixels: black and white give 188 at half; a uniform color survives unchanged") {
    const Pixels half = made({2, 1}, {0, 0, 0, 255, 255, 255, 255, 255}).scaled({1, 1});

    SAME(band(half, 0, 0, 0), 188);
    SAME(band(half, 0, 0, 3), 255);

    std::vector<std::uint8_t> red(64, 0);

    for (std::size_t at = 0; at < red.size(); at += 4) {
        red[at] = 200;
        red[at + 3] = 255;
    }

    const Pixels small = made({4, 4}, red).scaled({2, 2});

    SAME(band(small, 0, 0, 0), 200);
    SAME(band(small, 1, 1, 0), 200);
    SAME(band(small, 1, 1, 1), 0);
    SAME(band(small, 1, 0, 3), 255);
}

TRIAL("Pixels: a transparent neighbor does not darken the color; pure transparency stays clear") {
    const Pixels half = made({2, 1}, {255, 0, 0, 255, 0, 0, 0, 0}).scaled({1, 1});

    SAME(band(half, 0, 0, 0), 255);
    SAME(band(half, 0, 0, 1), 0);
    SAME(band(half, 0, 0, 3), 128);

    const Pixels faint = made({2, 1}, {255, 0, 0, 51, 0, 0, 255, 204}).scaled({1, 1});

    SAME(band(faint, 0, 0, 0), 124);
    SAME(band(faint, 0, 0, 2), 231);
    SAME(band(faint, 0, 0, 3), 128);

    const Pixels clear = made({2, 1}, {255, 0, 0, 0, 0, 255, 0, 0}).scaled({1, 1});

    SAME(band(clear, 0, 0, 3), 0);
    SAME(band(clear, 0, 0, 0), 0);
    SAME(band(clear, 0, 0, 1), 0);
}

// --- Scaling ------------------------------------------------------------------------------------

// The box filter takes all the source pixels, so a thin bright line does not vanish just because
// it missed the sample; upscaling multiplies pixels, sharp squares, the right answer for an icon.
TRIAL("Pixels: a thin line does not vanish; squares merge evenly; upscaling multiplies pixels") {
    const Pixels line = made({4, 1}, {0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255}).scaled({1, 1});

    SAME(band(line, 0, 0, 0), 137);

    const Pixels square = made({2, 2}, {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255}).scaled({1, 1});

    OUGHT(square.size() == (Size{1, 1}));
    SAME(band(square, 0, 0, 0), 255);

    const Pixels big = made({2, 1}, {10, 0, 0, 255, 200, 0, 0, 255}).scaled({4, 1});

    OUGHT(big.size() == (Size{4, 1}));
    SAME(band(big, 0, 0, 0), 10);
    SAME(band(big, 1, 0, 0), 10);
    SAME(band(big, 2, 0, 0), 200);
    SAME(band(big, 3, 0, 0), 200);

    const Pixels uneven = made({3, 1}, {0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255}).scaled({2, 1});

    SAME(band(uneven, 0, 0, 0), 0);
    SAME(band(uneven, 1, 0, 0), 188);

    const Pixels tall = made({1, 3}, {0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255}).scaled({1, 2});

    SAME(band(tall, 0, 0, 0), 0);
    SAME(band(tall, 0, 1, 0), 188);
}

TRIAL("Pixels: the same size gives the same image, clear colors too; a zero target nothing; delays survive") {
    const Pixels one = made({2, 1}, {1, 2, 3, 255, 4, 5, 6, 255});

    SAME(band(one.scaled({2, 1}), 1, 0, 1), 5);
    SAME(band(made({1, 1}, {9, 8, 7, 0}).scaled({1, 1}), 0, 0, 0), 9);
    OUGHT(!one.scaled({0, 4}).held());
    OUGHT(!one.scaled({4, 0}).held());
    OUGHT(!Pixels{}.scaled({4, 4}).held());

    const Pixels two{{2, 1},
                     {Still{lump({0, 0, 0, 255, 255, 255, 255, 255}), 40ms},
                      Still{lump({255, 255, 255, 255, 0, 0, 0, 255}), 90ms}}};
    const Pixels half = two.scaled({1, 1});

    SAME(half.stills().size(), std::size_t{2});
    SAME(half.stills()[0].after.count(), 40LL);
    SAME(half.stills()[1].after.count(), 90LL);
    SAME(band(half, 0, 0, 0, 1), 188);
}

// --- Decoding through the port -----------------------------------------------------------------

// Without a decoder for the format there is nothing to guess with: unsupported, naming it; what no
// image starts with is unsupported before anyone is asked. The decoder's own trouble passes whole.
TRIAL("Pixels: no decoder is unsupported, not an image is unsupported, a decoder's trouble passes") {
    const auto refused = [](const std::shared_ptr<Faking>& decoding, std::string_view hex) {
        try {
            tal::Loop loop;

            (void)tal::wait(loop, tal::media::decoded(lump(hex), Size{}, tal::Optional<tal::media::Decoding>{decoding}));
        } catch (const tal::Trouble& why) {
            return std::string{tal::spoken(why.kind())} + ": " + why.what();
        }

        return std::string{"none"};
    };
    const auto broken = std::make_shared<Faking>(std::vector<Frame>{}, tal::Trouble{tal::Kind::malformed, "Torn."});

    SAME(refused(nullptr, png), std::string{"unsupported: No decoder of PNG."});
    SAME(refused(nullptr, "ffd8ffe000104a464946"), std::string{"unsupported: No decoder of JPEG."});
    SAME(refused(nullptr, "424d3a0000000000000036000000280000000100000001000000"), std::string{"unsupported: No decoder of BMP."});
    SAME(refused(broken, "0102"), std::string{"unsupported: Not an image tal knows."});
    SAME(broken->handed, std::size_t{0});
    SAME(refused(broken, png), std::string{"malformed: Torn."});
    SAME(refused(std::make_shared<Faking>(std::vector<Frame>{}), png), std::string{"none"});
}

// The default decoder is the one set in `defaults()`, and the decoding runs on the pool, never on
// the caller's thread, which may be the one drawing the screen.
TRIAL("Pixels: decoding takes the default decoder, runs on the pool and gives every frame") {
    const auto decoding = std::make_shared<Faking>(std::vector<Frame>{{{2, 1}, 7, 40ms}, {{2, 1}, 9, 120ms}});

    tal::defaults().set<tal::media::Decoding>(decoding);

    tal::Loop loop;
    const Pixels one = tal::wait(loop, tal::media::decoded(lump(png)));

    tal::defaults().clear<tal::media::Decoding>();

    OUGHT(one.size() == (Size{2, 1}));
    SAME(one.stills().size(), std::size_t{2});
    SAME(one.stills()[0].after.count(), 40LL);
    SAME(one.stills()[1].after.count(), 120LL);
    SAME(band(one, 1, 0, 2, 1), 9);
    OUGHT(decoding->ran != std::this_thread::get_id());
    OUGHT(decoding->ran != std::thread::id{});
}

// A frame without pixels, over the ceiling or whose buffer is not its size's never reaches the
// caller and stops the stream: the ceiling is held here for every decoder alike.
TRIAL("Pixels: a frame over the ceiling, of nothing or of a wrong length stops the stream") {
    std::size_t seen = 0;
    const tal::media::Taking counting = [&seen](std::span<const std::byte>, Size, std::chrono::milliseconds) {
        ++seen;

        return true;
    };
    const auto stream = [&](std::vector<Frame> frames) {
        seen = 0;

        const auto decoding = std::make_shared<Faking>(std::move(frames));

        tal::media::decoded(lump(png), counting, tal::Optional<tal::media::Decoding>{decoding});

        return std::pair{seen, decoding->handed};
    };

    OUGHT(stream({{{1, 1}}, {{4000, 2001}, 0, 0ms, 4}, {{1, 1}}}) == (std::pair<std::size_t, std::size_t>{1, 2}));
    OUGHT(stream({{{1, 1}}, {{0, 3}}, {{1, 1}}}) == (std::pair<std::size_t, std::size_t>{1, 2}));
    OUGHT(stream({{{1, 1}}, {{2, 2}, 0, 0ms, 15}, {{1, 1}}}) == (std::pair<std::size_t, std::size_t>{1, 2}));
    OUGHT(stream({{{4000, 2000}}, {{1, 1}}}) == (std::pair<std::size_t, std::size_t>{2, 2}));

    const auto decoding = std::make_shared<Faking>(std::vector<Frame>{{{1, 1}}});

    tal::media::decoded(lump(png), tal::media::Taking{}, tal::Optional<tal::media::Decoding>{decoding});

    SAME(decoding->handed, std::size_t{0});
}

// Frames are shrunk as they come; a later frame of another size takes the first's when no size was
// asked for, so the frames of one image always share one; `most` stops the unfolding.
TRIAL("Pixels: frames shrunk as they come, of one size, and no more than asked for") {
    const auto decoding = std::make_shared<Faking>(std::vector<Frame>{{{4, 2}, 200, 40ms}, {{2, 2}, 100}, {{8, 8}, 50}});
    const Pixels small = pixels(decoding, {2, 1});

    OUGHT(small.size() == (Size{2, 1}));
    SAME(small.stills().size(), std::size_t{3});
    SAME(band(small, 1, 0, 0, 0), 200);
    SAME(small.stills()[0].after.count(), 40LL);

    const Pixels whole = pixels(decoding);

    OUGHT(whole.size() == (Size{4, 2}));
    SAME(whole.stills().size(), std::size_t{3});
    SAME(whole.stills()[2].rgba.size(), std::size_t{4 * 2 * 4});
    SAME(band(whole, 3, 1, 0, 1), 100);

    OUGHT(pixels(decoding, {0, 5}).size() == (Size{4, 2}));
    OUGHT(pixels(decoding, {5, 0}).size() == (Size{4, 2}));

    decoding->handed = 0;

    SAME(pixels(decoding, {}, 1).stills().size(), std::size_t{1});
    SAME(decoding->handed, std::size_t{1});
    SAME(pixels(decoding, {}, 2).stills().size(), std::size_t{2});
    SAME(pixels(decoding, {}, 5).stills().size(), std::size_t{3});
}

// The frames kept share one budget of the ceiling, spent on size or on time: the first always
// fits, and the stream stops before the frame that would not, so a long animation at full size
// does not fill the memory, while the same one shrunk keeps every frame. A size asked for over the
// ceiling is the caller's mistake.
TRIAL("Pixels: the frames kept share the budget of the ceiling") {
    const auto decoding = std::make_shared<Faking>(std::vector<Frame>{{{2000, 1600}}, {{2000, 1600}}, {{2000, 1600}}});

    SAME(pixels(decoding).stills().size(), std::size_t{2});
    SAME(decoding->handed, std::size_t{3});
    SAME(pixels(decoding, {200, 160}).stills().size(), std::size_t{3});

    const auto single = std::make_shared<Faking>(std::vector<Frame>{{{4000, 2000}}, {{1, 1}}});

    SAME(pixels(single).stills().size(), std::size_t{1});

    const auto exact = std::make_shared<Faking>(std::vector<Frame>{{{4000, 1000}}, {{4000, 1000}}, {{1, 1}}});

    SAME(pixels(exact).stills().size(), std::size_t{2});

    bool refused = false;

    try {
        (void)pixels(exact, {4000, 2001});
    } catch (const tal::Trouble& why) {
        refused = why.kind() == tal::Kind::other && std::string{why.what()} == "A size over the ceiling.";
    }

    OUGHT(refused);
    SAME(pixels(exact, {4000, 2000}, 1).stills().size(), std::size_t{1});
}

// --- Chains --------------------------------------------------------------------------------------

namespace {

using tal::media::Chain;
using tal::media::Decoding;

constexpr std::string_view jpeg = "ffd8ffe000104a464946";
constexpr std::string_view gif = "474946383961040004000000003b";
constexpr std::string_view bmp = "424d3a0000000000000036000000280000000100000001000000";
constexpr std::string_view dot = "47494638396101000100800000000000ffffff2c00000000010001000002024401003b";

// A decoder of the formats `formats` that tells the size `size` for every header it takes and
// gives one frame of 1 by 1 in `tone`; it counts the images it decoded.
class Marked final : public Decoding {
public:
    Marked(std::vector<Format> formats, std::uint8_t tone, Size size = {7, 3}) : formats_(std::move(formats)), tone_(tone), size_(size) {}

    std::string_view name() const override { return "marked"; }

    bool takes(std::span<const std::byte> head) const override { return std::ranges::find(formats_, tal::media::format_of(head)) != formats_.end(); }

    std::optional<Size> sized(std::span<const std::byte>) const override { return size_; }

    void decoded(std::span<const std::byte>, const tal::media::Taking& each) override {
        const Bytes one{std::byte{tone_}, std::byte{tone_}, std::byte{tone_}, std::byte{255}};

        ++asked;
        (void)each(one, Size{1, 1}, 0ms);
    }

    std::size_t asked = 0;

private:
    std::vector<Format> formats_;
    std::uint8_t tone_;
    Size size_;
};

// The red of the first pixel of `given` decoded by `decoding`, as a number, or the trouble as its
// kind and message.
std::string toned(const std::shared_ptr<Decoding>& decoding, const Bytes& given) {
    try {
        tal::Loop loop;
        const Pixels made = tal::wait(loop, tal::media::decoded(given, Size{}, tal::Optional<Decoding>{decoding}));

        return std::to_string(band(made, 0, 0, 0));
    } catch (const tal::Trouble& why) {
        return std::string{tal::spoken(why.kind())} + ": " + why.what();
    }
}

// The trouble `call` throws as its kind and message, or "none".
template <typename Call>
std::string refusal(Call call) {
    try {
        call();
    } catch (const tal::Trouble& why) {
        return std::string{tal::spoken(why.kind())} + ": " + why.what();
    }

    return "none";
}

}

// A chain asks its links in their order: the first that takes the image decodes it and tells its
// size; none taking it is unsupported, by the format's name when tal knows it. Chains nest, a
// chain keeps the name it is given, and a link of no decoder is the caller's mistake.
TRIAL("Decoding: a chain asks its links in order, the first that takes the image decodes it") {
    const auto first = std::make_shared<Marked>(std::vector<Format>{Format::jpeg}, 10, Size{5, 4});
    const auto second = std::make_shared<Marked>(std::vector<Format>{Format::jpeg, Format::png}, 20, Size{6, 2});
    const auto chain = std::make_shared<Chain>(std::vector<std::shared_ptr<Decoding>>{first, second});

    SAME(chain->name(), std::string_view{"chain"});
    SAME(toned(chain, lump(jpeg)), std::string{"10"});
    SAME(toned(chain, lump(png)), std::string{"20"});
    SAME(first->asked, std::size_t{1});
    SAME(second->asked, std::size_t{1});
    OUGHT(chain->takes(lump(jpeg)));
    OUGHT(chain->takes(lump(png)));
    OUGHT(!chain->takes(lump(gif)));
    OUGHT(chain->sized(lump(jpeg)) == (Size{5, 4}));
    OUGHT(chain->sized(lump(png)) == (Size{6, 2}));
    OUGHT(!chain->sized(lump(gif)).has_value());
    SAME(toned(chain, lump(gif)), std::string{"unsupported: No decoder of GIF."});
    SAME(toned(chain, lump("0102")), std::string{"unsupported: Not an image tal knows."});
    SAME(refusal([&] { chain->decoded(lump(gif), [](std::span<const std::byte>, Size, std::chrono::milliseconds) { return true; }); }),
         std::string{"unsupported: No decoder of GIF."});
    SAME(refusal([&] { chain->decoded(lump(png), [](std::span<const std::byte>, Size, std::chrono::milliseconds) { return true; }); }),
         std::string{"none"});
    SAME(second->asked, std::size_t{2});

    const auto last = std::make_shared<Marked>(std::vector<Format>{Format::gif, Format::jpeg}, 30);
    const auto outer = std::make_shared<Chain>(std::vector<std::shared_ptr<Decoding>>{chain, last}, "outer");

    SAME(outer->name(), std::string_view{"outer"});
    SAME(toned(outer, lump(gif)), std::string{"30"});
    SAME(toned(outer, lump(jpeg)), std::string{"10"});
    SAME(last->asked, std::size_t{1});
    SAME(refusal([] { (void)Chain{std::vector<std::shared_ptr<Decoding>>{nullptr}}; }), std::string{"other: A link of no decoder."});
}

// Our decoders are three adapters, each taking its own format alone, and the default is their
// chain, named "tal", which takes no BMP. One of them replaced alone leaves the others ours: a
// JPEG goes to the adapter put before the chain, a GIF and its size still to ours.
TRIAL("Decoding: our decoders one by one, chained as the default, one replaceable alone") {
    const std::vector<std::pair<std::shared_ptr<Decoding>, std::string_view>> ours{
        {Decoding::png(), png}, {Decoding::jpeg(), jpeg}, {Decoding::gif(), gif}};

    SAME(Decoding::png()->name(), std::string_view{"png"});
    SAME(Decoding::jpeg()->name(), std::string_view{"jpeg"});
    SAME(Decoding::gif()->name(), std::string_view{"gif"});
    SAME(Decoding::builtin()->name(), std::string_view{"tal"});

    for (const auto& [decoding, own] : ours) {
        for (const std::string_view one : {png, jpeg, gif, bmp}) SAME(decoding->takes(lump(one)), one == own);

        OUGHT(Decoding::builtin()->takes(lump(own)));
    }

    OUGHT(!Decoding::builtin()->takes(lump(bmp)));
    OUGHT(Decoding::builtin()->sized(lump(png)) == (Size{640, 352}));
    OUGHT(Decoding::builtin()->sized(lump(gif)) == (Size{4, 4}));
    SAME(refusal([] { Decoding::png()->decoded(lump(gif), [](std::span<const std::byte>, Size, std::chrono::milliseconds) { return true; }); }),
         std::string{"unsupported: No decoder of GIF."});
    SAME(toned(Decoding::png(), lump(dot)), std::string{"unsupported: No decoder of GIF."});
    SAME(toned(nullptr, lump(dot)), std::string{"unsupported: No decoder of GIF."});

    const auto mine = std::make_shared<Marked>(std::vector<Format>{Format::jpeg}, 40);
    const auto chain = std::make_shared<Chain>(std::vector<std::shared_ptr<Decoding>>{mine, Decoding::builtin()});
    tal::Loop loop;
    const Pixels dotted = tal::wait(loop, tal::media::decoded(lump(dot), Size{}, tal::Optional<Decoding>{chain}));

    SAME(toned(chain, lump(jpeg)), std::string{"40"});
    OUGHT(dotted.size() == (Size{1, 1}));
    OUGHT(chain->sized(lump(gif)) == (Size{4, 4}));
    OUGHT(chain->sized(lump(jpeg)) == (Size{7, 3}));
    SAME(mine->asked, std::size_t{1});
}

// A format no decoder of tal knows reaches the adapter that takes it: tal neither recognizes it nor
// reads its size, ours refuse it, and a chain with its adapter decodes it, set as the default too.
TRIAL("Decoding: a format tal does not know reaches the adapter that takes it") {
    const Bytes image = tal::testing::foreign_image(Size{3, 2}, 77);
    const auto chain = std::make_shared<Chain>(std::vector<std::shared_ptr<Decoding>>{Decoding::builtin(), tal::testing::foreign()});
    tal::Loop loop;

    SAME(static_cast<int>(tal::media::format_of(image)), static_cast<int>(Format::none));
    OUGHT(!tal::media::size_of(image).has_value());
    SAME(toned(Decoding::builtin(), image), std::string{"unsupported: Not an image tal knows."});
    OUGHT(chain->sized(image) == (Size{3, 2}));

    const Pixels made = tal::wait(loop, tal::media::decoded(image, Size{}, tal::Optional<Decoding>{chain}));

    OUGHT(made.size() == (Size{3, 2}));
    SAME(band(made, 2, 1, 0), 77);

    tal::defaults().set<Decoding>(chain);

    const Pixels set = tal::wait(loop, tal::media::decoded(image));

    tal::defaults().clear<Decoding>();

    OUGHT(set.size() == (Size{3, 2}));
}
