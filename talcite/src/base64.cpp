#include "talcite/base64.hpp"

#include <array>
#include <cstdint>

namespace talcite {

// Base64 from RFC 4648, the standard alphabet, with padding. It is our own, with no seam: a few
// dozen lines specified down to the bit, with nothing system-dependent, and any library would give
// the same bytes. The first caller is the kitty image, which sends pixels to the terminal as text.
//
// Decoding is strict: the length is divisible by four, padding appears only at the end and is at
// most two characters, and there are no spaces or line breaks. A string that does not meet this is
// not base64; guessing what its author meant is left to whoever received the string.
namespace {

constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr std::uint8_t outside = 0xff;

constexpr std::array<std::uint8_t, 256> values = [] {
    std::array<std::uint8_t, 256> made{};

    made.fill(outside);

    for (std::size_t at = 0; at < alphabet.size(); ++at)
        made[static_cast<unsigned char>(alphabet[at])] = static_cast<std::uint8_t>(at);

    return made;
}();

}

// The bytes encoded as base64 text, padded with `=` to a multiple of four characters.
std::string base64(std::span<const std::byte> given) {
    std::string made;

    made.reserve((given.size() + 2) / 3 * 4);

    std::size_t at = 0;

    for (; at + 3 <= given.size(); at += 3) {
        const auto three = (std::to_integer<std::uint32_t>(given[at]) << 16) |
                           (std::to_integer<std::uint32_t>(given[at + 1]) << 8) |
                           std::to_integer<std::uint32_t>(given[at + 2]);

        made += alphabet[(three >> 18) & 63];
        made += alphabet[(three >> 12) & 63];
        made += alphabet[(three >> 6) & 63];
        made += alphabet[three & 63];
    }

    const std::size_t rest = given.size() - at;

    if (rest == 0) return made;

    std::uint32_t tail = std::to_integer<std::uint32_t>(given[at]) << 16;

    if (rest == 2) tail |= std::to_integer<std::uint32_t>(given[at + 1]) << 8;

    made += alphabet[(tail >> 18) & 63];
    made += alphabet[(tail >> 12) & 63];
    made += rest == 2 ? alphabet[(tail >> 6) & 63] : '=';
    made += '=';

    return made;
}

// The bytes of strict base64 text, or nothing when the text is not base64.
std::optional<Bytes> unbase64(std::string_view given) {
    if (given.size() % 4 != 0) return std::nullopt;

    Bytes made;

    made.reserve(given.size() / 4 * 3);

    for (std::size_t at = 0; at < given.size(); at += 4) {
        const bool last = at + 4 == given.size();

        // Padding: only in the last group of four, at its end, one or two characters.
        std::size_t padded = 0;

        if (last && given[at + 3] == '=') padded = given[at + 2] == '=' ? 2 : 1;

        std::uint32_t four = 0;

        for (std::size_t one = 0; one < 4; ++one) {
            const std::uint8_t value =
                one >= 4 - padded ? std::uint8_t{0} : values[static_cast<unsigned char>(given[at + one])];

            if (value == outside) return std::nullopt;

            four = (four << 6) | value;
        }

        made.push_back(static_cast<std::byte>((four >> 16) & 0xff));

        if (padded < 2) made.push_back(static_cast<std::byte>((four >> 8) & 0xff));

        if (padded < 1) made.push_back(static_cast<std::byte>(four & 0xff));
    }

    return made;
}

}
