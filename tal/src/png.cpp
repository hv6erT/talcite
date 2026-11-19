#include "pnging.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <vector>

#include "tal/trouble.hpp"

#include "deflating.hpp"
#include "inflating.hpp"
#include "lighting.hpp"
#include "reducing.hpp"

// Our own PNG decoder and encoder, built into tal-media. The decoder reads every colour type and
// depth of the standard (gray of 1, 2, 4, 8 and 16 bits, RGB, palette, gray with alpha, RGBA),
// transparency by tRNS, and the interlacing of Adam7, into one frame of RGBA. Sixteen bits keep
// their high byte, as libpng's "strip 16" does; gamma and colour profiles are not applied, as no
// viewer of a file manager does. An animated PNG gives its default image.
//
// A file that is no PNG, a broken header, a palette missing or broken, a check (CRC) that does not
// hold and an image over the limits (`reduction`) are refused before the pixels are allocated. A
// file cut short or whose compressed data breaks gives what came before: the rows decoded stand,
// the rest stays transparent, since the top of a picture is a better answer than nothing, and a
// file manager also looks at files still being copied. The data is inflated in small pieces up to
// what the image needs, so a stream made to inflate without end stops there.
//
// The encoder writes RGBA of eight bits, not interlaced, each row with the filter whose bytes sum
// to the least taken as signed (the rule of libpng), compressed by our own `deflated`.
namespace tal::media::pnging {

namespace {

constexpr std::size_t bands = 4;

// How many compressed bytes the inflater gets at once: a piece inflates to at most about a
// thousand times as much, so the output never runs far past what the image needs.
constexpr std::size_t fed_piece = 1024;

// How long a row may be: an image of eight rows and of a width the reduction allows would
// otherwise hold rows of half a gigabyte each.
constexpr std::uint64_t most_row = std::uint64_t{32} << 20;

// Where each pass of Adam7 starts and how far it steps: x, y, step in x, step in y.
constexpr std::array<std::array<std::uint32_t, 4>, 7> adam7{{{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}}};

// What the header says of the image.
struct Header {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t depth = 0;
    std::uint32_t color = 0;
    bool interlaced = false;
};

// One pass of the image: where its pixels start, how far they step, how many there are across and
// down. A whole image is one pass from the corner with steps of one.
struct Pass {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t step_x = 1;
    std::uint32_t step_y = 1;
    std::uint32_t across = 0;
    std::uint32_t down = 0;
};

// A byte as an unsigned number.
[[nodiscard]] constexpr std::uint32_t sample(std::byte one) { return std::to_integer<std::uint32_t>(one); }

// The big-endian 32-bit number at `at`; the caller made sure the four bytes are there.
[[nodiscard]] std::uint32_t big32(std::span<const std::byte> from, std::size_t at) {
    return (sample(from[at]) << 24) | (sample(from[at + 1]) << 16) | (sample(from[at + 2]) << 8) | sample(from[at + 3]);
}

// How many samples a pixel of the colour type has: gray 1, RGB 3, palette 1, gray with alpha 2,
// RGBA 4; zero for a type the standard does not know.
[[nodiscard]] std::uint32_t channels_of(std::uint32_t color) {
    switch (color) {
    case 0: return 1;
    case 2: return 3;
    case 3: return 1;
    case 4: return 2;
    case 6: return 4;
    default: return 0;
    }
}

// Whether the standard allows the depth for the colour type.
[[nodiscard]] bool allowed(std::uint32_t color, std::uint32_t depth) {
    switch (color) {
    case 0: return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case 3: return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case 2:
    case 4:
    case 6: return depth == 8 || depth == 16;
    default: return false;
    }
}

// The bytes of a row of `across` pixels, without its filter byte.
[[nodiscard]] std::uint64_t row_bytes(std::uint64_t across, const Header& header) {
    return (across * channels_of(header.color) * header.depth + 7) / 8;
}

// The passes of the image: seven for Adam7, those of them that have no pixel left out; one
// otherwise.
[[nodiscard]] std::vector<Pass> passes_of(const Header& header) {
    if (!header.interlaced) return {Pass{0, 0, 1, 1, header.width, header.height}};

    std::vector<Pass> made;

    for (const auto& [x, y, step_x, step_y] : adam7) {
        const std::uint32_t across = header.width > x ? (header.width - x + step_x - 1) / step_x : 0;
        const std::uint32_t down = header.height > y ? (header.height - y + step_y - 1) / step_y : 0;

        if (across != 0 && down != 0) made.push_back(Pass{x, y, step_x, step_y, across, down});
    }

    return made;
}

// The predictor of Paeth: of the left, the upper and the upper left, the one nearest to their
// sum less the upper left, ties to the left, then the upper.
[[nodiscard]] std::uint32_t paeth(std::uint32_t left, std::uint32_t up, std::uint32_t corner) {
    const int guess = static_cast<int>(left) + static_cast<int>(up) - static_cast<int>(corner);
    const int to_left = std::abs(guess - static_cast<int>(left));
    const int to_up = std::abs(guess - static_cast<int>(up));
    const int to_corner = std::abs(guess - static_cast<int>(corner));

    if (to_left <= to_up && to_left <= to_corner) return left;

    return to_up <= to_corner ? up : corner;
}

// Undoes the filter of a row in place, with the row above (all zeros for the first of a pass) and
// the bytes of a pixel, at least one; false for a filter the standard does not know.
[[nodiscard]] bool unfiltered(std::uint32_t filter, std::span<std::uint8_t> row, std::span<const std::uint8_t> above, std::size_t step) {
    for (std::size_t at = 0; at < row.size(); ++at) {
        const std::uint32_t left = at >= step ? row[at - step] : 0;
        const std::uint32_t up = above[at];
        const std::uint32_t corner = at >= step ? above[at - step] : 0;
        std::uint32_t predicted = 0;

        switch (filter) {
        case 0: predicted = 0; break;
        case 1: predicted = left; break;
        case 2: predicted = up; break;
        case 3: predicted = (left + up) / 2; break;
        case 4: predicted = paeth(left, up, corner); break;
        default: return false;
        }

        row[at] = static_cast<std::uint8_t>(row[at] + predicted);
    }

    return true;
}

// The linear light of each byte of sRGB in sixteen bits, for the averages of a reduced image.
[[nodiscard]] const std::array<std::uint32_t, 256>& linear() {
    static const std::array<std::uint32_t, 256> made = [] {
        std::array<std::uint32_t, 256> these{};

        for (std::size_t at = 0; at < these.size(); ++at) these[at] = static_cast<std::uint32_t>(std::lround(lighting::straight()[at] * 65535.0));

        return these;
    }();

    return made;
}

// The canvas of RGBA of one image, with what fills it: the header, the palette and the key of
// transparency, and the rows as they come inflated, each unfiltered and set down as soon as it is
// whole, so the inflated data never stands whole in memory. Reduced `scale` times, each pixel of
// the canvas is the average of its square of the image in linear light, weighted by alpha, as
// `scaled` averages; the sums stay in 32 bits, which 64 pixels of 16 bits times 8 never fill.
class Canvas {
public:
    // Over a header already checked, reduced `scale` times; the canvas starts transparent.
    Canvas(const Header& header, std::uint32_t scale)
        : header_(header), scale_(scale), size_(reduced(Size{header.width, header.height}, scale)), passes_(passes_of(header)) {
        if (scale_ == 1) canvas_.assign(std::size_t{size_.width} * size_.height * bands, std::byte{0});
        else sums_.assign(std::size_t{size_.width} * size_.height * bands, 0);

        started();
    }

