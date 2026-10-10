#include "talcite/pixels.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>

#include "lighting.hpp"

namespace talcite {

namespace {

constexpr std::size_t bands = 4;

// The span of source pixels that falls on target cell `at`, as [near, far). At least one pixel,
// because when enlarging the division would come out empty, and then instead of blurring we get
// the nearest neighbor, that is, sharp squares. For an icon and pixel art that is the right
// answer, not negligence: blurred pixel art looks worse than enlarged pixel art.
[[nodiscard]] std::pair<int, int> spanned(int at, int from, int upto) {
    const int near = static_cast<int>(static_cast<std::int64_t>(at) * from / upto);
    const int far = static_cast<int>(static_cast<std::int64_t>(at + 1) * from / upto);

    return {near, std::max(near + 1, far)};
}

}

// An image of size `given` made of RGBA frames. Throws `std::logic_error` when there are frames
// without dimensions or a frame whose length does not match them. Negative dimensions are
// clamped to zero.
Pixels::Pixels(ui::Size given, std::vector<Still> stills)
    : size_{std::max(given.wide, 0), std::max(given.high, 0)}, stills_(std::move(stills)) {
    if (size_.wide == 0 || size_.high == 0) {
        if (!stills_.empty()) throw std::logic_error("talcite::Pixels: frames without dimensions");

        return;
    }

    const std::size_t much = static_cast<std::size_t>(size_.wide) *
                             static_cast<std::size_t>(size_.high) * bands;

    for (const Still& one : stills_)
        if (one.bytes.size() != much)
            throw std::logic_error("talcite::Pixels: the frame does not match the dimensions");
}

// Whether there is anything to show: nonzero dimensions and at least one frame.
bool Pixels::held() const { return size_.wide > 0 && size_.high > 0 && !stills_.empty(); }

// Every frame resampled to `given`, with a box filter: each target cell is the average of the
// source rectangle that falls on it. When shrinking this is the correct answer and the cheapest of
// the correct ones; it takes all the pixels, so a thin line does not vanish just because it missed
// a sample.
//
// We average in linear light and with premultiplied alpha. The second matters as much as the
// first: a transparent pixel carries some color in the file, usually black, and counted equally
// with the visible ones it makes a dark outline around every cut-out shape. That is why the color
// is weighted by alpha and divided by it at the end; a cell without a single visible pixel simply
// stays empty.
Pixels Pixels::scaled(ui::Size given) const {
    const ui::Size want{std::max(given.wide, 0), std::max(given.high, 0)};

    if (!held() || want.wide == 0 || want.high == 0) return {};

    if (want.wide == size_.wide && want.high == size_.high) return *this;

    const std::array<double, 256>& tone = straight();

    std::vector<Still> made;

    made.reserve(stills_.size());

    for (const Still& one : stills_) {
        Still fresh;

        fresh.after = one.after;
        fresh.bytes.resize(static_cast<std::size_t>(want.wide) *
                           static_cast<std::size_t>(want.high) * bands);

        for (int y = 0; y < want.high; ++y) {
            const auto [top, bottom] = spanned(y, size_.high, want.high);

            for (int x = 0; x < want.wide; ++x) {
                const auto [left, right] = spanned(x, size_.wide, want.wide);

                double sums[3] = {0.0, 0.0, 0.0};
                double alpha = 0.0;
                std::size_t seen = 0;

                for (int down = top; down < std::min(bottom, size_.high); ++down) {
                    const std::size_t row = static_cast<std::size_t>(down) *
                                            static_cast<std::size_t>(size_.wide) * bands;

                    for (int across = left; across < std::min(right, size_.wide); ++across) {
                        const std::size_t at = row + static_cast<std::size_t>(across) * bands;

                        const double weight =
                            static_cast<double>(sample(one.bytes[at + 3])) / 255.0;

                        for (std::size_t band = 0; band < 3; ++band)
                            sums[band] += tone[sample(one.bytes[at + band])] * weight;

                        alpha += weight;
                        ++seen;
                    }
                }

                const std::size_t into = (static_cast<std::size_t>(y) *
                                              static_cast<std::size_t>(want.wide) +
                                          static_cast<std::size_t>(x)) *
                                         bands;

                if (seen == 0 || alpha <= 0.0) {
                    fresh.bytes[into + 0] = std::byte{0};
                    fresh.bytes[into + 1] = std::byte{0};
                    fresh.bytes[into + 2] = std::byte{0};
                    fresh.bytes[into + 3] = std::byte{0};

                    continue;
                }

                for (std::size_t band = 0; band < 3; ++band)
                    fresh.bytes[into + band] = stored(bent(sums[band] / alpha));

                fresh.bytes[into + 3] = stored(static_cast<std::uint8_t>(
                    std::lround(alpha * 255.0 / static_cast<double>(seen))));
            }
        }

        made.push_back(std::move(fresh));
    }

    return Pixels{want, std::move(made)};
}

// How many cells to occupy so that the image keeps its proportions. A cell is not a square: on some
// terminals it is 10 by 20 pixels, on others 9 by 21, so without its size a circle comes out as an
// egg. Zero means "the terminal did not say", and then 1:2 is assumed, because that is what the
// overwhelming majority looks like.
//
// First we try to take the whole width and compute the height; when that does not fit into the
// room, we start from the height and compute the width. Always at least one cell, because an image
// occupying zero cells is an image that cannot be seen, and the caller asked for a preview.
ui::Size fitting(ui::Size image, ui::Size room, ui::Size cell) {
    if (image.wide <= 0 || image.high <= 0 || room.wide <= 0 || room.high <= 0) return {};

    const std::int64_t wide = cell.wide > 0 ? cell.wide : 1;
    const std::int64_t high = cell.high > 0 ? cell.high : 2;

    const std::int64_t across = image.wide;
    const std::int64_t down = image.high;

    int columns = room.wide;
    int rows = static_cast<int>(std::max<std::int64_t>(
        (static_cast<std::int64_t>(columns) * wide * down + high * across / 2) / (high * across),
        1));

    if (rows > room.high) {
        rows = room.high;
        columns = static_cast<int>(std::clamp<std::int64_t>(
            (static_cast<std::int64_t>(rows) * high * across + wide * down / 2) / (wide * down), 1,
            room.wide));
    }

    return {columns, rows};
}

}
