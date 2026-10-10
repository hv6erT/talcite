#include "talcite/encoding.hpp"

#include <cstdint>

namespace talcite {

namespace utf8 {

// How many continuation bytes this lead byte announces. Zero means "either a plain ASCII
// character or a byte that starts nothing"; the caller settles that itself, because the input
// decoder rejects garbage differently from writing to the screen.
std::size_t trailing(unsigned char lead) {
    if ((lead & 0xE0) == 0xC0) return 1;
    if ((lead & 0xF0) == 0xE0) return 2;
    if ((lead & 0xF8) == 0xF0) return 3;

    return 0;
}

// The next character of the string. A byte that breaks the rules and a tail cut off halfway come
// back as the replacement character, and decoding moves on by one byte: a string with a broken
// middle has to be displayed, not vanish. Where a cut-off tail has to wait for the rest instead
// of vanishing, that is in the input decoder, this function is not used.
char32_t decode(std::string_view from, std::size_t& at) {
    if (at >= from.size()) return 0;

    const unsigned char one = static_cast<unsigned char>(from[at]);

    if (one < 0x80) {
        ++at;

        return one;
    }

    const std::size_t much = utf8::trailing(one);

    if (much == 0 || at + much >= from.size()) {
        ++at;

        return 0xFFFD;
    }

    char32_t made = one & (0x3F >> much);

    for (std::size_t which = 1; which <= much; ++which) {
        const unsigned char next = static_cast<unsigned char>(from[at + which]);

        if ((next & 0xC0) != 0x80) {
            ++at;

            return 0xFFFD;
        }

        made = (made << 6) | (next & 0x3F);
    }

    at += much + 1;

    return made;
}

// Appends the UTF-8 form of the character to `into`.
void encode(char32_t one, std::string& into) {
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

// The UTF-8 form of a single character.
std::string encode(char32_t one) {
    std::string made;

    utf8::encode(one, made);

    return made;
}

}

// A character to show, never to execute. Text that reaches the screen is often foreign (a file
// name, a title from the network, a description from an NFO), and a terminal executes control
// characters: ESC starts a sequence that can rearrange the screen, change the window title and,
// in some terminals, write something to the clipboard or answer on the input as if a person had
// typed it. A file named with an escape sequence would thus be a command to the terminal. So every
// cell gets its character passed through here:
//
// - C0 and DEL become their pictures from the Control Pictures block (␛, ␊, ␡), so it is visible
//   that something is there;
// - C1 (U+0080–U+009F, among them U+009B, the single-character CSI) becomes the replacement
//   character;
// - writing-direction characters (U+200E, U+200F, U+061C, U+202A–U+202E, U+2066–U+2069) and the
//   line and paragraph separators become the replacement character. Reversing the direction turns
//   the name "film", U+202E, "4pm.exe" into "filmexe.mp4" on the screen: an executable poses as a
//   film.
//
// The width stays the same (one cell), so nothing that counts columns gets out of line.
char32_t shown(char32_t one) {
    if (one < 0x20) return 0x2400 + one;

    if (one == 0x7F) return 0x2421;

    if ((one >= 0x80 && one <= 0x9F) || one == 0x061C || one == 0x200E || one == 0x200F ||
        (one >= 0x2028 && one <= 0x202E) || (one >= 0x2066 && one <= 0x2069))
        return 0xFFFD;

    return one;
}

// The width of a character in cells. There is no full Unicode table here, and there need not be:
// what matters are the ranges that really take two cells, East Asian scripts and emoji. Combining
// characters are treated as ordinary ones, because merging them with the previous cell is a
// separate matter that is not needed yet, and pretending they have zero width would throw the
// line out of alignment.
int width(char32_t one) {
    if (one < 0x1100) return 1;

    if ((one >= 0x1100 && one <= 0x115F) || (one >= 0x2E80 && one <= 0x303E) ||
        (one >= 0x3041 && one <= 0x33FF) || (one >= 0x3400 && one <= 0x4DBF) ||
        (one >= 0x4E00 && one <= 0x9FFF) || (one >= 0xA000 && one <= 0xA4CF) ||
        (one >= 0xAC00 && one <= 0xD7A3) || (one >= 0xF900 && one <= 0xFAFF) ||
        (one >= 0xFE10 && one <= 0xFE19) || (one >= 0xFE30 && one <= 0xFE6F) ||
        (one >= 0xFF00 && one <= 0xFF60) || (one >= 0xFFE0 && one <= 0xFFE6) ||
        (one >= 0x1F300 && one <= 0x1F64F) || (one >= 0x1F900 && one <= 0x1F9FF) ||
        (one >= 0x20000 && one <= 0x3FFFD))
        return 2;

    return 1;
}

// Whether a character belongs to a word. This is an approximation and is meant to stay one:
// everything outside ASCII counts as a letter, because a real answer would need Unicode tables,
// and talcite has none. As a result "ą" is a letter, but so are a Chinese bracket and a Cyrillic
// punctuation mark, and that is a price paid on purpose.
//
// The rule is also written so that a widened byte of a UTF-8 sequence gives the same answer as
// the character it comes from: both fall under "not ASCII, so a letter". That makes it usable
// both where text is walked by characters and where it is walked by bytes.
//
// It lives here and not in `Comb`, because whole-word search, double-click word selection and a
// future jump by words must all mean the same thing.
bool in_word(char32_t one) {
    if (one >= 0x80) return true;

    const char sign = static_cast<char>(one);

    return (sign >= '0' && sign <= '9') || (sign >= 'a' && sign <= 'z') ||
           (sign >= 'A' && sign <= 'Z') || sign == '_';
}


// The system's native wide string turned into UTF-8 (`to_wstring` goes the other way). The pair
// is needed wherever Windows talks to the library: Windows speaks UTF-16, and all the rest of this
// code speaks UTF-8.
//
// It is written with our own decoder, not through Win32, and that is a decision, not a
// convenience: `MultiByteToWideChar` exists on one system only, so code written with it cannot
// be run in trials anywhere else. Here the same path runs on both; the only difference is whether
// `wchar_t` has two bytes (Windows, UTF-16 with surrogate pairs) or four (POSIX, UTF-32).
//
// Known gap: the 16-bit branch runs only on Windows, so trials running on POSIX do not touch it.
// The surrogate pair is checked by computing in the other direction (a character outside the
// Basic Multilingual Plane goes there and back), but the pair assembly itself is seen by no one
// here.
std::string to_string(std::wstring_view from) {
    std::string made;

    made.reserve(from.size());

    for (std::size_t at = 0; at < from.size(); ++at) {
        char32_t one = static_cast<char32_t>(static_cast<std::uint32_t>(from[at]));

        if constexpr (sizeof(wchar_t) == 2) {
            // A high surrogate without a low one is a damaged string: it becomes the replacement
            // character, as in `utf8::decode` for broken UTF-8, instead of a guess.
            if (one >= 0xD800 && one <= 0xDBFF) {
                const char32_t high = one;

                if (at + 1 < from.size()) {
                    const char32_t low =
                        static_cast<char32_t>(static_cast<std::uint32_t>(from[at + 1]));

                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        one = 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);

                        ++at;
                    } else {
                        one = 0xFFFD;
                    }
                } else {
                    one = 0xFFFD;
                }
            } else if (one >= 0xDC00 && one <= 0xDFFF) {
                one = 0xFFFD;
            }
        }

        utf8::encode(one, made);
    }

    return made;
}

// The system's native wide string from UTF-8: UTF-16 with surrogate pairs where `wchar_t` has two
// bytes, UTF-32 where it has four. Broken UTF-8 becomes the replacement character.
std::wstring to_wstring(std::string_view from) {
    std::wstring made;

    made.reserve(from.size());

    std::size_t at = 0;

    while (at < from.size()) {
        // `utf8::decode` returns zero only when `at` has gone past the string (and the loop
        // condition already guards that) or when the byte is a real zero. A zero in the middle of
        // the string is therefore copied like any other character: `string_view` knows its length
        // and does not end at NUL, and cutting the string there would silently lose the rest.
        const char32_t one = utf8::decode(from, at);

        if constexpr (sizeof(wchar_t) == 2) {
            if (one >= 0x10000) {
                const char32_t rest = one - 0x10000;

                made.push_back(static_cast<wchar_t>(0xD800 + (rest >> 10)));
                made.push_back(static_cast<wchar_t>(0xDC00 + (rest & 0x3FF)));

                continue;
            }
        }

        made.push_back(static_cast<wchar_t>(one));
    }

    return made;
}

}
