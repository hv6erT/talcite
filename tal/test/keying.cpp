#include <optional>
#include <string>
#include <vector>

#include "tal/key.hpp"

#include "tal/testing.hpp"

using tal::Code;
using tal::Key;
using tal::Mods;

// The written form of a key as a person sees it: in language files, in menu hints and in help.
// There is one form for every program on tal, so that a shortcut written in data and a shortcut
// pressed can be compared by one rule (`alike`). Moved from talcite with the key; letter case
// folds by tal::text, whose trials cover the folding itself.

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

            const std::optional<Key> back = Key::from_string(tal::to_string(one));

            OUGHT(back.has_value());
            OUGHT(back.has_value() && *back == one);
        }
}

TRIAL("Key: a letter with a modifier, written as on the keycap") {
    OUGHT(read("Alt+K", Key{U'k', Mods{.alt = true}}));
    SAME(tal::to_string(Key{U'k', Mods{.alt = true}}), std::string{"Alt+K"});

    OUGHT(read("Ctrl+R", Key{U'r', Mods{.ctrl = true}}));
    SAME(tal::to_string(Key{U'r', Mods{.ctrl = true}}), std::string{"Ctrl+R"});
}

TRIAL("Key: letter case in the written form does not matter") {
    OUGHT(read("alt+k", Key{U'k', Mods{.alt = true}}));
    OUGHT(read("CTRL+SHIFT+f7", Key{Code::f7, Mods{.ctrl = true, .shift = true}}));
    OUGHT(read("pgdn", Key{Code::pagedown}));
}

TRIAL("Key: modifiers are read in any order and written in one") {
    const Key one{Code::f7, Mods{true, true, true}};

    OUGHT(read("Shift+Alt+Ctrl+F7", one));
    SAME(tal::to_string(one), std::string{"Ctrl+Alt+Shift+F7"});
}

// Shift with a letter means nothing (it is what makes the letter uppercase), so writing leaves it
// out and reading drops it.
TRIAL("Key: Shift with a letter disappears") {
    OUGHT(read("Shift+K", Key{U'k'}));
    SAME(tal::to_string(Key{U'k', Mods{.shift = true}}), std::string{"K"});
}

TRIAL("Key: a non-Latin character, space and plus") {
    OUGHT(read("Alt+\xc5\x82", Key{U'ł', Mods{.alt = true}}));
    SAME(tal::to_string(Key{U'ł', Mods{.alt = true}}), std::string{"Alt+\xc5\x82"});

    OUGHT(read("Ctrl+Space", Key{U' ', Mods{.ctrl = true}}));
    OUGHT(read("Shift+Space", Key{U' '}));
    SAME(tal::to_string(Key{U' ', Mods{.ctrl = true}}), std::string{"Ctrl+Space"});

    OUGHT(read("Ctrl++", Key{U'+', Mods{.ctrl = true}}));
    SAME(tal::to_string(Key{U'+', Mods{.ctrl = true}}), std::string{"Ctrl++"});
}

// A modifier alone is not a key, also when nothing follows it: `Ctrl+` is a truncated form.
TRIAL("Key: a modifier without a key gives nothing") {
    OUGHT(!Key::from_string("Alt+").has_value());
    OUGHT(!Key::from_string("Ctrl+Alt+").has_value());
}

// Half a shortcut would be a different shortcut, so anything that cannot be read in full gives
// nothing.
TRIAL("Key: a malformed written form gives nothing") {
    for (const char* bad : {"", "Ctrl+", "Alt+Alt+K", "Alt+KK", "Insert", "Foo", "Ctrl+\x80", "Alt+\x01", "Alt+\xef\xbf\xbd"})
        OUGHT(!Key::from_string(bad).has_value());
}

// Plus is a key too (in a panel it selects by pattern), so on its own it is a written form, not a
// leftover after a modifier.
TRIAL("Key: a plus alone is a key") { OUGHT(read("+", Key{U'+'})); }

TRIAL("Key: an empty key has no written form") { SAME(tal::to_string(Key{}), std::string{}); }

TRIAL("Key: alike compares a letter without case and Shift, a code together with Shift") {
    OUGHT(tal::alike(Key{U'k', Mods{.alt = true}}, Key{U'K', Mods{.alt = true, .shift = true}}));
    OUGHT(!tal::alike(Key{U'k', Mods{.alt = true}}, Key{U'k', Mods{.ctrl = true}}));
    OUGHT(!tal::alike(Key{U'k', Mods{.alt = true}}, Key{U'k'}));

    OUGHT(tal::alike(Key{Code::f7}, Key{Code::f7}));
    OUGHT(!tal::alike(Key{Code::f7}, Key{Code::f7, Mods{.shift = true}}));
    OUGHT(!tal::alike(Key{Code::f7}, Key{U'f'}));

    OUGHT(!tal::alike(Key{}, Key{}));
}

TRIAL("Key: a Cyrillic shortcut, written, read and compared with alike") {
    OUGHT(read("Alt+\xd0\x9b", Key{U'л', Mods{.alt = true}}));
    OUGHT(read("Alt+\xd0\xbb", Key{U'л', Mods{.alt = true}}));
    SAME(tal::to_string(Key{U'л', Mods{.alt = true}}), std::string{"Alt+\xd0\xbb"});

    OUGHT(tal::alike(Key{U'Л', Mods{.alt = true}}, Key{U'л', Mods{.alt = true, .shift = true}}));
    OUGHT(tal::alike(Key{U'Ą', Mods{.alt = true}}, Key{U'ą', Mods{.alt = true}}));
    OUGHT(!tal::alike(Key{U'л', Mods{.alt = true}}, Key{U'k', Mods{.alt = true}}));
}
