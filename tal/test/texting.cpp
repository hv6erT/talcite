#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <ctime>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "tal/text.hpp"

#include "tal/testing.hpp"

// Text of tal::core: UTF-8 by the one rule of Table 3-7, wide strings for Windows, the cells and
// clusters a terminal draws, wrapping, letter case, the order of languages and the rules by which
// languages write numbers, sizes, dates and plurals.

namespace {

namespace text = tal::text;

using text::Case;
using text::Collation;
using text::Language;
using text::Rendering;

// The next character, a replacement character for a broken sequence.
char32_t point(std::string_view given, std::size_t& at) { return text::decoded(given, at).value_or(U'\ufffd'); }

// The UTF-8 form of one character.
std::string spelt(char32_t one) {
    std::string made;

    text::encoded(made, one);

    return made;
}

// The text cut into clusters, separated by "|".
std::string parted(std::string_view given, Rendering how = {}) {
    std::string made;

    for (const std::string_view one : text::clusters(given, how)) {
        if (!made.empty()) made += '|';

        made += one;
    }

    return made;
}

// The starts of the clusters walking back from the end, as `text::before` finds them.
std::vector<std::size_t> backward(std::string_view given, Rendering how) {
    std::vector<std::size_t> made;

    for (std::size_t at = given.size(); at > 0;) {
        at = text::before(given, at, how);
        made.insert(made.begin(), at);
    }

    return made;
}

// The starts of the clusters walking forward.
std::vector<std::size_t> forward(std::string_view given, Rendering how) {
    std::vector<std::size_t> made;

    for (std::size_t at = 0; at < given.size(); at = text::after(given, at, how)) made.push_back(at);

    return made;
}

// The words sorted by the collation, through their keys, as a sort uses it.
std::vector<std::string> ordered(const Collation& by, std::vector<std::string> words) {
    std::sort(words.begin(), words.end(), [&by](const std::string& one, const std::string& two) { return by.key(one) < by.key(two); });

    return words;
}

// A moment of local time from its calendar fields.
std::chrono::system_clock::time_point at_local(int year, int month, int day, int hour, int minute) {
    std::tm fields{};

    fields.tm_year = year - 1900;
    fields.tm_mon = month - 1;
    fields.tm_mday = day;
    fields.tm_hour = hour;
    fields.tm_min = minute;
    fields.tm_isdst = -1;

    return std::chrono::system_clock::from_time_t(std::mktime(&fields));
}

// The lines of a wrapped text, as strings.
std::vector<std::string> lines(std::string_view given, int columns) {
    std::vector<std::string> made;

    for (const std::string_view one : text::wrapped(given, columns)) made.emplace_back(one);

    return made;
}

const std::string acute = "\xcc\x81";
const std::string zwj = "\xe2\x80\x8d";
const std::string woman = "\xf0\x9f\x91\xa9";
const std::string laptop = "\xf0\x9f\x92\xbb";
const std::string thumb = "\xf0\x9f\x91\x8d";
const std::string tone = "\xf0\x9f\x8f\xbd";
const std::string heart = "\xe2\x9d\xa4";
const std::string selector = "\xef\xb8\x8f";
const std::string poland = "\xf0\x9f\x87\xb5\xf0\x9f\x87\xb1";
const std::string space = "\xc2\xa0";

constexpr Rendering joined{true, true};
constexpr Rendering apart{false, false};

}

TRIAL("Text: character by character through the whole string") {
    const std::string said = "aż💡";

    std::size_t at = 0;

    SAME(point(said, at), U'a');
    SAME(point(said, at), U'ż');
    SAME(point(said, at), U'💡');
    SAME(at, said.size());
}

TRIAL("Text: an invalid byte gives a replacement character and one step") {
    std::string said = "a";
    said += static_cast<char>(0xFF);
    said += "b";

    std::size_t at = 1;

    SAME(point(said, at), U'�');
    SAME(at, std::size_t{2});
}

// A tail cut off halfway is one replacement character for the whole of it, its maximal subpart.
TRIAL("Text: a sequence cut off halfway also gives one replacement character") {
    std::string said;
    said += static_cast<char>(0xE2);
    said += static_cast<char>(0x9D);

    std::size_t at = 0;

    SAME(point(said, at), U'�');
    SAME(at, std::size_t{2});
}

