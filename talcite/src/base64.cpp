#include "talcite/base64.hpp"

#include <array>
#include <cstdint>

namespace talcite {

// **Base64 z RFC 4648, alfabet zwykły, z dopełnieniem.** Własny, bez szwu: to kilkadziesiąt
// wierszy opisanych do bitu, bez niczego zależnego od systemu, a każda biblioteka dałaby te same
// bajty. Pierwszym wołającym jest obraz kitty (G2), który wysyła piksele terminalowi tekstem.
//
// Odczyt jest ścisły: długość podzielna przez cztery, dopełnienie tylko na końcu i najwyżej dwa
// znaki, żadnych odstępów ani końców wierszy. Napis, który tego nie spełnia, nie jest base64 —
// zgadywanie, co autor miał na myśli, zostawiamy temu, kto napis dostał.
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

std::optional<Bytes> unbase64(std::string_view given) {
    if (given.size() % 4 != 0) return std::nullopt;

    Bytes made;

    made.reserve(given.size() / 4 * 3);

    for (std::size_t at = 0; at < given.size(); at += 4) {
        const bool last = at + 4 == given.size();

        // Dopełnienie: tylko w ostatniej czwórce, na jej końcu, jeden albo dwa znaki.
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
