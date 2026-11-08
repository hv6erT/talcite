#include "tal/testing.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "talcite/event.hpp"

#include "folding.hpp"

using namespace talcite;
using ui::Code;
using ui::Key;
using ui::Mods;

// The written form of a key as a person sees it: in language files, in menu hints and in help.
// There is one form for every program on talcite, so that a shortcut written in data and a shortcut
// pressed can be compared by one rule (`alike`).

namespace {

// Tells whether the written form parses into exactly the wanted key.
bool read(const char* written, Key wanted) {
    const std::optional<Key> got = Key::from_string(written);

    return got.has_value() && *got == wanted;
}

}

TRIAL("Key: every code round-trips through its written form, with every set of modifiers") {
    const std::vector<Mods> sets{{}, {true, false, false}, {false, true, false}, {false, false, true},
                                 {true, true, true}};

    for (int which = static_cast<int>(Code::escape); which <= static_cast<int>(Code::f20); ++which)
        for (const Mods with : sets) {
            const Key one{static_cast<Code>(which), with};

            const std::optional<Key> back = Key::from_string(ui::to_string(one));

            OUGHT(back.has_value());
            OUGHT(back.has_value() && *back == one);
        }
}

TRIAL("Key: a letter with a modifier, written as on the keycap") {
    OUGHT(read("Alt+K", Key{U'k', Mods{.alt = true}}));
    SAME(ui::to_string(Key{U'k', Mods{.alt = true}}), std::string{"Alt+K"});

    OUGHT(read("Ctrl+R", Key{U'r', Mods{.ctrl = true}}));
    SAME(ui::to_string(Key{U'r', Mods{.ctrl = true}}), std::string{"Ctrl+R"});
}

TRIAL("Key: letter case in the written form does not matter") {
    OUGHT(read("alt+k", Key{U'k', Mods{.alt = true}}));
    OUGHT(read("CTRL+SHIFT+f7", Key{Code::f7, Mods{.ctrl = true, .shift = true}}));
    OUGHT(read("pgdn", Key{Code::pagedown}));
}

TRIAL("Key: modifiers are read in any order and written in one") {
    const Key one{Code::f7, Mods{true, true, true}};

    OUGHT(read("Shift+Alt+Ctrl+F7", one));
    SAME(ui::to_string(one), std::string{"Ctrl+Alt+Shift+F7"});
}

// Shift with a letter means nothing (it is what makes the letter uppercase), so writing leaves it
// out and reading drops it.
TRIAL("Key: Shift with a letter disappears") {
    OUGHT(read("Shift+K", Key{U'k'}));
    SAME(ui::to_string(Key{U'k', Mods{.shift = true}}), std::string{"K"});
}

TRIAL("Key: a non-Latin character, space and plus") {
    OUGHT(read("Alt+\xc5\x82", Key{U'ł', Mods{.alt = true}}));
    SAME(ui::to_string(Key{U'ł', Mods{.alt = true}}), std::string{"Alt+\xc5\x82"});

    OUGHT(read("Ctrl+Space", Key{U' ', Mods{.ctrl = true}}));
    OUGHT(read("Shift+Space", Key{U' '}));
    SAME(ui::to_string(Key{U' ', Mods{.ctrl = true}}), std::string{"Ctrl+Space"});

    OUGHT(read("Ctrl++", Key{U'+', Mods{.ctrl = true}}));
    SAME(ui::to_string(Key{U'+', Mods{.ctrl = true}}), std::string{"Ctrl++"});
}

// A modifier alone is not a key, also when nothing follows it: `Ctrl+` is a truncated form.
TRIAL("Key: a modifier without a key gives nothing") {
    OUGHT(!Key::from_string("Alt+").has_value());
    OUGHT(!Key::from_string("Ctrl+Alt+").has_value());
}

// Half a shortcut would be a different shortcut, so anything that cannot be read in full gives
// nothing.
TRIAL("Key: a malformed written form gives nothing") {
    for (const char* bad : {"", "Ctrl+", "Alt+Alt+K", "Alt+KK", "Insert", "Foo", "Ctrl+\x80", "Alt+\x01"})
        OUGHT(!Key::from_string(bad).has_value());
}

// Plus is a key too (in a panel it selects by pattern), so on its own it is a written form, not a
// leftover after a modifier.
TRIAL("Key: a plus alone is a key") { OUGHT(read("+", Key{U'+'})); }

TRIAL("Key: an empty key has no written form") { SAME(ui::to_string(Key{}), std::string{}); }

TRIAL("Key: alike compares a letter without case and Shift, a code together with Shift") {
    OUGHT(ui::alike(Key{U'k', Mods{.alt = true}}, Key{U'K', Mods{.alt = true, .shift = true}}));
    OUGHT(!ui::alike(Key{U'k', Mods{.alt = true}}, Key{U'k', Mods{.ctrl = true}}));
    OUGHT(!ui::alike(Key{U'k', Mods{.alt = true}}, Key{U'k'}));

    OUGHT(ui::alike(Key{Code::f7}, Key{Code::f7}));
    OUGHT(!ui::alike(Key{Code::f7}, Key{Code::f7, Mods{.shift = true}}));
    OUGHT(!ui::alike(Key{Code::f7}, Key{U'f'}));

    OUGHT(!ui::alike(Key{}, Key{}));
}