// The one rule of talcite for UTF-8 (Unicode, table 3-7): the second byte bounds the rest, so an
// overlong form, a surrogate or a value past U+10FFFF is bad as soon as its second byte comes, and
// a bad sequence takes its maximal subpart, the bytes that still could have started a character.
TRIAL("Text: span tells a whole, a partial and a bad sequence") {
    using Kind = text::Span::Kind;

    const auto is = [](std::string_view text, Kind kind, std::size_t size) {
        const text::Span got = text::span(text);

        return got.kind == kind && got.size == size;
    };

    OUGHT(is("a", Kind::whole, 1));
    OUGHT(is("\xc5\xbc!", Kind::whole, 2));
    OUGHT(is("\xe2\x9d\xa4", Kind::whole, 3));
    OUGHT(is("\xf0\x9f\x99\x82", Kind::whole, 4));

    OUGHT(is("", Kind::partial, 0));
    OUGHT(is("\xe2", Kind::partial, 1));
    OUGHT(is("\xe2\x9d", Kind::partial, 2));
    OUGHT(is("\xf4\x8f\xbf", Kind::partial, 3));

    OUGHT(is("\x80", Kind::bad, 1));
    OUGHT(is("\xc1\xbf", Kind::bad, 1));
    OUGHT(is("\xf5\x80", Kind::bad, 1));
    OUGHT(is("\xe0\x9f", Kind::bad, 1));
    OUGHT(is("\xed\xa0", Kind::bad, 1));
    OUGHT(is("\xf0\x8f", Kind::bad, 1));
    OUGHT(is("\xf4\x90", Kind::bad, 1));
    OUGHT(is("\xe2\x9d" "A", Kind::bad, 2));
    OUGHT(is("\xf0\x9f\x99" "A", Kind::bad, 3));
}

TRIAL("Text: strict says whether the whole text is well-formed") {
    OUGHT(text::valid(""));
    OUGHT(text::valid("za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87"));
    OUGHT(!text::valid("a\xc0\xaf"));
    OUGHT(!text::valid("a\xed\xa0\x80"));
    OUGHT(!text::valid("a\xe2\x9d"));
}

// A broken sequence is one replacement character for its maximal subpart, and the byte that broke
// it is read on its own.
TRIAL("Text: a sequence broken off by another byte is one replacement character") {
    const std::string said = "\xe2\x9d" "A";

    std::size_t at = 0;

    SAME(point(said, at), U'\ufffd');
    SAME(at, std::size_t{2});
    SAME(point(said, at), U'A');
}

TRIAL("Text: a lead byte without its continuation is garbage too") {
    std::string said;
    said += static_cast<char>(0xC5);
    said += static_cast<char>(0x41);

    std::size_t at = 0;

    SAME(point(said, at), U'�');
    SAME(at, std::size_t{1});
}

TRIAL("Text: the end of the string gives nothing and does not move the position") {
    const std::string said = "a";

    std::size_t at = 1;

    OUGHT(!text::decoded(said, at).has_value());
    SAME(at, std::size_t{1});
}

TRIAL("Text: encoding and decoding round-trips") {
    for (const char32_t one : {U'a', U'ż', U'漢', U'💡'}) {
        const std::string said = spelt(one);

        std::size_t at = 0;

        SAME(point(said, at), one);
        SAME(at, said.size());
    }
}

TRIAL("Text: encoding appends rather than replaces") {
    std::string said = "a";

    text::encoded(said, U'ż');

    SAME(said, std::string{"aż"});
}

TRIAL("Text: the encoded length follows the rule") {
    SAME(spelt(U'a').size(), std::size_t{1});
    SAME(spelt(U'ż').size(), std::size_t{2});
    SAME(spelt(U'漢').size(), std::size_t{3});
    SAME(spelt(U'💡').size(), std::size_t{4});
}

TRIAL("Text: only characters that really take two cells are wide") {
    SAME(text::width(U'a'), 1);
    SAME(text::width(U'ż'), 1);
    SAME(text::width(U'漢'), 2);
    SAME(text::width(U'가'), 2);
    SAME(text::width(U'😀'), 2);
    SAME(text::width(U'→'), 1);
}

// UTF-8 and the native wide string. This pair exists so that we can talk to Windows with one
// encoding: Windows speaks UTF-16, the rest of the library UTF-8.
TRIAL("Text: a wide string and UTF-8 convert there and back") {
    const std::string said = "Zażółć gęślą jaźń";

    SAME(text::narrow(text::wide(said)), said);

    // Plain ASCII too, because it is the most common case and the easiest to break with a shortcut.
    SAME(text::narrow(text::wide(std::string{"plain"})), std::string{"plain"});

    SAME(text::narrow(std::wstring_view{}), std::string{});
    OUGHT(text::wide(std::string_view{}).empty());
}

// A character outside the Basic Multilingual Plane is the only place where the two paths differ: on
// Windows `wchar_t` has two bytes and such a character needs a surrogate pair, on POSIX it has four
// and there is no pair at all. The trial asks about the result, so it tells the truth on both, but
// nobody sees the pair being assembled here, because the suite runs on POSIX. This is a known gap,
// not an oversight.
TRIAL("Text: a character outside the Basic Multilingual Plane converts there and back") {
    const std::string said = "a\U0001F600b";

    const std::wstring wide = text::wide(said);

    SAME(text::narrow(wide), said);

    // On Windows these are four units (a, the pair, b), elsewhere three characters.
    SAME(wide.size(), sizeof(wchar_t) == 2 ? std::size_t{4} : std::size_t{3});
}

