#include "tal/pixels.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <exception>
#include <optional>
#include <string>
#include <utility>

#include "tal/executor.hpp"
#include "tal/trouble.hpp"

#include "gifting.hpp"
#include "jpeging.hpp"
#include "lighting.hpp"
#include "pnging.hpp"
#include "reducing.hpp"

// Pixels: an image as RGBA frames with their delays, ported from talcite's pixels.cpp and
// picture.cpp. The format and the size come from the header, never from a name: a file manager
// looks at other people's disks, where the extension is sometimes something other than the
// content. Decoding streams frame by frame: GIF through our own decoder, the rest through the port
// `Decoding`, and every frame passes the ceiling here, so every adapter is subject to it alike; a
// frame lives for a moment and is gone once shrunk, so an animation never sits in memory at the
// size of its file.
//
// Scaling averages in linear light (`lighting`): a byte of sRGB is not a brightness, and averaging
// raw bytes darkens every edge into the gray mud by which a downscaled image is known.
namespace tal::media {

namespace {

constexpr std::size_t bands = 4;

// The byte `one` as a number.
constexpr unsigned sample(std::byte one) { return std::to_integer<unsigned>(one); }

// The span of source pixels that falls on the target pixel `at` of `upto`, out of `from`, as
// [near, far): at least one pixel, so that enlarging gives the nearest neighbor, sharp squares,
// which is the right answer for icons and pixel art.
std::pair<std::uint64_t, std::uint64_t> spanned(std::uint64_t at, std::uint64_t from, std::uint64_t upto) {
    const std::uint64_t near = at * from / upto;
    const std::uint64_t far = (at + 1) * from / upto;

    return {near, std::max(near + 1, far)};
}

// Whether `what` starts with `sign`.
bool leads(std::span<const std::byte> what, std::span<const unsigned char> sign) {
    if (what.size() < sign.size()) return false;

    for (std::size_t at = 0; at < sign.size(); ++at)
        if (sample(what[at]) != sign[at]) return false;

    return true;
}

constexpr unsigned char png[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
constexpr unsigned char jpeg[3] = {0xFF, 0xD8, 0xFF};
constexpr unsigned char bmp[2] = {'B', 'M'};
constexpr unsigned char gif[4] = {'G', 'I', 'F', '8'};

// The big-endian number of `much` bytes at `at` of `what`.
std::uint32_t big(std::span<const std::byte> what, std::size_t at, std::size_t much) {
    std::uint32_t made = 0;

    for (std::size_t each = 0; each < much; ++each) made = (made << 8) | sample(what[at + each]);

    return made;
}

// The little-endian number of `much` bytes at `at` of `what`.
std::uint32_t little(std::span<const std::byte> what, std::size_t at, std::size_t much) {
    std::uint32_t made = 0;

    for (std::size_t each = much; each > 0; --each) made = (made << 8) | sample(what[at + each - 1]);

    return made;
}

// The size of `width` by `height`, or nothing when either is zero or above 65535.
std::optional<Size> sized(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0 || width > 65535 || height > 65535) return std::nullopt;

    return Size{width, height};
}

// The RGBA frame `rgba` of `from` resampled to `to` with a box filter: each target pixel the
// average of the source rectangle on it, which takes every pixel, so a thin line does not vanish
// for missing a sample. The average is in linear light and weighted by alpha: a transparent pixel
// carries some color, usually black, which counted alike with the visible ones would outline every
// cut-out shape in dark; a pixel with nothing visible on it stays transparent.
Still resampled(std::span<const std::byte> rgba, Size from, Size to, std::chrono::milliseconds after) {
    const std::array<double, 256>& tone = lighting::straight();
    Still fresh;

    fresh.after = after;
    fresh.rgba.resize(std::size_t{to.width} * to.height * bands);

    for (std::uint64_t y = 0; y < to.height; ++y) {
        const auto [top, bottom] = spanned(y, from.height, to.height);

        for (std::uint64_t x = 0; x < to.width; ++x) {
            const auto [left, right] = spanned(x, from.width, to.width);
            double sums[3] = {0.0, 0.0, 0.0};
            double alpha = 0.0;
            std::uint64_t seen = 0;

            for (std::uint64_t down = top; down < std::min<std::uint64_t>(bottom, from.height); ++down) {
                for (std::uint64_t across = left; across < std::min<std::uint64_t>(right, from.width); ++across) {
                    const auto at = static_cast<std::size_t>((down * from.width + across) * bands);
                    const double weight = static_cast<double>(sample(rgba[at + 3])) / 255.0;

                    for (std::size_t band = 0; band < 3; ++band) sums[band] += tone[sample(rgba[at + band])] * weight;

                    alpha += weight;
                    ++seen;
                }
            }

            const auto into = static_cast<std::size_t>((y * to.width + x) * bands);

            if (seen == 0 || alpha <= 0.0) continue;

            for (std::size_t band = 0; band < 3; ++band) fresh.rgba[into + band] = static_cast<std::byte>(lighting::bent(sums[band] / alpha));

            fresh.rgba[into + 3] = static_cast<std::byte>(std::lround(alpha * 255.0 / static_cast<double>(seen)));
        }
    }

    return fresh;
}

// The name of `format`, for troubles.
std::string named(Format format) {
    switch (format) {
        case Format::png: return "PNG";
        case Format::jpeg: return "JPEG";
        case Format::bmp: return "BMP";
        case Format::gif: return "GIF";
        case Format::none: break;
    }

    return "an unknown format";
}

// Our own decoders as the port: PNG and JPEG; any other format is left to an adapter of its own.
class Own final : public Decoding {
public:
    // The name of the decoders.
    [[nodiscard]] std::string_view name() const override { return "tal"; }

