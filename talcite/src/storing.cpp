#include "storing.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "talcite/encoding.hpp"

#include "hexing.hpp"

namespace talcite::fs {

// A path for storage: always valid UTF-8, and always the same path back.
//
// A file name need not be text. On POSIX it is any bytes except zero and the slash: old archives
// and discs burned on Windows 98 carry names in Latin-1 ("Am\xE9lie.mkv"). On Windows it is UTF-16
// units, among which a lone half of a surrogate pair may stand. JSON accepts only valid UTF-8: a
// name outside it would break writing the history and the library index (nlohmann throws), and
// replacing it with a replacement character would lose the file, since reading it back would point
// at a different name than the one stored.
//
// So the stored form has two shapes:
//
// - a name that is valid text stays itself, readable in the file;
// - any other gets U+FFFF in front (a character found in no text: Unicode keeps it out of use),
//   followed by everything that is text as it is and the rest escaped: a byte as %XX, a lone UTF-16
//   unit as %uXXXX, the percent sign itself as %25.
//
// A valid name that starts with U+FFFF is escaped too, or the reader would take it for the escaped
// form. This way no name has two stored forms and no stored form has two names.
//
// A stored form from one system read on the other: a non-ASCII byte means nothing on Windows, so it
// becomes a replacement character; a lone unit on POSIX becomes three bytes of WTF-8. Such a path
// does not point at an existing file anyway; the only point is that reading does not break.
//
// The stored form is computed separately for both kinds of system names (`stored_bytes`,
// `stored_units`, `restored_bytes`, `restored_units`): POSIX bytes and Windows UTF-16 units. Both
// branches are ordinary functions, so the tests check them on every system, not only on the one
// whose names happen to be native.
namespace {

// U+FFFF in UTF-8, the mark of the escaped form.
constexpr std::string_view marker = "\xEF\xBF\xBF";

constexpr char32_t substitute = 0xFFFD;

// A decoded character and the number of bytes it took.
struct Sign {
    char32_t value = 0;
    std::size_t much = 0;
};

// The valid UTF-8 character at `at` and its length, or nothing when the byte does not start a valid
// character by the one rule of talcite (`utf8::span`): a truncated or overlong sequence, a stray
// continuation byte, a surrogate half, or a value past U+10FFFF. What is not valid is stored byte
// by byte, since the stored form must give back exactly the bytes it came from.
std::optional<Sign> sound(std::string_view text, std::size_t at) {
    const utf8::Span one = utf8::span(text.substr(at));

    if (one.kind != utf8::Span::Kind::whole) return std::nullopt;

    std::size_t from = at;

    return Sign{utf8::decode(text, from), one.size};
}

// Appends the character in UTF-8.
void put(char32_t one, std::string& into) {
    if (one < 0x80) {
        into += static_cast<char>(one);
    } else if (one < 0x800) {
        into += static_cast<char>(0xC0 | (one >> 6));
        into += static_cast<char>(0x80 | (one & 0x3F));
    } else if (one < 0x10000) {
        into += static_cast<char>(0xE0 | (one >> 12));
        into += static_cast<char>(0x80 | ((one >> 6) & 0x3F));
        into += static_cast<char>(0x80 | (one & 0x3F));
    } else {
        into += static_cast<char>(0xF0 | (one >> 18));
        into += static_cast<char>(0x80 | ((one >> 12) & 0x3F));
        into += static_cast<char>(0x80 | ((one >> 6) & 0x3F));
        into += static_cast<char>(0x80 | (one & 0x3F));
    }
}

// Appends the character in UTF-16, as a surrogate pair past U+FFFF.
void put(char32_t one, std::u16string& into) {
    if (one < 0x10000) {
        into += static_cast<char16_t>(one);

        return;
    }

    one -= 0x10000;

    into += static_cast<char16_t>(0xD800 + (one >> 10));
    into += static_cast<char16_t>(0xDC00 + (one & 0x3FF));
}

// Appends an escape: %XX for two digits, %uXXXX for four.
void escaped(std::uint32_t value, int digits, std::string& into) {
    into += '%';

    if (digits == 4) into += 'u';

    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) into += hex::upper[(value >> shift) & 0xF];
}

// A text character in the escaped form: the percent sign escaped, everything else as it is.
void kept(char32_t one, std::string& into) {
    if (one == U'%') escaped('%', 2, into);
    else put(one, into);
}

// The plain form when the name is clean text and does not start with the marker; otherwise the
// marker followed by the escaped form.
std::string chosen(bool clean, std::string plain, std::string escapes) {
    if (clean && plain.compare(0, marker.size(), marker) != 0) return plain;

    return std::string{marker} + escapes;
}

// The value of `digits` hexadecimal digits at `at`, or nothing when they are not all there.
std::optional<std::uint32_t> hexed(std::string_view text, std::size_t at, std::size_t digits) {
    if (at + digits > text.size()) return std::nullopt;

    std::uint32_t made = 0;

    for (std::size_t which = 0; which < digits; ++which) {
        const int one = hex::digit(text[at + which]);

        if (one < 0) return std::nullopt;

        made = (made << 4) | static_cast<std::uint32_t>(one);
    }

    return made;
}

// An escape in the escaped form: a byte (%XX) or a UTF-16 unit (%uXXXX), and its length.
struct Escape {
    bool unit = false;
    std::uint32_t value = 0;
    std::size_t much = 0;
};

// The escape at `at`, if there is one. A percent sign not followed by a valid escape is an ordinary
// character, so a hand-edited file does not break reading.
std::optional<Escape> escape_at(std::string_view text, std::size_t at) {
    if (text[at] != '%') return std::nullopt;

    if (at + 1 < text.size() && text[at + 1] == 'u')
        if (const std::optional<std::uint32_t> value = hexed(text, at + 2, 4)) return Escape{true, *value, 6};

    if (const std::optional<std::uint32_t> value = hexed(text, at + 1, 2)) return Escape{false, *value, 3};

    return std::nullopt;
}

}