// Simple Unicode case folding, in the blocks from which language files take their letters: Latin
// from Latin-1 and Latin Extended-A, Greek, Cyrillic. An uppercase letter becomes lowercase, a
// lowercase one stays, and letters whose folding is two characters ("ß", "İ", "ŉ") stay themselves.
TRIAL("Key: folded folds case in Latin, Greek and Cyrillic") {
    const std::vector<std::pair<char32_t, char32_t>> pairs{
        {U'K', U'k'}, {U'Ä', U'ä'}, {U'Þ', U'þ'}, {U'Ą', U'ą'}, {U'Ł', U'ł'}, {U'Ž', U'ž'},
        {U'Ÿ', U'ÿ'}, {U'ſ', U's'}, {U'µ', U'μ'}, {U'Σ', U'σ'}, {U'ς', U'σ'}, {U'Ά', U'ά'},
        {U'Ώ', U'ώ'}, {U'ϐ', U'β'}, {U'Л', U'л'}, {U'Ё', U'ё'}, {U'Ї', U'ї'}, {U'Ґ', U'ґ'},
        {U'Ӏ', U'ӏ'}, {U'Ӂ', U'ӂ'}, {U'Ԯ', U'ԯ'}};

    for (const auto& [upper, lower] : pairs) {
        SAME(ui::folded(upper), lower);
        SAME(ui::folded(lower), lower);
    }

    // Both ends of every range of the table: a shifted range end would otherwise go unnoticed.
    const std::vector<std::pair<char32_t, char32_t>> edges{
        {0x00C0, 0x00E0}, {0x00D6, 0x00F6}, {0x00D8, 0x00F8}, {0x0100, 0x0101}, {0x012E, 0x012F},
        {0x0132, 0x0133}, {0x0136, 0x0137}, {0x0139, 0x013A}, {0x0147, 0x0148}, {0x014A, 0x014B},
        {0x0176, 0x0177}, {0x0179, 0x017A}, {0x017D, 0x017E}, {0x0370, 0x0371}, {0x0372, 0x0373},
        {0x0376, 0x0377}, {0x037F, 0x03F3}, {0x0386, 0x03AC}, {0x0388, 0x03AD}, {0x038A, 0x03AF},
        {0x038C, 0x03CC}, {0x038E, 0x03CD}, {0x038F, 0x03CE}, {0x0391, 0x03B1}, {0x03A1, 0x03C1},
        {0x03A3, 0x03C3}, {0x03AB, 0x03CB}, {0x03CF, 0x03D7}, {0x03D0, 0x03B2}, {0x03D1, 0x03B8},
        {0x03D5, 0x03C6}, {0x03D6, 0x03C0}, {0x03D8, 0x03D9}, {0x03EE, 0x03EF}, {0x03F0, 0x03BA},
        {0x03F1, 0x03C1}, {0x03F4, 0x03B8}, {0x03F5, 0x03B5}, {0x03F7, 0x03F8}, {0x03F9, 0x03F2},
        {0x03FA, 0x03FB}, {0x03FD, 0x037B}, {0x03FF, 0x037D}, {0x0400, 0x0450}, {0x040F, 0x045F},
        {0x0410, 0x0430}, {0x042F, 0x044F}, {0x0460, 0x0461}, {0x0480, 0x0481}, {0x048A, 0x048B},
        {0x04BE, 0x04BF}, {0x04C0, 0x04CF}, {0x04C1, 0x04C2}, {0x04CD, 0x04CE}, {0x04D0, 0x04D1},
        {0x052E, 0x052F}};

    for (const auto& [upper, lower] : edges) {
        SAME(ui::folded(upper), lower);
        SAME(ui::folded(lower), lower);
    }

    for (const char32_t kept : {U'k', U'ł', U'л', U'×', U'÷', U'ß', U'İ', U'ı', U'ĸ', U'ŉ', U'ΐ', U'·', U'Ꙁ',
                                U'ẞ', U'Ⴀ', U'0', U'+'})
        SAME(ui::folded(kept), kept);
}

// Folded is already folded: a second pass changes nothing, at any code point of the blocks
// examined.
TRIAL("Key: folding a second time changes nothing") {
    for (char32_t one = 0; one < 0x600; ++one) SAME(ui::folded(ui::folded(one)), ui::folded(one));
}

TRIAL("Key: a Cyrillic shortcut, written, read and compared with alike") {
    OUGHT(read("Alt+\xd0\x9b", Key{U'л', Mods{.alt = true}}));
    OUGHT(read("Alt+\xd0\xbb", Key{U'л', Mods{.alt = true}}));
    SAME(ui::to_string(Key{U'л', Mods{.alt = true}}), std::string{"Alt+\xd0\xbb"});

    OUGHT(ui::alike(Key{U'Л', Mods{.alt = true}}, Key{U'л', Mods{.alt = true, .shift = true}}));
    OUGHT(ui::alike(Key{U'Ą', Mods{.alt = true}}, Key{U'ą', Mods{.alt = true}}));
    OUGHT(!ui::alike(Key{U'л', Mods{.alt = true}}, Key{U'k', Mods{.alt = true}}));
}
