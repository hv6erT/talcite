#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/picture.hpp"
#include "talcite/pixels.hpp"

#include "faking.hpp"

using namespace talcite;

namespace {

// Recognizing the format from the header stays in talcite, since it is eight bytes and not a
// decoder; the decoder lives in talcite-adapters together with its trials (test/gifting.cpp
// there). Here the image seam is checked on a fake that does not read GIF but hands out the frames
// from the header.
//
// Bytes from a string of lowercase hex digits, two digits per byte.
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

constexpr std::string_view leaving =
    "47494638396110000c00810000000000ff000000000000000021ff0b4e45545343415045322e300301000000"
    "21f90404040000002c0000000010000c000008220003080c00a0a0c18305071244c85021c386031f4a9c48b1"
    "a2c58b18336adc2831200021f90405000002002c00000000070006008100000000ff00000000000000081900"
    "01080420a0e0408206050658186020c30005053c8cf830200021f90405010002002c03000200070006008100"
    "00000000ff00000000000008190001080420a0e0408206050658186020c30005053c8cf830200021f904050c"
    "0002002c060004000700060081000000ff000000000000000008190001080420a0e0408206050658186020c3"
    "0005053c8cf83020003b";

}

// --- Recognition ---------------------------------------------------------------------------

TRIAL("guessed: recognizes GIF by both ages of the signature") {
    same(static_cast<int>(format_of(lump(leaving))), static_cast<int>(Format::gif));
    same(static_cast<int>(format_of(lump("474946383761040004000000003b"))), static_cast<int>(Format::gif));
}

TRIAL("guessed: recognizes PNG, JPEG and BMP by the header, without a decoder") {
    same(static_cast<int>(format_of(lump("89504e470d0a1a0a0000000d49484452"))),
         static_cast<int>(Format::png));
    same(static_cast<int>(format_of(lump("ffd8ffe000104a464946"))),
         static_cast<int>(Format::jpeg));
    same(static_cast<int>(format_of(lump("424d3a0000000000000036000000280000000100000001000000"))),
         static_cast<int>(Format::bmp));
}

// Size from the header, before the decoder: PNG from IHDR, GIF from the logical screen, BMP from
// the bitmap header (a negative height means "top down", the old OS/2 header uses two bytes), JPEG
// from the SOF after the APP blocks, walking the markers by their lengths. A truncated header or a
// SOF beyond the given bytes gives nothing, not a guess.
TRIAL("shaped: size from the PNG, GIF, BMP and JPEG header, nothing when it is missing") {
    const std::optional<ui::Size> png = size_of(lump("89504e470d0a1a0a0000000d49484452000002800000016008060000"));

    ought(png.has_value());
    same(png->wide, 640);
    same(png->high, 352);

    const std::optional<ui::Size> gif = size_of(lump(leaving));

    ought(gif.has_value());
    same(gif->wide, 16);
    same(gif->high, 12);

    const std::optional<ui::Size> bmp =
        size_of(lump("424d3a0000000000000036000000280000000300000005000000"));

    ought(bmp.has_value());
    same(bmp->wide, 3);
    same(bmp->high, 5);

    const std::optional<ui::Size> upside =
        size_of(lump("424d3a00000000000000360000002800000003000000fbffffff"));

    ought(upside.has_value());
    same(upside->wide, 3);
    same(upside->high, 5);

    const std::optional<ui::Size> old =
        size_of(lump("424d3a00000000000000360000000c0000000700090001000100"));

    ought(old.has_value());
    same(old->wide, 7);
    same(old->high, 9);

    // JPEG: SOI, APP1 (EXIF, length 16), then SOF0: length, depth, height 16, width 32.
    const std::optional<ui::Size> jpeg = size_of(lump("ffd8"
                                                     "ffe100104578696600000000000000000000"
                                                     "ffc0001108001000200301220002110103110100"));

    ought(jpeg.has_value());
    same(jpeg->wide, 32);
    same(jpeg->high, 16);

    ought(!size_of(lump("ffd8ffe100104578696600000000000000000000")).has_value());
    ought(!size_of(lump("ffd8ffe1001045786966")).has_value());
    ought(!size_of(lump("89504e470d0a1a0a0000000d49484452")).has_value());
    ought(!size_of(lump("47494638396110")).has_value());
    ought(!size_of(lump("424d3a0000000000000036000000280000")).has_value());
    ought(!size_of(lump("89504e470d0a1a0a0000000d494844520000000000000160")).has_value());
    ought(!size_of(lump("hello")).has_value());
}

TRIAL("guessed: BM alone without the rest of the header is not an image") {
    same(static_cast<int>(format_of(lump("424d"))), static_cast<int>(Format::none));
    same(static_cast<int>(format_of(lump("424d3a000000"))), static_cast<int>(Format::none));
}

TRIAL("guessed: garbage and emptiness are not an image") {
    same(static_cast<int>(format_of(Bytes{})), static_cast<int>(Format::none));
    same(static_cast<int>(format_of(lump("47494638"))), static_cast<int>(Format::none));
    same(static_cast<int>(format_of(lump("474946383861"))), static_cast<int>(Format::none));
}

// --- Image seam ----------------------------------------------------------------------------------

