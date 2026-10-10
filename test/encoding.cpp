#include "proving.hpp"

#include <cstddef>
#include <string>

#include "talcite/bytes.hpp"
#include "talcite/encoding.hpp"

using namespace talcite;

TRIAL("Encoding: the lead byte tells how many continuation bytes follow") {
    same(utf8::trailing(0x41), std::size_t{0});
    same(utf8::trailing(0xC5), std::size_t{1});
    same(utf8::trailing(0xE2), std::size_t{2});
    same(utf8::trailing(0xF0), std::size_t{3});
    same(utf8::trailing(0x80), std::size_t{0});
}

TRIAL("Encoding: character by character through the whole string") {
    const std::string said = "aż💡";

    std::size_t at = 0;

    same(utf8::decode(said, at), U'a');
    same(utf8::decode(said, at), U'ż');
    same(utf8::decode(said, at), U'💡');
    same(at, said.size());
}

TRIAL("Encoding: an invalid byte gives a replacement character and one step") {
    std::string said = "a";
    said += static_cast<char>(0xFF);
    said += "b";

    std::size_t at = 1;

    same(utf8::decode(said, at), U'�');
    same(at, std::size_t{2});
}

TRIAL("Encoding: a sequence cut off halfway also gives a replacement character") {
    std::string said;
    said += static_cast<char>(0xE2);
    said += static_cast<char>(0x9D);

    std::size_t at = 0;

    same(utf8::decode(said, at), U'�');
    same(at, std::size_t{1});
}

TRIAL("Encoding: a lead byte without its continuation is garbage too") {
    std::string said;
    said += static_cast<char>(0xC5);
    said += static_cast<char>(0x41);

    std::size_t at = 0;

    same(utf8::decode(said, at), U'�');
    same(at, std::size_t{1});
}

TRIAL("Encoding: the end of the string returns zero and does not move the position") {
    const std::string said = "a";

    std::size_t at = 1;

    same(utf8::decode(said, at), static_cast<char32_t>(0));
    same(at, std::size_t{1});
}

TRIAL("Encoding: encoding and decoding round-trips") {
    for (const char32_t one : {U'a', U'ż', U'漢', U'💡'}) {
        const std::string said = utf8::encode(one);

        std::size_t at = 0;

        same(utf8::decode(said, at), one);
        same(at, said.size());
    }
}

TRIAL("Encoding: encoding appends rather than replaces") {
    std::string said = "a";

    utf8::encode(U'ż', said);

    same(said, std::string{"aż"});
}

TRIAL("Encoding: the encoded length follows the rule") {
    same(utf8::encode(U'a').size(), std::size_t{1});
    same(utf8::encode(U'ż').size(), std::size_t{2});
    same(utf8::encode(U'漢').size(), std::size_t{3});
    same(utf8::encode(U'💡').size(), std::size_t{4});
}

TRIAL("Encoding: only characters that really take two cells are wide") {
    same(width(U'a'), 1);
    same(width(U'ż'), 1);
    same(width(U'漢'), 2);
    same(width(U'가'), 2);
    same(width(U'😀'), 2);
    same(width(U'→'), 1);
}

// UTF-8 and the native wide string. This pair exists so that we can talk to Windows with one
// encoding: Windows speaks UTF-16, the rest of the library UTF-8.
TRIAL("Encoding: a wide string and UTF-8 convert there and back") {
    const std::string said = "Zażółć gęślą jaźń";

    same(talcite::to_string(talcite::to_wstring(said)), said);

    // Plain ASCII too, because it is the most common case and the easiest to break with a shortcut.
    same(talcite::to_string(talcite::to_wstring(std::string{"plain"})), std::string{"plain"});

    same(talcite::to_string(std::wstring_view{}), std::string{});
    ought(talcite::to_wstring(std::string_view{}).empty());
}

// A character outside the Basic Multilingual Plane is the only place where the two paths differ: on
// Windows `wchar_t` has two bytes and such a character needs a surrogate pair, on POSIX it has four
// and there is no pair at all. The trial asks about the result, so it tells the truth on both, but
// nobody sees the pair being assembled here, because the suite runs on POSIX. This is a known gap,
// not an oversight.
TRIAL("Encoding: a character outside the Basic Multilingual Plane converts there and back") {
    const std::string said = "a\U0001F600b";

    const std::wstring wide = talcite::to_wstring(said);

    same(talcite::to_string(wide), said);

    // On Windows these are four units (a, the pair, b), elsewhere three characters.
    same(wide.size(), sizeof(wchar_t) == 2 ? std::size_t{4} : std::size_t{3});
}

// A byte that is not valid UTF-8 ends up as a replacement character, just as in `utf8::decode`.
// This is exactly the symptom of a file name that arrives in the ANSI code page instead of UTF-8:
// replacement characters in place of Polish letters.
TRIAL("Encoding: a byte outside UTF-8 becomes a replacement character, not silent garbage") {
    const std::string broken = "Za\xbf\xf3\xb3\xe6.txt";

    const std::string made = talcite::to_string(talcite::to_wstring(broken));

    ought(made != broken);
    ought(made.find("\xef\xbf\xbd") != std::string::npos);
}

// A zero in the middle of a string. `std::string_view` knows its length, so a zero in it is a
// character, not the end, and it must come out the same on the other side. The trial guards that
// the conversion does not stop at the first zero and silently lose the rest.
TRIAL("Encoding: a zero in the middle of a string is a character, not the end") {
    const std::string said{"a\0b", 3};

    const std::wstring wide = talcite::to_wstring(said);

    same(wide.size(), std::size_t{3});
    same(wide[1], wchar_t{0});

    same(talcite::to_string(wide), said);
}

// String to bytes and back, byte for byte: no encoding, with a zero byte and non-ASCII characters,
// in both directions.
TRIAL("Bytes: bytes and text copy byte for byte, in both directions") {
    const std::string said{"za\0\xc5\xbc\xc3\xb3\xc5\x82\xff", 10};
    const Bytes made = bytes(said);

    same(made.size(), std::size_t{10});
    ought(made[2] == std::byte{0});
    ought(made[9] == std::byte{0xff});
    same(text(made), said);
    ought(bytes("").empty());
    same(text(Bytes{}), std::string{});
}