    // Hands the frame of a PNG or a JPEG, whole, to `each`.
    void decoded(std::span<const std::byte> given, const Taking& each) override { decoded(given, each, Size{}); }

    // Hands the frame of a PNG or a JPEG to `each`, reduced as far as it still covers `most`
    // (`reduction`); throws `Trouble` unsupported for another format, and the decoder's own.
    void decoded(std::span<const std::byte> given, const Taking& each, Size most) override {
        switch (format_of(given)) {
        case Format::png: pnging::pnged(given, each, most); return;
        case Format::jpeg: jpeging::jpeged(given, each, most); return;
        default: throw Trouble{Kind::unsupported, "No decoder of " + named(format_of(given)) + "."};
        }
    }
};

}

// An image of `size` made of the RGBA frames `stills`; throws `Trouble` (other) for frames without
// a size or a frame whose length is not the size's.
Pixels::Pixels(Size size, std::vector<Still> stills) : size_(size), stills_(std::move(stills)) {
    if (size_.width == 0 || size_.height == 0) {
        if (!stills_.empty()) throw Trouble{Kind::other, "Frames without a size."};

        size_ = Size{};

        return;
    }

    const std::uint64_t much = std::uint64_t{size_.width} * size_.height * bands;

    for (const Still& one : stills_)
        if (one.rgba.size() != much) throw Trouble{Kind::other, "A frame whose length is not its size's."};
}

// Whether there is anything to show: a size and at least one frame.
bool Pixels::held() const { return size_.width > 0 && size_.height > 0 && !stills_.empty(); }

// Every frame resampled to `to` (see `resampled`).
Pixels Pixels::scaled(Size to) const {
    if (!held() || to.width == 0 || to.height == 0) return {};

    if (to == size_) return *this;

    std::vector<Still> made;

    made.reserve(stills_.size());

    for (const Still& one : stills_) made.push_back(resampled(one.rgba, size_, to, one.after));

    return Pixels{to, std::move(made)};
}

// The image turned as an orientation of EXIF says it is to be shown (1 to 8: as it is, mirrored
// across, turned half round, mirrored down, mirrored along its diagonal, turned a quarter right,
// mirrored along the other diagonal, turned a quarter left), every frame alike; any other value
// leaves it as it is. Each pixel of the result is taken from where the orientation puts it.
Pixels Pixels::oriented(std::uint8_t orientation) const {
    if (!held() || orientation < 2 || orientation > 8) return *this;

    const std::uint32_t w = size_.width;
    const std::uint32_t h = size_.height;
    const bool turned = orientation >= 5;
    const Size to = turned ? Size{h, w} : size_;
    const auto from = [&](std::uint32_t x, std::uint32_t y) -> std::pair<std::uint32_t, std::uint32_t> {
        switch (orientation) {
        case 2: return {w - 1 - x, y};
        case 3: return {w - 1 - x, h - 1 - y};
        case 4: return {x, h - 1 - y};
        case 5: return {y, x};
        case 6: return {y, h - 1 - x};
        case 7: return {w - 1 - y, h - 1 - x};
        default: return {w - 1 - y, x};
        }
    };
    std::vector<Still> made;

    for (const Still& one : stills_) {
        Still fresh{Bytes(one.rgba.size()), one.after};

        for (std::uint32_t y = 0; y < to.height; ++y)
            for (std::uint32_t x = 0; x < to.width; ++x) {
                const auto [sx, sy] = from(x, y);
                const std::size_t source = (std::size_t{sy} * w + sx) * bands;
                const std::size_t target = (std::size_t{y} * to.width + x) * bands;

                std::copy_n(one.rgba.begin() + static_cast<std::ptrdiff_t>(source), bands, fresh.rgba.begin() + static_cast<std::ptrdiff_t>(target));
            }

        made.push_back(std::move(fresh));
    }

    return Pixels{to, std::move(made)};
}

// The format of the image whose first bytes are `head`, or `Format::none`. "BM" alone would match
// many a text file, so BMP needs its whole file header and the start of the bitmap header.
Format format_of(std::span<const std::byte> head) {
    if (leads(head, png)) return Format::png;

    if (leads(head, jpeg)) return Format::jpeg;

    if (leads(head, bmp) && head.size() >= 14 + 12) return Format::bmp;

    if (leads(head, gif) && head.size() >= 6) {
        const unsigned age = sample(head[4]);

        if ((age == '7' || age == '9') && sample(head[5]) == 'a') return Format::gif;
    }

    return Format::none;
}

// The size of the image from its header `head`, before decoding: PNG in IHDR, GIF in the logical
// screen, BMP in the bitmap header (a negative height means top-down; the old OS/2 header has
// sizes of two bytes), JPEG in its SOF marker, after APP and COM blocks with their own lengths.
// Nothing when the size lies beyond what was given: EXIF may be longer than the window read.
std::optional<Size> size_of(std::span<const std::byte> head) {
    switch (format_of(head)) {
        case Format::png:
            if (head.size() < 24) return std::nullopt;

            return sized(big(head, 16, 4), big(head, 20, 4));
        case Format::gif:
            if (head.size() < 10) return std::nullopt;

            return sized(little(head, 6, 2), little(head, 8, 2));
        case Format::bmp: {
            if (little(head, 14, 4) == 12) return sized(little(head, 18, 2), little(head, 20, 2));

            const auto height = static_cast<std::int32_t>(little(head, 22, 4));

            return sized(little(head, 18, 4), height < 0 ? 0U - static_cast<std::uint32_t>(height) : static_cast<std::uint32_t>(height));
        }
        case Format::jpeg: {
            std::size_t at = 2;

            while (at + 4 <= head.size()) {
                if (sample(head[at]) != 0xFF) return std::nullopt;

                const unsigned marker = sample(head[at + 1]);

                if (marker == 0xFF) {
                    ++at;

                    continue;
                }

                if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                    if (at + 9 > head.size()) return std::nullopt;

                    return sized(big(head, at + 7, 2), big(head, at + 5, 2));
                }

                if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
                    at += 2;

                    continue;
                }

                if (marker == 0xD9 || marker == 0xDA) return std::nullopt;

                at += 2 + big(head, at + 2, 2);
            }

            return std::nullopt;
        }
        case Format::none: break;
    }

    return std::nullopt;
}