    // Takes the palette of PLTE: three bytes an entry, opaque until tRNS says otherwise.
    void paletted(std::span<const std::byte> data);

    // Takes the transparency of tRNS: an alpha for each entry of the palette, or the gray or the
    // RGB that is transparent.
    void keyed(std::span<const std::byte> data);

    // Takes more of the inflated rows; whether it still wants more.
    [[nodiscard]] bool fed(std::span<const std::byte> more);

    // The size of the canvas.
    [[nodiscard]] Size size() const { return size_; }

    // The canvas of RGBA, taken out.
    [[nodiscard]] Bytes done();

private:
    [[nodiscard]] std::uint32_t at(std::span<const std::uint8_t> row, std::uint64_t index) const;

    void put(std::span<const std::uint8_t> row, std::uint32_t pixel, std::uint32_t x, std::uint32_t y);

    [[nodiscard]] std::uint8_t eight(std::uint32_t value) const;

    void started();

    Header header_;
    std::uint32_t scale_;
    Size size_;
    Bytes canvas_;
    std::vector<std::uint32_t> sums_;
    std::array<std::array<std::uint8_t, 4>, 256> palette_{};
    std::size_t entries_ = 0;
    std::optional<std::array<std::uint32_t, 3>> key_;
    std::vector<Pass> passes_;
    std::size_t pass_ = 0;
    std::uint32_t line_ = 0;
    std::vector<std::uint8_t> above_;
    std::vector<std::uint8_t> row_;
    Bytes pending_;
    std::size_t used_ = 0;
    bool over_ = false;
};

// Takes the palette of PLTE: three bytes an entry, opaque until tRNS says otherwise.
void Canvas::paletted(std::span<const std::byte> data) {
    entries_ = data.size() / 3;

    for (std::size_t one = 0; one < entries_; ++one)
        palette_[one] = {static_cast<std::uint8_t>(sample(data[3 * one])), static_cast<std::uint8_t>(sample(data[3 * one + 1])),
                         static_cast<std::uint8_t>(sample(data[3 * one + 2])), 255};
}

// Takes the transparency of tRNS: an alpha for each entry of the palette (those it does not
// reach stay opaque), or the gray or the RGB, in samples of the image's depth, that is
// transparent. A chunk too short for its colour type is left out, as libpng does.
void Canvas::keyed(std::span<const std::byte> data) {
    if (header_.color == 3) {
        for (std::size_t one = 0; one < std::min<std::size_t>(data.size(), 256); ++one) palette_[one][3] = static_cast<std::uint8_t>(sample(data[one]));

        return;
    }

    if (header_.color == 0 && data.size() >= 2) key_ = std::array<std::uint32_t, 3>{(sample(data[0]) << 8) | sample(data[1]), 0, 0};

    if (header_.color == 2 && data.size() >= 6)
        key_ = std::array<std::uint32_t, 3>{(sample(data[0]) << 8) | sample(data[1]), (sample(data[2]) << 8) | sample(data[3]), (sample(data[4]) << 8) | sample(data[5])};
}

// The sample numbered `index` of an unfiltered row, at the image's depth.
std::uint32_t Canvas::at(std::span<const std::uint8_t> row, std::uint64_t index) const {
    if (header_.depth == 16) return (std::uint32_t{row[2 * index]} << 8) | row[2 * index + 1];

    if (header_.depth == 8) return row[index];

    const std::uint64_t bit = index * header_.depth;
    const std::uint32_t mask = (1u << header_.depth) - 1;

    return (row[bit / 8] >> (8 - header_.depth - bit % 8)) & mask;
}

// A sample of the image's depth as eight bits: sixteen keep their high byte, fewer are spread
// over the range (a 1 of one bit is 255).
std::uint8_t Canvas::eight(std::uint32_t value) const {
    if (header_.depth == 16) return static_cast<std::uint8_t>(value >> 8);

    if (header_.depth == 8) return static_cast<std::uint8_t>(value);

    return static_cast<std::uint8_t>(value * 255 / ((1u << header_.depth) - 1));
}

// Sets down the pixel numbered `pixel` of an unfiltered row, of the image at (x, y): written to
// the canvas, or added to the sums of its square when reduced. An index past the palette is
// opaque black, as most readers show it.
void Canvas::put(std::span<const std::uint8_t> row, std::uint32_t pixel, std::uint32_t x, std::uint32_t y) {
    const std::uint64_t first = std::uint64_t{pixel} * channels_of(header_.color);
    std::array<std::uint8_t, 4> made{0, 0, 0, 255};

    switch (header_.color) {
    case 0: {
        const std::uint32_t gray = at(row, first);

        made = {eight(gray), eight(gray), eight(gray), static_cast<std::uint8_t>(key_ && (*key_)[0] == gray ? 0 : 255)};
        break;
    }
    case 2: {
        const std::array<std::uint32_t, 3> rgb{at(row, first), at(row, first + 1), at(row, first + 2)};

        made = {eight(rgb[0]), eight(rgb[1]), eight(rgb[2]), static_cast<std::uint8_t>(key_ && *key_ == rgb ? 0 : 255)};
        break;
    }
    case 3: {
        const std::uint32_t index = at(row, first);

        if (index < entries_) made = palette_[index];

        break;
    }
    case 4: {
        const std::uint32_t gray = at(row, first);

        made = {eight(gray), eight(gray), eight(gray), eight(at(row, first + 1))};
        break;
    }
    default: made = {eight(at(row, first)), eight(at(row, first + 1)), eight(at(row, first + 2)), eight(at(row, first + 3))};
    }

    const std::size_t place = (std::size_t{y / scale_} * size_.width + x / scale_) * bands;

    if (scale_ == 1) {
        for (std::size_t band = 0; band < bands; ++band) canvas_[place + band] = static_cast<std::byte>(made[band]);

        return;
    }

    for (std::size_t band = 0; band < 3; ++band) sums_[place + band] += linear()[made[band]] * made[3];

    sums_[place + 3] += made[3];
}

// Gets ready for the rows of the pass `pass_`: the row above the first is all zeros.
void Canvas::started() {
    if (pass_ >= passes_.size()) return;

    const auto width = static_cast<std::size_t>(row_bytes(passes_[pass_].across, header_));

    above_.assign(width, 0);
    row_.assign(width, 0);
    line_ = 0;
}

// Takes more of the inflated rows: every row made whole is unfiltered and set down, pass after
// pass. A filter the standard does not know ends the filling, and what was filled stays. Whether
// it still wants more.
bool Canvas::fed(std::span<const std::byte> more) {
    if (over_ || pass_ >= passes_.size()) return false;

    pending_.insert(pending_.end(), more.begin(), more.end());

    const std::size_t step = std::max<std::size_t>(1, channels_of(header_.color) * header_.depth / 8);

    while (pass_ < passes_.size()) {
        const Pass& pass = passes_[pass_];
        const std::size_t width = row_.size();

        if (pending_.size() - used_ < width + 1) break;

        const std::uint32_t filter = sample(pending_[used_]);

        for (std::size_t one = 0; one < width; ++one) row_[one] = static_cast<std::uint8_t>(sample(pending_[used_ + 1 + one]));

        used_ += width + 1;

        if (!unfiltered(filter, row_, above_, step)) {
            over_ = true;

            return false;
        }

        for (std::uint32_t pixel = 0; pixel < pass.across; ++pixel) put(row_, pixel, pass.x + pixel * pass.step_x, pass.y + line_ * pass.step_y);

        std::swap(above_, row_);

        if (++line_ == pass.down) {
            ++pass_;
            started();
        }
    }

    if (used_ >= (std::size_t{64} << 10)) {
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(used_));
        used_ = 0;
    }