// A byte that is not valid UTF-8 ends up as a replacement character, just as in `text::decoded`.
// This is exactly the symptom of a file name that arrives in the ANSI code page instead of UTF-8:
// replacement characters in place of Polish letters.
TRIAL("Text: a byte outside UTF-8 becomes a replacement character, not silent garbage") {
    const std::string broken = "Za\xbf\xf3\xb3\xe6.txt";

    const std::string made = text::narrow(text::wide(broken));

    OUGHT(made != broken);
    OUGHT(made.find("\xef\xbf\xbd") != std::string::npos);
}

// Forms UTF-8 forbids although their bits fit come back as the replacement character, one byte at
// a time, as any other broken byte: past U+10FFFF (it would pass for the handle of a grapheme
// cluster on the screen), half of a surrogate pair, and a character written longer than it needs
// (a hidden "/" and a hidden NUL). The largest character, the last before the surrogates and the
// shortest forms of each length still decode.
TRIAL("Text: overlong forms, surrogates and values past U+10FFFF are broken bytes") {
    const auto decoded = [](std::string_view given) {
        std::u32string made;

        for (std::size_t at = 0; at < given.size();) made += point(given, at);

        return made;
    };

    SAME(decoded("\xf4\x90\x80\x80"), std::u32string(4, U'\ufffd'));
    SAME(decoded("\xed\xa0\x80"), std::u32string(3, U'\ufffd'));
    SAME(decoded("\xe0\x80\xaf"), std::u32string(3, U'\ufffd'));
    SAME(decoded("\xc0\xaf"), std::u32string(2, U'\ufffd'));
    SAME(decoded("\xc0\x80"), std::u32string(2, U'\ufffd'));
    SAME(decoded("\xf0\x80\x80\xaf"), std::u32string(4, U'\ufffd'));

    SAME(decoded("\xf4\x8f\xbf\xbf"), std::u32string{U'\U0010ffff'});
    SAME(decoded("\xed\x9f\xbf"), std::u32string{U'\ud7ff'});
    SAME(decoded("\xc2\x80\xe0\xa0\x80\xf0\x90\x80\x80"), (std::u32string{U'\u0080', U'\u0800', U'\U00010000'}));
    SAME(decoded("a\xc5\xbc"), (std::u32string{U'a', U'\u017c'}));
}

// A zero in the middle of a string. `std::string_view` knows its length, so a zero in it is a
// character, not the end, and it must come out the same on the other side. The trial guards that
// the conversion does not stop at the first zero and silently lose the rest.
TRIAL("Text: a zero in the middle of a string is a character, not the end") {
    const std::string said{"a\0b", 3};

    const std::wstring wide = text::wide(said);

    SAME(wide.size(), std::size_t{3});
    SAME(wide[1], wchar_t{0});

    SAME(text::narrow(wide), said);
}

TRIAL("Text: the width of a single character") {
    SAME(text::width(U'a'), 1);
    SAME(text::width(U'\x142'), 1);
    SAME(text::width(U'\x4F60'), 2);
    SAME(text::width(U'\x301'), 0);
    SAME(text::width(U'\x200B'), 0);
    SAME(text::width(U'\x200D'), 0);
    SAME(text::width(U'\xFE0F'), 0);
    SAME(text::width(U'\x1161'), 0);
    SAME(text::width(U'\x1100'), 2);
    SAME(text::width(U'\x01'), 1);
    SAME(text::width(U'\x202E'), 1);
    SAME(text::width(U'\U0001F469'), 2);
    SAME(text::width(U'\x231A'), 2);
    SAME(text::width(U'\U0001F3FD', joined), 0);
    SAME(text::width(U'\U0001F3FD', apart), 2);
}

// A letter keeps its accents, whatever came before them, and a mark after a control has nothing to
// sit on.
TRIAL("Text: letters with their marks, as names from macOS have them") {
    SAME(parted("Zoe" + acute), std::string{"Z|o|e" + acute});
    SAME(parted("o" + acute + "\xcc\xa8" + "x"), std::string{"o" + acute + "\xcc\xa8|x"});
    SAME(parted(acute + "a"), std::string{acute + "|a"});
    SAME(parted("\x01" + acute), std::string{"\x01|" + acute});
    SAME(parted("a\r\nb"), std::string{"a|\r|\n|b"});
}

// A letter with a hundred combining marks does not make one cell of two hundred bytes: a cluster
// holds at most 30 characters after its first (UAX #15), and the marks past it start clusters of
// their own. Walking back finds the same boundaries as walking forward.
TRIAL("Text: a cluster holds at most thirty marks after its letter") {
    std::string given = "e";

    for (int one = 0; one < 100; ++one) given += acute;

    SAME(text::after(given, 0), std::size_t{1 + 30 * 2});
    SAME(text::after(given, 61), std::size_t{61 + 31 * 2});
    SAME(text::before(given, 61), std::size_t{0});
    SAME(text::before(given, given.size()), std::size_t{61 + 31 * 2 + 31 * 2});
    SAME(text::width(given), 4);
}