TRIAL("Image: an empty seam refuses with Unbound naming the kind") {
    const Seam<ImageProvider> none;

    ought(!none.bound());

    bool amiss = false;

    try {
        (void)none.decoded(lump(leaving));
    } catch (const Unbound& why) {
        amiss = std::string{why.what()} == "No provider for Image.";
    }

    ought(amiss);

    amiss = false;

    try {
        none.decoded(lump(leaving), [](const Bytes&, ui::Size, std::chrono::milliseconds) { return true; });
    } catch (const Unbound&) {
        amiss = true;
    }

    ought(amiss);
}

// The provider's reason comes out as `Refused` with the kind's name in front, in both forms.
TRIAL("Image: a provider's refusal comes out as Refused") {
    const Seam<ImageProvider> images = faking::images();

    same(std::string{images.provider().name()}, std::string{"fake images"});

    bool amiss = false;

    try {
        (void)images.decoded(lump("ffd8ffe000104a464946000102000000"));
    } catch (const Refused& why) {
        amiss = std::string{why.what()} == "Image provider refused: not a GIF the fake would read";
    }

    ought(amiss);
}

TRIAL("Image: collecting the stream gives all frames with size and delays") {
    const Seam<ImageProvider> images = faking::images();

    const Pixels one = images.decoded(lump(leaving));

    ought(one.held());
    same(one.size().wide, 16);
    same(one.size().high, 12);
    same(one.stills().size(), std::size_t{4});
    same(one.stills()[0].after.count(), 40LL);
    same(one.stills()[3].after.count(), 120LL);
    same(one.stills()[0].bytes.size(), std::size_t{16 * 12 * 4});
}

TRIAL("Image: asking for fewer frames stops the unfolding") {
    const Seam<ImageProvider> images = faking::images();

    same(images.decoded(lump(leaving), 1).stills().size(), std::size_t{1});
    same(images.decoded(lump(leaving), 3).stills().size(), std::size_t{3});
    same(images.decoded(lump(leaving), 9).stills().size(), std::size_t{4});
}

// The streaming form hands over a frame and forgets it. False from the caller means "enough" and is
// not an error, so a preview that wants only the first frame does not pay for the rest.
TRIAL("Image: the stream can be stopped, and without an action it does nothing") {
    const Seam<ImageProvider> images = faking::images();

    std::size_t seen = 0;

    images.decoded(lump(leaving), [&](const Bytes& canvas, ui::Size size, std::chrono::milliseconds) {
        ++seen;

        same(size.wide, 16);
        same(canvas.size(), std::size_t{16 * 12 * 4});

        return seen < 2;
    });

    same(seen, std::size_t{2});

    images.decoded(lump(leaving), Taking{});

    ought(true);
}

// The ceiling is enforced by the seam, equally for every provider: a frame above `ceiling` does not
// reach the sink and stops the stream. The fake declares a header size of 65535 by 65535 (seventeen
// terabytes of RGBA, if anyone allocated it) with an empty buffer; the caller's sink must never see
// it.
TRIAL("Image: a frame above the ceiling does not reach the caller") {
    const Seam<ImageProvider> images = faking::images();

    std::size_t seen = 0;

    images.decoded(lump("474946383961ffffffff000000" "21f9040400000000" "3b"),
                   [&](const Bytes&, ui::Size, std::chrono::milliseconds) {
                       ++seen;

                       return true;
                   });

    same(seen, std::size_t{0});
    ought(!images.decoded(lump("474946383961ffffffff000000" "21f9040400000000" "3b")).held());
}

// --- Image as a source ----------------------------------------------------------------------------

// `figured` ties the bytes and the seam into a source: the kind and shape at once, from the header,
// and the pixels on request in the size the caller asks for, already reduced, never at the file's
// size.
TRIAL("Image: figured gives kind and shape at once, and pixels at the requested size") {
    const Figure one = figured(lump(leaving), faking::images());

    same(static_cast<int>(one.format), static_cast<int>(Format::gif));
    same(one.size.wide, 16);
    same(one.size.high, 12);
    ought(static_cast<bool>(one.fetch));

    const Pixels made = one.fetch({8, 6}).await();

    ought(made.held());
    same(made.size().wide, 8);
    same(made.size().high, 6);
    same(made.stills().size(), std::size_t{4});
    same(made.stills()[3].after.count(), 120LL);
    same(made.stills()[0].bytes.size(), std::size_t{8 * 6 * 4});
}

// The provider's refusal is not lost in the source: the promise is rejected with the same `Refused`
// that the seam throws, so whoever wants the reason has it. The kind still comes from the header.
TRIAL("Image: figured rejects the promise when the provider refuses") {
    const Figure one = figured(lump("ffd8ffe000104a464946000102000000"), faking::images());

    same(static_cast<int>(one.format), static_cast<int>(Format::jpeg));

    bool amiss = false;

    try {
        (void)one.fetch({8, 6}).await();
    } catch (const Refused& why) {
        amiss = std::string{why.what()} == "Image provider refused: not a GIF the fake would read";
    }

    ought(amiss);
}

// Without a provider there is nothing to promise: the shape is empty, so that the layout asks for no
// room, and the source rejects with `Unbound` instead of pretending to have something.
TRIAL("Image: figured without a provider has no shape and rejects with Unbound") {
    const Figure one = figured(lump(leaving), Seam<ImageProvider>{});

    same(static_cast<int>(one.format), static_cast<int>(Format::gif));
    same(one.size.wide, 0);
    same(one.size.high, 0);

    bool amiss = false;

    try {
        (void)one.fetch({8, 6}).await();
    } catch (const Unbound&) {
        amiss = true;
    }

    ought(amiss);
}