// The frames of the image `given` handed to `each`: a GIF by our own decoder, the rest by
// `decoding`, which may give them reduced as far as they still cover `most` (zero: whole). A frame
// without pixels or over `ceiling` stops the stream before it reaches `each`. Throws `Trouble`:
// unsupported for no image tal knows or a format with no decoder, and the decoder's own (malformed
// for damage, full for a canvas over the ceiling).
void decoded(std::span<const std::byte> given, const Taking& each, Optional<Decoding> decoding, Size most) {
    const Format format = format_of(given);

    if (format == Format::none) throw Trouble{Kind::unsupported, "Not an image tal knows."};

    if (!each) return;

    const Taking guarded = [&each](std::span<const std::byte> rgba, Size size, std::chrono::milliseconds after) {
        const std::uint64_t much = std::uint64_t{size.width} * size.height;

        if (much == 0 || much > ceiling || rgba.size() != much * bands) return false;

        return each(rgba, size, after);
    };

    if (format == Format::gif) {
        gifting::gifted(given, guarded);

        return;
    }

    if (!decoding) throw Trouble{Kind::unsupported, "No decoder of " + named(format) + "."};

    decoding->decoded(given, guarded, most);
}

// The image `given` decoded on the pool, each frame shrunk (or grown) to `want` at once (the
// decoder told of `want`, so that it may give a frame already reduced towards it), or kept whole
// when `want` is empty, when a later frame of another size takes the first's; at most `most`
// frames, all of them when zero. The frames kept share one budget of `ceiling` pixels, spent on
// size or on time: the first always fits, and the stream stops before one that would not. Throws
// `Trouble` (other) for `want` over the ceiling, and what the streaming `decoded` throws.
Task<Pixels> decoded(Bytes given, Size want, Optional<Decoding> decoding, std::size_t most) {
    if (std::uint64_t{want.width} * want.height > ceiling) throw Trouble{Kind::other, "A size over the ceiling."};

    Executor& back = co_await detail::Here{};
    std::vector<Still> made;
    Size size;
    std::exception_ptr failed;

    co_await to(pool());

    try {
        std::uint64_t spent = 0;

        decoded(
            given,
            [&](std::span<const std::byte> rgba, Size seen, std::chrono::milliseconds after) {
                const Size kept = want.width != 0 && want.height != 0 ? want : made.empty() ? seen : size;
                const std::uint64_t much = std::uint64_t{kept.width} * kept.height;

                if (spent + much > ceiling) return false;

                if (kept == seen)
                    made.push_back(Still{Bytes(rgba.begin(), rgba.end()), after});
                else
                    made.push_back(resampled(rgba, seen, kept, after));

                spent += much;
                size = kept;

                return most == 0 || made.size() < most;
            },
            std::move(decoding), want);
    } catch (...) {
        failed = std::current_exception();
    }

    co_await to(back);

    if (failed) std::rethrow_exception(failed);

    co_return Pixels{size, std::move(made)};
}