    return pass_ < passes_.size();
}

// The canvas of RGBA, taken out: reduced, each pixel the average in linear light of what its
// square holds, weighted by alpha, and its alpha the average over the pixels of the image in the
// square; a square with nothing visible stays transparent.
Bytes Canvas::done() {
    if (scale_ == 1) return std::move(canvas_);

    Bytes made(sums_.size(), std::byte{0});

    for (std::uint32_t y = 0; y < size_.height; ++y)
        for (std::uint32_t x = 0; x < size_.width; ++x) {
            const std::size_t place = (std::size_t{y} * size_.width + x) * bands;
            const std::uint32_t alpha = sums_[place + 3];

            if (alpha == 0) continue;

            const std::uint64_t across = std::min<std::uint64_t>(scale_, header_.width - std::uint64_t{x} * scale_);
            const std::uint64_t down = std::min<std::uint64_t>(scale_, header_.height - std::uint64_t{y} * scale_);

            for (std::size_t band = 0; band < 3; ++band)
                made[place + band] = static_cast<std::byte>(lighting::bent(static_cast<double>(sums_[place + band]) / alpha / 65535.0));

            made[place + 3] = static_cast<std::byte>(std::lround(static_cast<double>(alpha) / static_cast<double>(across * down)));
        }

    return made;
}

// The header of IHDR checked: a size of at least one pixel, below 2^31 each way as the standard
// asks, a colour type and depth it allows, compression and filtering of method 0, interlacing
// none or Adam7. Throws `Trouble` malformed for any other.
[[nodiscard]] Header headed(std::span<const std::byte> data) {
    if (data.size() != 13) throw Trouble{Kind::malformed, "The header of the PNG is broken."};

    const Header made{big32(data, 0), big32(data, 4), sample(data[8]), sample(data[9]), sample(data[12]) == 1};

    if (made.width == 0 || made.height == 0 || made.width > 0x7FFFFFFFu || made.height > 0x7FFFFFFFu || !allowed(made.color, made.depth) ||
        sample(data[10]) != 0 || sample(data[11]) != 0 || sample(data[12]) > 1)
        throw Trouble{Kind::malformed, "The header of the PNG is broken."};

    return made;
}

}

