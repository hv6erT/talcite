#include "charsets.hpp"

#include <array>
#include <cstdint>
#include <optional>

#include "tal/text.hpp"

// The encodings the text versions read, into UTF-8: UTF-8 itself, UTF-16 and three single-byte
// encodings, mapped as the WHATWG Encoding standard maps them. Nothing here fails: whatever does
// not decode becomes U+FFFD, as a browser shows it.
namespace tal::docs::charsets {

namespace {

#include "charsets.inc"

// The text of `given` as a view of characters.
[[nodiscard]] std::string_view viewed(std::span<const std::byte> given) {
    return {reinterpret_cast<const char*>(given.data()), given.size()};
}

}

// Whether `given` is UTF-8 throughout: every sequence well-formed and none cut off at the end.
bool whole_utf8(std::span<const std::byte> given) {
    const std::string_view text = viewed(given);
    std::size_t at = 0;

    while (at < text.size())
        if (!text::decoded(text, at)) return false;

    return true;
}

// `given` read as UTF-8: each sequence that is not well-formed, at its maximal subpart, becomes
// U+FFFD.
std::string from_utf8(std::span<const std::byte> given) {
    const std::string_view text = viewed(given);
    std::string made;
    std::size_t at = 0;

    made.reserve(text.size());

    while (at < text.size()) text::encoded(made, text::decoded(text, at).value_or(char32_t{0xFFFD}));

    return made;
}

// `given` read as UTF-16 in the order `big` says: a surrogate pair is one character, a lone
// surrogate and an odd last byte are U+FFFD.
std::string from_utf16(std::span<const std::byte> given, bool big) {
    const auto unit = [&](std::size_t at) {
        const auto first = std::to_integer<std::uint32_t>(given[at]);
        const auto second = std::to_integer<std::uint32_t>(given[at + 1]);

        return big ? (first << 8U) | second : (second << 8U) | first;
    };

    std::string made;

    made.reserve(given.size());

    for (std::size_t at = 0; at + 1 < given.size(); at += 2) {
        const std::uint32_t one = unit(at);

        if (one >= 0xD800 && one <= 0xDBFF && at + 3 < given.size()) {
            const std::uint32_t low = unit(at + 2);

            if (low >= 0xDC00 && low <= 0xDFFF) {
                text::encoded(made, static_cast<char32_t>(0x10000 + ((one - 0xD800) << 10U) + (low - 0xDC00)));
                at += 2;

                continue;
            }
        }

        text::encoded(made, one >= 0xD800 && one <= 0xDFFF ? char32_t{0xFFFD} : static_cast<char32_t>(one));
    }

    if (given.size() % 2 != 0) text::encoded(made, 0xFFFD);

    return made;
}

// The character the byte `one` stands for in the single-byte `encoding`.
char32_t single(std::byte one, Encoding encoding) {
    const auto code = std::to_integer<std::uint32_t>(one);

    if (code < 0x80) return code;

    switch (encoding) {
        case Encoding::windows_1250: return windows_1250[code - 0x80];
        case Encoding::iso_8859_2: return iso_8859_2[code - 0x80];
        case Encoding::windows_1252: break;
    }

    return windows_1252[code - 0x80];
}

// `given` read in the single-byte `encoding`.
std::string from_single(std::span<const std::byte> given, Encoding encoding) {
    std::string made;

    made.reserve(given.size());

    for (const std::byte one : given) text::encoded(made, single(one, encoding));

    return made;
}

}