// The stored form of a POSIX name made of arbitrary bytes.
std::string stored_bytes(std::string_view native) {
    std::string plain;
    std::string escapes;
    bool clean = true;

    for (std::size_t at = 0; at < native.size();) {
        const std::optional<Sign> one = sound(native, at);

        if (!one) {
            clean = false;

            escaped(static_cast<unsigned char>(native[at]), 2, escapes);

            ++at;

            continue;
        }

        put(one->value, plain);
        kept(one->value, escapes);

        at += one->much;
    }

    return chosen(clean, std::move(plain), std::move(escapes));
}

// The stored form of a Windows name made of UTF-16 units, possibly with lone surrogates.
std::string stored_units(std::u16string_view native) {
    std::string plain;
    std::string escapes;
    bool clean = true;

    for (std::size_t at = 0; at < native.size(); ++at) {
        const char32_t one = static_cast<char32_t>(native[at]);

        if (one >= 0xD800 && one <= 0xDBFF && at + 1 < native.size() && native[at + 1] >= 0xDC00 &&
            native[at + 1] <= 0xDFFF) {
            const char32_t whole = 0x10000 + ((one - 0xD800) << 10) + (native[at + 1] - 0xDC00);

            put(whole, plain);
            kept(whole, escapes);

            ++at;

            continue;
        }

        if (one >= 0xD800 && one <= 0xDFFF) {
            clean = false;

            escaped(one, 4, escapes);

            continue;
        }

        put(one, plain);
        kept(one, escapes);
    }

    return chosen(clean, std::move(plain), std::move(escapes));
}

// The POSIX name for a stored form.
std::string restored_bytes(std::string_view said) {
    if (said.compare(0, marker.size(), marker) != 0) return std::string{said};

    said.remove_prefix(marker.size());

    std::string made;

    for (std::size_t at = 0; at < said.size();) {
        if (const std::optional<Escape> one = escape_at(said, at)) {
            // A lone UTF-16 unit from Windows: three bytes of WTF-8, meaningless here but lossless.
            if (one->unit) {
                made += static_cast<char>(0xE0 | (one->value >> 12));
                made += static_cast<char>(0x80 | ((one->value >> 6) & 0x3F));
                made += static_cast<char>(0x80 | (one->value & 0x3F));
            } else {
                made += static_cast<char>(one->value);
            }

            at += one->much;

            continue;
        }

        made += said[at];

        ++at;
    }

    return made;
}

// The Windows name for a stored form.
std::u16string restored_units(std::string_view said) {
    const bool escaping = said.compare(0, marker.size(), marker) == 0;

    if (escaping) said.remove_prefix(marker.size());

    std::u16string made;

    for (std::size_t at = 0; at < said.size();) {
        if (escaping) {
            if (const std::optional<Escape> one = escape_at(said, at)) {
                // A non-ASCII byte from POSIX means nothing here: a replacement character.
                if (one->unit) made += static_cast<char16_t>(one->value);
                else put(one->value < 0x80 ? one->value : substitute, made);

                at += one->much;

                continue;
            }
        }

        if (const std::optional<Sign> one = sound(said, at)) {
            put(one->value, made);

            at += one->much;

            continue;
        }

        // What the rule does not let through is one replacement character for its maximal subpart,
        // as everywhere in talcite (`utf8::decode`).
        put(substitute, made);

        at += std::max<std::size_t>(utf8::span(said.substr(at)).size, 1);
    }

    return made;
}

}