// Decodes a PNG into one frame of RGBA handed to `each`, reduced as far as it still covers `most`
// (zero: whole; `reduction`). Throws `Trouble`: malformed for no PNG, a broken header or palette,
// a check that does not hold; full for an image over the limits. A file cut short or with broken
// compressed data gives what came before it.
void pnged(std::span<const std::byte> what, const Taking& each, Size most) {
    constexpr std::array<std::uint8_t, 8> signature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

    if (what.size() < 8 || !std::equal(signature.begin(), signature.end(), what.begin(), [](std::uint8_t one, std::byte other) { return one == sample(other); }))
        throw Trouble{Kind::malformed, "Not a PNG."};

    std::optional<Header> header;
    std::optional<Canvas> canvas;
    std::uint32_t scale = 1;
    inflating::Inflater inflater{inflating::Wrapping::zlib, inflating::Rest::left};
    bool palette = false;
    bool data_seen = false;
    bool data_over = false;
    bool wanting = true;
    std::size_t at = 8;

    const auto made = [&] {
        if (!canvas) canvas.emplace(*header, scale);
    };

    const auto begun = [&] {
        if (header->color == 3 && !palette) throw Trouble{Kind::malformed, "The PNG has no palette."};

        made();
        data_seen = true;
    };

    const auto inflated = [&](std::span<const std::byte> data) {
        for (std::size_t from = 0; from < data.size() && wanting; from += fed_piece) {
            Bytes out;

            try {
                inflater.fed(data.subspan(from, std::min(fed_piece, data.size() - from)), out);
            } catch (const Trouble&) {
                wanting = false;
            }

            if (!canvas->fed(out)) wanting = false;
        }
    };

    while (at + 8 <= what.size()) {
        const std::uint32_t length = big32(what, at);
        const std::string_view type{reinterpret_cast<const char*>(what.data() + at + 4), 4};

        if (length > 0x7FFFFFFFu) throw Trouble{Kind::malformed, "A chunk of the PNG is broken."};

        if (!header && type != "IHDR") throw Trouble{Kind::malformed, "The PNG does not start with its header."};

        if (what.size() - at - 8 < std::uint64_t{length} + 4) {
            if (type == "IDAT" && header && !data_over) {
                begun();
                inflated(what.subspan(at + 8, std::min<std::size_t>(length, what.size() - at - 8)));
            }

            break;
        }

        const std::span<const std::byte> data = what.subspan(at + 8, length);

        if (inflating::crc32(0, what.subspan(at + 4, std::size_t{length} + 4)) != big32(what, at + 8 + length))
            throw Trouble{Kind::malformed, "A check of the PNG does not hold."};

        if (type == "IHDR") {
            if (header) throw Trouble{Kind::malformed, "The PNG has two headers."};

            header = headed(data);
            scale = reduction(Size{header->width, header->height}, most);

            if (row_bytes(header->width, *header) > most_row) throw Trouble{Kind::full, "A row of the PNG is over the ceiling."};
        } else if (type == "PLTE" && !data_seen) {
            if (length % 3 != 0 || length == 0 || length > 768 || header->color == 0 || header->color == 4)
                throw Trouble{Kind::malformed, "The palette of the PNG is broken."};

            made();
            canvas->paletted(data);
            palette = true;
        } else if (type == "tRNS" && !data_seen) {
            made();
            canvas->keyed(data);
        } else if (type == "IDAT" && !data_over) {
            begun();
            inflated(data);
        } else if (data_seen) {
            data_over = true;
        }

        if (type == "IEND") break;

        at += 12 + std::size_t{length};
    }

    if (!data_seen) throw Trouble{Kind::malformed, "The PNG has no image data."};

    const Size size = canvas->size();
    const Bytes rgba = canvas->done();

    (void)each(rgba, size, std::chrono::milliseconds{0});
}