// Korean from macOS comes as jamo: an initial, a vowel and a final make one syllable.
TRIAL("Text: Hangul jamo make syllables") {
    const std::string han = "\xe1\x84\x92\xe1\x85\xa1\xe1\x86\xab";
    const std::string geul = "\xe1\x84\x80\xe1\x85\xb3\xe1\x86\xaf";

    SAME(parted(han + geul), han + "|" + geul);
    SAME(text::width(han + geul), 4);
    SAME(parted("\xea\xb0\x80\xe1\x86\xa8"), std::string{"\xea\xb0\x80\xe1\x86\xa8"});
}

// Regional indicators pair up from the left: three make a flag and a lone half.
TRIAL("Text: flags are pairs of regional indicators") {
    SAME(parted(poland + "\xf0\x9f\x87\xa9"), poland + "|\xf0\x9f\x87\xa9");
    SAME(text::width(poland), 2);
    SAME(text::width(poland + "\xf0\x9f\x87\xa9"), 3);
}

// A terminal that joins draws a programmer and a dark thumb in two cells; one that does not draws
// a woman and a laptop, a thumb and a colour, each in two.
TRIAL("Text: emoji sequences follow the terminal's rendering") {
    SAME(parted(woman + zwj + laptop + "!", joined), woman + zwj + laptop + "|!");
    SAME(text::width(woman + zwj + laptop, joined), 2);
    SAME(parted(thumb + tone, joined), thumb + tone);
    SAME(text::width(thumb + tone, joined), 2);
    SAME(text::width(heart + selector, joined), 2);

    SAME(parted(woman + zwj + laptop + "!", apart), woman + zwj + "|" + laptop + "|!");
    SAME(text::width(woman + zwj + laptop, apart), 4);
    SAME(parted(thumb + tone, apart), thumb + "|" + tone);
    SAME(text::width(thumb + tone, apart), 4);
    SAME(text::width(heart + selector, apart), 1);
}

TRIAL("Text: a format character alone shows nothing, a mark alone takes a cell") {
    SAME(text::width("a\xe2\x80\x8b" "b"), 2);
    SAME(text::width(acute), 1);
    SAME(text::width("\xe2\x80\x8d"), 0);
}

// Walking back finds the same boundaries as walking forward, for strings mixed from every kind of
// character, in both renderings.
TRIAL("Text: walking back agrees with walking forward") {
    const std::vector<std::string> pieces{"a", "\xc5\x82", acute, "\xcc\xa8", zwj, woman, laptop, thumb, tone,
                                          heart, selector, "\xf0\x9f\x87\xb5", "\xf0\x9f\x87\xb1", "\xe4\xbd\xa0",
                                          "\xe1\x84\x92", "\xe1\x85\xa1", "\xe1\x86\xab", "\xea\xb0\x80", "\r", "\n",
                                          "\xe2\x80\x8b", " "};

    std::mt19937 random{20261013};

    for (const Rendering how : {joined, apart}) {
        for (int round = 0; round < 3000; ++round) {
            std::string given;

            const int many = 1 + static_cast<int>(random() % 12);

            for (int one = 0; one < many; ++one) given += pieces[random() % pieces.size()];

            if (backward(given, how) != forward(given, how)) {
                OUGHT(false);

                return;
            }
        }
    }
}

// A control character is shown, never executed: C0 and DEL as their pictures, C1 and the
// characters that turn the writing direction as the replacement character.
TRIAL("Text: shown() makes controls and direction marks harmless") {
    OUGHT(text::shown(U'\x1B') == U'\x241B');
    OUGHT(text::shown(U'\x7F') == U'\x2421');
    OUGHT(text::shown(U'\x9B') == U'\xFFFD');
    OUGHT(text::shown(U'\x202E') == U'\xFFFD');
    OUGHT(text::shown(U'a') == U'a');
    OUGHT(text::in_word(U'_'));
    OUGHT(text::in_word(U'\x142'));
    OUGHT(!text::in_word(U'-'));
}

// Lines break after spaces, a word wider than the line is cut, a line break ends a line, and a
// wide character counts two cells.
TRIAL("Text: wrapped() breaks lines after spaces and cuts only words wider than a line") {
    SAME(lines("Zażółć gęślą jaźń", 10), (std::vector<std::string>{"Zażółć ", "gęślą jaźń"}));
    SAME(lines("Zażółć gęślą jaźń", 9), (std::vector<std::string>{"Zażółć ", "gęślą ", "jaźń"}));
    SAME(lines("abcdefghij", 4), (std::vector<std::string>{"abcd", "efgh", "ij"}));
    SAME(lines("one\ntwo three", 20), (std::vector<std::string>{"one", "two three"}));
    SAME(lines("\xe4\xbd\xa0\xe5\xa5\xbd\xe4\xb8\x96", 4), (std::vector<std::string>{"\xe4\xbd\xa0\xe5\xa5\xbd", "\xe4\xb8\x96"}));
    SAME(lines("", 5), (std::vector<std::string>{""}));
    SAME(lines("a\n", 5), (std::vector<std::string>{"a", ""}));
    SAME(text::wrapped("e" + acute + "e" + acute + "e", 2).size(), std::size_t{2});
}

