#include "talcite/picture.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "lighting.hpp"

namespace talcite {

namespace {

// Whether `what` starts with the `much` bytes of `sign`. Recognition goes by the file's header,
// not its name. A file manager looks at other people's disks, where the extension is sometimes
// something other than the content, and images are exactly where a lie in the name is most common.
[[nodiscard]] bool leads(const Bytes& what, const std::uint8_t* sign, std::size_t much) {
    if (what.size() < much) return false;

    for (std::size_t at = 0; at < much; ++at)
        if (sample(what[at]) != sign[at]) return false;

    return true;
}

constexpr std::uint8_t png[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
constexpr std::uint8_t jpeg[3] = {0xFF, 0xD8, 0xFF};
constexpr std::uint8_t bmp[2] = {'B', 'M'};
constexpr std::uint8_t gif[4] = {'G', 'I', 'F', '8'};

}

// The image format recognized from the leading bytes, or `Format::none`.
Format format_of(const Bytes& what) {
    if (leads(what, png, 8)) return Format::png;

    if (leads(what, jpeg, 3)) return Format::jpeg;

    // "BM" alone is two bytes and would match many a text file, so the whole file header is
    // required as well: fourteen bytes, followed by the bitmap header.
    if (leads(what, bmp, 2) && what.size() >= 14 + 12) return Format::bmp;

    if (leads(what, gif, 4) && what.size() >= 6) {
        const std::uint8_t age = sample(what[4]);

        if ((age == '7' || age == '9') && sample(what[5]) == 'a') return Format::gif;
    }

    return Format::none;
}

namespace {

// A big-endian unsigned number of `much` bytes at `at`.
[[nodiscard]] std::uint32_t big(const Bytes& what, std::size_t at, int much) {
    std::uint32_t made = 0;

    for (int each = 0; each < much; ++each) made = (made << 8) | sample(what[at + static_cast<std::size_t>(each)]);

    return made;
}

// A little-endian unsigned number of `much` bytes at `at`.
[[nodiscard]] std::uint32_t little(const Bytes& what, std::size_t at, int much) {
    std::uint32_t made = 0;

    for (int each = much - 1; each >= 0; --each) made = (made << 8) | sample(what[at + static_cast<std::size_t>(each)]);

    return made;
}

// Dimensions as a size, or nothing when either is zero or above 65535.
[[nodiscard]] std::optional<ui::Size> sized(std::uint32_t wide, std::uint32_t high) {
    if (wide == 0 || high == 0 || wide > 65535 || high > 65535) return std::nullopt;

    return ui::Size{static_cast<int>(wide), static_cast<int>(high)};
}

}

// The dimensions from the header, before decoding, so that the frame for an image can say how
// large it is before the pixels arrive. PNG keeps them in IHDR right after the signature, GIF in
// the logical screen descriptor, BMP in the bitmap header (the height is signed: negative means
// "top-down"), and JPEG in the SOF marker, which stands after APP/COM blocks with their own
// lengths, so we walk the markers until we hit SOF or the given bytes run out. Nothing when the
// dimensions are not within what was given: the caller reads the header through a window, and
// EXIF can be longer than the window.
std::optional<ui::Size> size_of(const Bytes& head) {
    switch (format_of(head)) {
        case Format::png:
            if (head.size() < 24) return std::nullopt;

            return sized(big(head, 16, 4), big(head, 20, 4));

        case Format::gif:
            if (head.size() < 10) return std::nullopt;

            return sized(little(head, 6, 2), little(head, 8, 2));

        case Format::bmp: {
            if (head.size() < 26) return std::nullopt;

            const std::uint32_t told = little(head, 14, 4);

            // The old OS/2 header (12 bytes) has two-byte dimensions, every newer one four-byte.
            if (told == 12) return sized(little(head, 18, 2), little(head, 20, 2));

            const auto high = static_cast<std::int32_t>(little(head, 22, 4));

            return sized(little(head, 18, 4), static_cast<std::uint32_t>(high < 0 ? -high : high));
        }

        case Format::jpeg: {
            std::size_t at = 2;

            while (at + 4 <= head.size()) {
                if (sample(head[at]) != 0xFF) return std::nullopt;

                const std::uint8_t marker = sample(head[at + 1]);

                if (marker == 0xFF) {
                    ++at;
                    continue;
                }

                // SOF0..SOF15 except DHT (C4), JPG (C8) and DAC (CC): height, then width.
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

// The bound image provider; throws `Unbound` when there is none.
//
// This is the third shape of a seam, next to a transformation (`Codec`) and a session
// (`TlsProvider`): a streaming decoder. An image is only read, and frame by frame (a full frame
// exists for a moment and is gone after shrinking), so a `Codec<Pixels>` with the whole animation
// in memory would be the wrong shape. The handle translates the seam's language into talcite's in
// one place, like `Seam<Codec<…>>`: a missing provider is `Unbound`, the provider's message is
// `Refused`, and false from the sink means "enough" and is not an error.
ImageProvider& Seam<ImageProvider>::provider() const {
    if (provider_ == nullptr) throw Unbound{ImageProvider::kind()};

    return *provider_;
}

// Decodes `given` frame by frame into `each`. The ceiling is enforced here, not by the provider,
// so every provider is subject to it equally: a frame above `ceiling` does not reach the caller's
// sink and stops the stream. A provider may check the header earlier and refuse before it unpacks
// anything; that is its own saving, not our duty. Throws `Unbound` without a provider and
// `Refused` when the provider reports an error.
void Seam<ImageProvider>::decoded(const Bytes& given, const Taking& each) const {
    ImageProvider& one = provider();

    if (!each) return;

    const Taking guarded = [&each](const Bytes& canvas, ui::Size size, std::chrono::milliseconds after) {
        const std::size_t much = static_cast<std::size_t>(size.wide) * static_cast<std::size_t>(size.high);

        if (much == 0 || much > ceiling) return false;

        return each(canvas, size, after);
    };

    if (const std::string said = one.decode(given, guarded); !said.empty())
        throw Refused{ImageProvider::kind(), said};
}

// Collects the stream into `Pixels`, at most `most` frames (zero means all). The budget is one and
// is spent either on resolution or on time: the first frame always fits, because the stream would
// have refused a canvas above the ceiling earlier, and every next one must still fit into it.
Pixels Seam<ImageProvider>::decoded(const Bytes& given, std::size_t most) const {
    std::vector<Still> made;
    std::size_t spent = 0;
    ui::Size size{};

    decoded(given, [&](const Bytes& canvas, ui::Size seen, std::chrono::milliseconds after) {
        const std::size_t much =
            static_cast<std::size_t>(seen.wide) * static_cast<std::size_t>(seen.high);

        if (spent + much > ceiling) return false;

        spent += much;
        size = seen;

        made.push_back(Still{canvas, after});

        return most == 0 || made.size() < most;
    });

    if (made.empty()) return {};

    return Pixels{size, std::move(made)};
}

namespace {

// Decodes to the size of the cells. The stream goes through the seam: a full frame exists for a
// moment on the calling thread (a pool thread, because the promise from `figured` runs there) and
// is gone right after shrinking to `want`; the result is `Pixels` at the size of the cells, never
// at the size of the file. An interrupted stream (false from the sink) is not an exception, so the
// ordinary path catches nothing here; a refusal and a missing provider leave as the seam's
// exception, and the promise turns it into a rejection.
[[nodiscard]] Pixels shrunk(const Seam<ImageProvider>& images, const Bytes& raw, ui::Size want) {
    std::vector<Still> these;

    images.decoded(raw, [&](const Bytes& canvas, ui::Size size, std::chrono::milliseconds after) {
        const Pixels one{size, {Still{canvas, after}}};

        these.push_back(one.scaled(want).stills().front());
        these.back().after = after;

        return true;
    });

    if (these.empty()) return {};

    return Pixels{want, std::move(these)};
}

}

// An image as a source, like `Feed` for text. The widget knows neither the seam nor the bytes: it
// gets the format and the shape (for layout, before anything is decoded) and a function it calls
// with the size learned while painting. The seam and the bytes sit in the closure; when the size
// changes, the widget calls it again and decodes anew, because we keep the file, not frames at
// full resolution. The shape comes from the header (`size_of`) only with a bound seam: without a
// provider the image has nothing to promise and the layout should not ask for room. The format
// always stays, so the legend can say "JPEG, no provider".
Figure figured(Bytes raw, Seam<ImageProvider> images) {
    Figure made;

    made.format = format_of(raw);
    made.size = images.bound() ? size_of(raw).value_or(ui::Size{}) : ui::Size{};
    // Decoding and scaling compute, so they run on the `cpu` lane, whose ceiling equals the number
    // of cores. On the ordinary lane ten images scrolled at once would be ten computing threads on
    // a few cores, and each would finish later than if they waited in a queue.
    made.fetch = [raw = std::move(raw), images = std::move(images)](ui::Size want) {
        return run(cpu(), [raw, images, want]() -> Pixels { return shrunk(images, raw, want); });
    };

    return made;
}

}
