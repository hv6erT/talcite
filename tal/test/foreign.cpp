#include "foreign.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include "tal/trouble.hpp"

// A format no decoder of tal knows, for the trials of the port `Decoding`: "TALX", the width and
// the height in two bytes each, little-endian, and one tone; the image is that tone, opaque, all
// over. Its adapter is what a user's adapter of a format tal does not know would be.
namespace tal::testing {

namespace {

constexpr std::string_view mark = "TALX";

// The number of two bytes at `at` of `head`, little-endian.
std::uint32_t two(std::span<const std::byte> head, std::size_t at) {
    return std::to_integer<std::uint32_t>(head[at]) | (std::to_integer<std::uint32_t>(head[at + 1]) << 8);
}

// The adapter of the format.
class Foreign final : public media::Decoding {
public:
    // The adapter's name.
    [[nodiscard]] std::string_view name() const override { return "foreign"; }

    // Whether `head` starts with the mark of the format.
    [[nodiscard]] bool takes(std::span<const std::byte> head) const override {
        if (head.size() < mark.size()) return false;

        for (std::size_t at = 0; at < mark.size(); ++at)
            if (std::to_integer<char>(head[at]) != mark[at]) return false;

        return true;
    }

    // The size the header gives, when it is all there.
    [[nodiscard]] std::optional<media::Size> sized(std::span<const std::byte> head) const override {
        if (!takes(head) || head.size() < 8) return std::nullopt;

        return media::Size{two(head, 4), two(head, 6)};
    }

    // The image as one frame of its tone for `each`. Throws `Trouble` malformed for a file cut
    // short.
    void decoded(std::span<const std::byte> given, const media::Taking& each) override {
        const std::optional<media::Size> size = sized(given);

        if (!size || given.size() < 9) throw Trouble{Kind::malformed, "A foreign image cut short."};

        const std::byte tone = given[8];
        Bytes rgba;

        for (std::uint64_t at = 0; at < std::uint64_t{size->width} * size->height; ++at) rgba.insert(rgba.end(), {tone, tone, tone, std::byte{255}});

        (void)each(rgba, *size, std::chrono::milliseconds{0});
    }
};

}

// The adapter of the foreign format.
std::shared_ptr<media::Decoding> foreign() { return std::make_shared<Foreign>(); }

// A foreign image of `size` in the tone `tone`.
Bytes foreign_image(media::Size size, std::uint8_t tone) {
    Bytes made;

    for (const char one : mark) made.push_back(static_cast<std::byte>(one));

    for (const std::uint32_t side : {size.width, size.height}) {
        made.push_back(static_cast<std::byte>(side & 0xFF));
        made.push_back(static_cast<std::byte>((side >> 8) & 0xFF));
    }

    made.push_back(std::byte{tone});

    return made;
}

}