TRIAL("Text: repaired() replaces each broken sequence, valid text stays") {
    SAME(text::repaired("za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87"), std::string{"za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87"});
    SAME(text::repaired("a\xff" "b\xe2\x82"), std::string{"a\xef\xbf\xbd" "b\xef\xbf\xbd"});
    SAME(text::repaired("\xed\xa0\x80"), std::string{"\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"});
}

// The simple case folding of Unicode: letters that differ only in case meet, in every script, ß
// folds to itself (it becomes two letters only in full folding), the final sigma meets sigma.
TRIAL("Text: folded() and lowered() follow the simple mappings of Unicode") {
    SAME(text::folded("ŁÓDŹ Ąę ÀÉÞ Ÿ"), std::string{"łódź ąę àéþ ÿ"});
    SAME(text::folded("ΣΑς ПРИВЕТ ЁЖ Ǆ"), std::string{"σασ привет ёж ǆ"});
    SAME(text::folded("× ß ẞ 1"), std::string{"× ß ß 1"});
    SAME(text::folded("a\xff" "B"), std::string{"a\xff" "b"});
    OUGHT(text::folded(U'\x13A0') == U'\x13A0');
    OUGHT(text::folded(U'\xAB70') == U'\x13A0');
    OUGHT(text::folded(U'\x1FD3') == U'\x390');
    SAME(text::lowered("ŁÓDŹ ΣΑΣ İ"), std::string{"łódź σασ İ"});
    SAME(text::lowered("HTTP/1.1"), std::string{"http/1.1"});
    SAME(text::trimmed("  \tpoirot \r\n"), std::string_view{"poirot"});
}

// Letters without their marks and strokes, in their own case, also where the marks stand apart
// after the letter; letters that are two, other characters and broken bytes stay.
TRIAL("Text: unmarked() gives the base letters") {
    SAME(text::unmarked("Zażółć gęślą jaźń ŻÓŁW"), std::string{"Zazolc gesla jazn ZOLW"});
    SAME(text::unmarked("Ærø Đuro Ħal Ŧ ı"), std::string{"Æro Duro Hal T i"});
    SAME(text::unmarked("crème Ångström Ÿ ά Ά й Й"), std::string{"creme Angstrom Y α Α и И"});
    SAME(text::unmarked("a\u0328e\u0301"), std::string{"ae"});
    SAME(text::unmarked("ß æ × 1 Ж"), std::string{"ß æ × 1 Ж"});
    SAME(text::unmarked("a\xff" "ą"), std::string{"a\xff" "a"});
    SAME(text::unmarked(""), std::string{});
}

// The common order: a letter with marks stands with its base and after it when the rest is equal,
// lowercase before uppercase, spaces and punctuation before digits, digits before letters.
TRIAL("Text: collation, marks and case come after the letter, punctuation and digits before letters") {
    const Collation common{Language{"en-US"}};

    SAME(ordered(common, {"Zebra", "éclair", "eclair", "Eclair", "apple", "Äpfel", "2001", "-x", "zoo"}),
         (std::vector<std::string>{"-x", "2001", "Äpfel", "apple", "eclair", "Eclair", "éclair", "Zebra", "zoo"}));

    OUGHT(common.compare("a", "b") < 0);
    OUGHT(common.compare("b", "a") > 0);
    OUGHT(common.compare("same", "same") == 0);

    // A shorter text that begins the longer one comes first.
    OUGHT(common.compare("Alien", "Aliens") < 0);
}

// Polish puts its letters after their bases: a < ą < b, l < ł < m, z < ź < ż, and ó after o.
TRIAL("Text: collation, Polish letters stand after their bases") {
    const Collation polish{Language{"pl-PL"}};

    SAME(ordered(polish, {"żaba", "zebra", "źrebię", "łoś", "lis", "mysz", "ąb", "b", "a", "ósmy", "ozon", "pies"}),
         (std::vector<std::string>{"a", "ąb", "b", "lis", "łoś", "mysz", "ozon", "ósmy", "pies", "zebra", "źrebię", "żaba"}));

    // Uppercase letters keep the same places.
    SAME(ordered(polish, {"Żółw", "Zorro", "Łódź", "Lublin"}), (std::vector<std::string>{"Lublin", "Łódź", "Zorro", "Żółw"}));

    // Without the tailoring ą is a with a mark and stands before b, as in German.
    const Collation common{Language{"de-DE"}};

    SAME(ordered(common, {"az", "ąb"}), (std::vector<std::string>{"ąb", "az"}));
    SAME(ordered(polish, {"az", "ąb"}), (std::vector<std::string>{"az", "ąb"}));
}

