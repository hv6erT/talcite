#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Hexadecimal digits and percent escapes, each written once for every file of talcite.

namespace talcite::hex {

// The digits in capitals, which addresses and escapes take, as RFC 3986 recommends.
inline constexpr std::string_view upper = "0123456789ABCDEF";

// The digits in small letters, which names made up for files and pipes take.
inline constexpr std::string_view lower = "0123456789abcdef";

// The value of a hexadecimal digit of either case, or -1 for any other character.
[[nodiscard]] constexpr int digit(char one) {
    if (one >= '0' && one <= '9') return one - '0';
    if (one >= 'a' && one <= 'f') return one - 'a' + 10;
    if (one >= 'A' && one <= 'F') return one - 'A' + 10;

    return -1;
}

// Appends a byte as a percent escape, %XX in capitals.
inline void escaped(unsigned char one, std::string& into) {
    into += '%';
    into += upper[one >> 4];
    into += upper[one & 0x0F];
}

// The percent escapes of a piece of an address undone: %20 is a space. A percent sign not followed
// by two hexadecimal digits stays as it is, so a hand-written address is never refused.
[[nodiscard]] inline std::string unescaped(std::string_view said) {
    std::string made;

    for (std::size_t at = 0; at < said.size(); ++at) {
        if (said[at] == '%' && at + 2 < said.size()) {
            const int high = digit(said[at + 1]);
            const int low = digit(said[at + 2]);

            if (high >= 0 && low >= 0) {
                made += static_cast<char>(high * 16 + low);
                at += 2;

                continue;
            }
        }

        made += said[at];
    }

    return made;
}

}