// Our own decoders of PNG and JPEG, the port's answer when no adapter is set.
std::shared_ptr<Decoding> Decoding::builtin() {
    static const std::shared_ptr<Decoding> made = std::make_shared<Own>();

    return made;
}

// The dHash of an image's first frame, by which near copies are found: the frame brought down to
// nine by eight (in linear light, as `scaled` does), the luminance of each pixel (Rec. 709 in
// linear light), and for each of the eight rows the eight comparisons of a pixel with the one on
// its left, a bit set where it is brighter, the first comparison the highest bit. Throws `Trouble`
// (other) for an image without pixels.
std::uint64_t hashed(const Pixels& pixels) {
    if (!pixels.held()) throw Trouble{Kind::other, "No pixels to hash."};

    const Pixels small = Pixels{pixels.size(), {pixels.stills().front()}}.scaled(Size{9, 8});
    const Bytes& rgba = small.stills().front().rgba;
    const std::array<double, 256>& tone = lighting::straight();
    const auto light = [&](std::size_t x, std::size_t y) {
        const std::size_t at = (y * 9 + x) * bands;

        return 0.2126 * tone[sample(rgba[at])] + 0.7152 * tone[sample(rgba[at + 1])] + 0.0722 * tone[sample(rgba[at + 2])];
    };
    std::uint64_t made = 0;

    for (std::size_t y = 0; y < 8; ++y)
        for (std::size_t x = 0; x < 8; ++x) made = (made << 1) | (light(x + 1, y) > light(x, y) ? 1u : 0u);

    return made;
}

// How many bits two hashes differ in: the distance of Hamming, small for near copies.
int apart(std::uint64_t one, std::uint64_t other) { return std::popcount(one ^ other); }

// How many times to reduce an image of `source` each way, 1, 2, 4 or 8: the most for which the
// image still covers `most` on both sides (1 for a `most` without size), and at least as many as
// bring it within `ceiling`. Throws `Trouble` (full) for a source over `most_source` pixels, or one
// that an eighth does not bring within the ceiling.
std::uint32_t reduction(Size source, Size most) {
    const std::uint64_t whole = std::uint64_t{source.width} * source.height;

    if (whole > most_source) throw Trouble{Kind::full, "The image is over the ceiling."};

    std::uint32_t made = 1;

    if (most.width != 0 && most.height != 0)
        while (made < 8 && reduced(source, made * 2).width >= most.width && reduced(source, made * 2).height >= most.height) made *= 2;

    while (made < 8 && std::uint64_t{reduced(source, made).width} * reduced(source, made).height > ceiling) made *= 2;

    if (std::uint64_t{reduced(source, made).width} * reduced(source, made).height > ceiling) throw Trouble{Kind::full, "The image is over the ceiling."};

    return made;
}

// The size of an image of `source` reduced `by` times each way, a part of a pixel counting whole.
Size reduced(Size source, std::uint32_t by) { return Size{(source.width + by - 1) / by, (source.height + by - 1) / by}; }

}