// Czech and Slovak read ch as one letter after h, and č, ř, š, ž as letters after their bases.
TRIAL("Text: collation, Czech ch comes after h, and č after c") {
    const Collation czech{Language{"cs"}};

    SAME(ordered(czech, {"chata", "hrad", "cibule", "čaj", "dům", "ivan", "Chrudim"}),
         (std::vector<std::string>{"cibule", "čaj", "dům", "hrad", "chata", "Chrudim", "ivan"}));

    const Collation slovak{Language{"sk-SK"}};

    SAME(ordered(slovak, {"ôsmy", "oko", "päť", "ändern", "auto", "b"}),
         (std::vector<std::string>{"auto", "ändern", "b", "oko", "ôsmy", "päť"}));

    // In English ch is just c and h.
    SAME(ordered(Collation{Language{"en"}}, {"chata", "hrad", "cibule"}), (std::vector<std::string>{"chata", "cibule", "hrad"}));
}

// Spanish ñ after n; German ß as ss, æ as ae; ł and ø with their base letters in the common order.
TRIAL("Text: collation, ñ in Spanish, ß and æ as two letters, ł and ø by their bases") {
    SAME(ordered(Collation{Language{"es-ES"}}, {"ñu", "nube", "oso"}), (std::vector<std::string>{"nube", "ñu", "oso"}));
    // Elsewhere ñ is n with a mark, so the shorter word comes first.
    SAME(ordered(Collation{Language{"en"}}, {"ñu", "nube", "oso"}), (std::vector<std::string>{"ñu", "nube", "oso"}));

    const Collation german{Language{"de"}};

    SAME(ordered(german, {"Strauß", "Strausberg", "Straus", "Strauss"}),
         (std::vector<std::string>{"Straus", "Strausberg", "Strauss", "Strauß"}));
    SAME(ordered(german, {"Aesop", "Æneid", "Afrika"}), (std::vector<std::string>{"Æneid", "Aesop", "Afrika"}));
    SAME(ordered(Collation{Language{"en-US"}}, {"Lyon", "Łódź", "Lima"}), (std::vector<std::string>{"Lima", "Łódź", "Lyon"}));
    SAME(ordered(Collation{Language{"en-US"}}, {"Oslo", "Ørsted", "Osaka", "Paris"}), (std::vector<std::string>{"Ørsted", "Osaka", "Oslo", "Paris"}));
}

// Cyrillic goes by its alphabet: й after и, ё with е, and the Ukrainian і, ї, ґ, є in their places;
// Greek by its own, accents as marks; Latin before Greek before Cyrillic.
TRIAL("Text: collation, Cyrillic and Greek by their alphabets") {
    const Collation russian{Language{"ru-RU"}};

    SAME(ordered(russian, {"яблоко", "ёж", "еда", "йога", "игра", "кот", "Юла", "жук"}),
         (std::vector<std::string>{"еда", "ёж", "жук", "игра", "йога", "кот", "Юла", "яблоко"}));

    const Collation ukrainian{Language{"uk-UA"}};

    SAME(ordered(ukrainian, {"їжак", "іній", "йод", "ґанок", "гора", "дім", "єнот", "ехо", "иній"}),
         (std::vector<std::string>{"гора", "ґанок", "дім", "ехо", "єнот", "иній", "іній", "їжак", "йод"}));

    SAME(ordered(Collation{Language{"el"}}, {"ωμέγα", "άλφα", "βήτα", "αλφα"}), (std::vector<std::string>{"αλφα", "άλφα", "βήτα", "ωμέγα"}));

    SAME(ordered(Collation{Language{"en-US"}}, {"дом", "home", "οίκος"}), (std::vector<std::string>{"home", "οίκος", "дом"}));
}

// A mark written apart, after its letter, weighs as the same letter written whole; the texts stay
// distinct, so only the bytes tell them apart, and two different texts never compare equal.
TRIAL("Text: collation, a separate combining mark weighs as the composed letter") {
    const Collation common{Language{"en-US"}};

    const std::string composed = "caf\xc3\xa9";
    const std::string apart = "cafe\xcc\x81";

    OUGHT(common.compare(composed, apart) != 0);
    OUGHT(common.compare("cafe", apart) < 0);
    OUGHT(common.compare(apart, "cafes") < 0);
    OUGHT(common.compare(composed, "cafes") < 0);

    // Hangul syllables keep the order of their code points, which is that of the alphabet.
    SAME(ordered(common, {"하나", "가방", "나무"}), (std::vector<std::string>{"가방", "나무", "하나"}));
}

// A key is the levels and then the text itself, which is what tells apart two texts that weigh
// alike; so it ends with the text, in both orders, also with marks composed and written apart.
TRIAL("Text: collation, a key ends with its text") {
    for (const Case cased : {Case::ignored, Case::considered}) {
        const Collation polish{Language{"pl"}, cased};

        for (const std::string text :
             {"", "abc", "Caf\xc3\xa9 2", "cafe\xcc\x81", "\xc5\x82\xc3\xb3" "d\xc5\xba\xcc\x88"}) {
            const std::string key = polish.key(text);

            OUGHT(key.size() > text.size());
            OUGHT(key.ends_with(text));
        }
    }
}