namespace {

// A chunk of PNG: the length, the type, the data and the check of type and data.
void chunked(Bytes& into, std::string_view type, std::span<const std::byte> data) {
    for (int shift = 24; shift >= 0; shift -= 8) into.push_back(static_cast<std::byte>((data.size() >> shift) & 0xFF));

    const std::size_t from = into.size();

    for (const char one : type) into.push_back(static_cast<std::byte>(one));

    into.insert(into.end(), data.begin(), data.end());

    const std::uint32_t check = inflating::crc32(0, std::span<const std::byte>{into}.subspan(from));

    for (int shift = 24; shift >= 0; shift -= 8) into.push_back(static_cast<std::byte>((check >> shift) & 0xFF));
}

}

// The pixels of RGBA of `size` (four bytes each, row after row) as a PNG of RGBA of eight bits:
// each row filtered by the filter of the five whose bytes, taken as signed, sum to the least.
Bytes encoded(std::span<const std::byte> rgba, Size size) {
    const std::size_t width = std::size_t{size.width} * bands;
    std::vector<std::uint8_t> above(width, 0);
    std::vector<std::uint8_t> row(width, 0);
    std::vector<std::uint8_t> trial(width, 0);
    std::vector<std::uint8_t> best(width, 0);
    Bytes filtered;

    filtered.reserve((width + 1) * size.height);

    for (std::uint32_t line = 0; line < size.height; ++line) {
        for (std::size_t one = 0; one < width; ++one) row[one] = static_cast<std::uint8_t>(sample(rgba[line * width + one]));

        std::uint64_t least = ~std::uint64_t{0};
        std::uint32_t chosen = 0;

        for (std::uint32_t filter = 0; filter < 5; ++filter) {
            std::uint64_t sum = 0;

            for (std::size_t at = 0; at < width; ++at) {
                const std::uint32_t left = at >= bands ? row[at - bands] : 0;
                const std::uint32_t up = above[at];
                const std::uint32_t corner = at >= bands ? above[at - bands] : 0;
                const std::uint32_t predicted = filter == 0 ? 0 : filter == 1 ? left : filter == 2 ? up : filter == 3 ? (left + up) / 2 : paeth(left, up, corner);
                const auto value = static_cast<std::uint8_t>(row[at] - predicted);

                trial[at] = value;
                sum += value < 128 ? value : 256 - value;
            }

            if (sum < least) {
                least = sum;
                chosen = filter;
                best.swap(trial);
            }
        }

        filtered.push_back(static_cast<std::byte>(chosen));

        for (const std::uint8_t one : best) filtered.push_back(static_cast<std::byte>(one));

        std::swap(above, row);
    }

    Bytes made{std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'}, std::byte{'\r'}, std::byte{'\n'}, std::byte{0x1A}, std::byte{'\n'}};
    Bytes header;

    for (const std::uint32_t one : {size.width, size.height})
        for (int shift = 24; shift >= 0; shift -= 8) header.push_back(static_cast<std::byte>((one >> shift) & 0xFF));

    header.insert(header.end(), {std::byte{8}, std::byte{6}, std::byte{0}, std::byte{0}, std::byte{0}});
    chunked(made, "IHDR", header);
    chunked(made, "IDAT", deflating::deflated(filtered));
    chunked(made, "IEND", {});

    return made;
}

}