// The language is the part of the code before the region, in any case; an unknown one is common.
TRIAL("Text: collation, the language code picks the tailoring, region and case aside") {
    OUGHT(Collation{Language{"PL_pl"}}.compare("łza", "mama") == Collation{Language{"pl"}}.compare("łza", "mama"));
    OUGHT(Collation{Language{"pl"}}.compare("łza", "lza") > 0);
    OUGHT(Collation{Language{"xx"}}.compare("ąb", "az") < 0);
    SAME(std::string{Collation{Language{"pt_br"}}.language().tag()}, std::string{"pt-BR"});
}

// Numbers are numbers: a shorter one comes first, then digit by digit, so "Part 2" stands before
// "Part 10"; leading zeros make no other number, and only the bytes tell "007" from "7". Without
// the numeric reading digits go one by one.
TRIAL("Text: collation, numbers in a text compare as numbers") {
    const Collation common{Language{"en-US"}};

    SAME(ordered(common, {"Part 10", "Part 2", "Part 1", "Part 100", "Part 02", "Part"}),
         (std::vector<std::string>{"Part", "Part 1", "Part 02", "Part 2", "Part 10", "Part 100"}));
    SAME(ordered(common, {"2001", "1999", "300", "0"}), (std::vector<std::string>{"0", "300", "1999", "2001"}));
    SAME(ordered(common, {"S01E10", "S01E9", "S02E01", "S1E2"}), (std::vector<std::string>{"S1E2", "S01E9", "S01E10", "S02E01"}));

    OUGHT(common.compare("007", "7") != 0);
    OUGHT(common.compare("007", "8") < 0);

    // Numbers stand before letters, as digits do, and a number of any length before a letter.
    OUGHT(common.compare("99999 x", "a") < 0);

    const Collation digits{Language{""}, Case::ignored, false};

    SAME(ordered(digits, {"Part 10", "Part 2"}), (std::vector<std::string>{"Part 10", "Part 2"}));
}

// An exact collation keeps the order of code points, uppercase before lowercase and ASCII before
// the rest, as the bytes have it, but reads numbers as numbers.
TRIAL("Text: collation, an exact order goes by code points, with numbers as numbers") {
    const Collation exact{Language{"pl"}, Case::considered};

    SAME(ordered(exact, {"b", "a", "B", "ą", "Z", "file10", "file9", "file-", "file:"}),
         (std::vector<std::string>{"B", "Z", "a", "b", "file-", "file9", "file10", "file:", "ą"}));

    const Collation bytes{Language{"pl"}, Case::considered, false};

    SAME(ordered(bytes, {"file10", "file9"}), (std::vector<std::string>{"file10", "file9"}));
}
TRIAL("Text: a language is its tag, written the one way") {
    SAME(std::string{Language{"PL_pl"}.tag()}, std::string{"pl-PL"});
    SAME(std::string{Language{"pt-br"}.base()}, std::string{"pt"});
    OUGHT(Language{"zh-cn"} == Language{"zh-CN"});

    const Language was = text::current();

    text::choose(Language{"pl-PL"});
    OUGHT(text::current() == Language{"pl-PL"});
    SAME(text::number(1.5, 1), std::string{"1,5"});
    text::choose(was);
}

// The forms of the plural for whole numbers, as twa's texts use them.
TRIAL("Text: plural forms follow the CLDR rules of each language") {
    using text::Plural;

    const Language polish{"pl"};

    OUGHT(text::plural(1, polish) == Plural::one);
    OUGHT(text::plural(2, polish) == Plural::few);
    OUGHT(text::plural(5, polish) == Plural::many);
    OUGHT(text::plural(12, polish) == Plural::many);
    OUGHT(text::plural(22, polish) == Plural::few);
    OUGHT(text::plural(0, polish) == Plural::many);
    OUGHT(text::plural(21, Language{"ru"}) == Plural::one);
    OUGHT(text::plural(11, Language{"uk"}) == Plural::many);
    OUGHT(text::plural(3, Language{"cs"}) == Plural::few);
    OUGHT(text::plural(5, Language{"sk"}) == Plural::other);
    OUGHT(text::plural(0, Language{"fr"}) == Plural::one);
    OUGHT(text::plural(1'000'000, Language{"es"}) == Plural::many);
    OUGHT(text::plural(1, Language{"ja"}) == Plural::other);
    OUGHT(text::plural(1, Language{"xx"}) == Plural::one);
    OUGHT(text::plurals(polish) == (std::vector<Plural>{Plural::one, Plural::few, Plural::many}));
    OUGHT(text::plurals(Language{"en"}) == (std::vector<Plural>{Plural::one, Plural::other}));
    OUGHT(text::plurals(Language{"it"}) == (std::vector<Plural>{Plural::one, Plural::other, Plural::many}));
    OUGHT(text::plurals(Language{"zh"}) == (std::vector<Plural>{Plural::other}));
}

TRIAL("Text: numbers are written with the signs and the grouping of the language") {
    SAME(text::number(1234567.891, 2, Language{"en-US"}), std::string{"1,234,567.89"});
    SAME(text::number(1234567.891, 2, Language{"de"}), std::string{"1.234.567,89"});
    SAME(text::number(1234.5, 1, Language{"pl"}), std::string{"1234,5"});
    SAME(text::number(12345.5, 1, Language{"pl"}), "12" + space + "345,5");
    SAME(text::number(1234.0, 0, Language{"fr"}), std::string{"1\xe2\x80\xaf" "234"});
    SAME(text::number(1234.0, 0, Language{"es"}), std::string{"1234"});
    SAME(text::number(-0.04, 1, Language{"en"}), std::string{"0.0"});
    SAME(text::number(-2.5, 0, Language{"en"}), std::string{"-2"});
    SAME(text::number(0.125, 2, Language{"ja"}), std::string{"0.12"});
}

TRIAL("Text: sizes are written in decimal units, with one decimal below a hundred") {
    SAME(text::size(512, Language{"en"}), "512" + space + "B");
    SAME(text::size(1000, Language{"en"}), "1.0" + space + "kB");
    SAME(text::size(1'234'567'890, Language{"pl"}), "1,2" + space + "GB");
    SAME(text::size(123'456'789, Language{"pl"}), "123" + space + "MB");
    SAME(text::size(999'960, Language{"en"}), "1.0" + space + "MB");
    SAME(text::size(2'500'000'000, Language{"fr"}), "2,5" + space + "Go");
    SAME(text::size(3'000'000, Language{"ru"}), "3,0" + space + "МБ");
}

TRIAL("Text: dates and times are written in numbers, in the order of the language") {
    const auto when = at_local(2026, 11, 8, 14, 52);

    SAME(text::date(when, text::Style::date, Language{"pl-PL"}), std::string{"08.11.2026"});
    SAME(text::date(when, text::Style::time, Language{"pl-PL"}), std::string{"14:52"});
    SAME(text::date(when, text::Style::stamp, Language{"pl-PL"}), std::string{"08.11.2026 14:52"});
    SAME(text::date(when, text::Style::date, Language{"en-US"}), std::string{"11/8/2026"});
    SAME(text::date(when, text::Style::time, Language{"en-US"}), "2:52" + space + "PM");
    SAME(text::date(when, text::Style::date, Language{"en-GB"}), std::string{"08/11/2026"});
    SAME(text::date(when, text::Style::time, Language{"en-GB"}), std::string{"14:52"});
    SAME(text::date(when, text::Style::date, Language{"cs"}), std::string{"8. 11. 2026"});
    SAME(text::date(when, text::Style::date, Language{"sv"}), std::string{"2026-11-08"});
    SAME(text::date(when, text::Style::date, Language{"ja"}), std::string{"2026/11/08"});
    SAME(text::date(when, text::Style::date, Language{"ko"}), std::string{"2026. 11. 8."});
    SAME(text::date(when, text::Style::date, Language{"nl"}), std::string{"08-11-2026"});
    SAME(text::date(at_local(2026, 1, 2, 0, 5), text::Style::time, Language{"en-US"}), "12:05" + space + "AM");
}

// The system's language is a proposal for the program, which chooses its own. POSIX looks at
// LC_ALL, LC_MESSAGES and LANG in turn; the C locale names no language.
TRIAL("Text: proposed() gives the language of the system's locale") {
#ifdef _WIN32
    if (const auto found = text::proposed()) OUGHT(!found->base().empty());
#else
    const auto kept = [](const char* name) -> std::optional<std::string> {
        const char* given = std::getenv(name);

        return given != nullptr ? std::optional<std::string>{given} : std::nullopt;
    };
    const auto put = [](const char* name, const std::optional<std::string>& value) {
        if (value) {
            ::setenv(name, value->c_str(), 1);
        } else {
            ::unsetenv(name);
        }
    };

    const std::optional<std::string> all = kept("LC_ALL");
    const std::optional<std::string> messages = kept("LC_MESSAGES");
    const std::optional<std::string> lang = kept("LANG");

    put("LC_ALL", std::nullopt);
    put("LC_MESSAGES", std::nullopt);
    put("LANG", "pl_PL.UTF-8@euro");

    const std::optional<Language> polish = text::proposed();

    put("LC_MESSAGES", "cs_CZ");

    const std::optional<Language> czech = text::proposed();

    put("LC_ALL", "C.UTF-8");

    const std::optional<Language> none = text::proposed();

    put("LC_ALL", all);
    put("LC_MESSAGES", messages);
    put("LANG", lang);

    OUGHT(polish == Language{"pl-PL"});
    OUGHT(czech == Language{"cs-CZ"});
    OUGHT(!none.has_value());
#endif
}
