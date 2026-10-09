#include "proving.hpp"

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

// Zapis klawisza, jaki widzi człowiek — w plikach języków, w podpowiedziach menu i w pomocy. Jeden
// dla każdego programu na talcite, żeby skrót zapisany w danych i skrót naciśnięty dało się
// porównać jedną regułą (`alike`).

namespace {

bool read(const char* written, Key wanted) {
    const std::optional<Key> got = Key::from_string(written);

    return got.has_value() && *got == wanted;
}

}

TRIAL("Klawisz: każdy kod wraca z zapisu, z każdym zestawem modyfikatorów") {
    const std::vector<Mods> sets{{}, {true, false, false}, {false, true, false}, {false, false, true},
                                 {true, true, true}};

    for (int which = static_cast<int>(Code::escape); which <= static_cast<int>(Code::f12); ++which)
        for (const Mods with : sets) {
            const Key one{static_cast<Code>(which), with};

            const std::optional<Key> back = Key::from_string(ui::to_string(one));

            ought(back.has_value());
            ought(back.has_value() && *back == one);
        }
}

TRIAL("Klawisz: litera z modyfikatorem, pisana jak na klawiszu") {
    ought(read("Alt+K", Key{U'k', Mods{.alt = true}}));
    same(ui::to_string(Key{U'k', Mods{.alt = true}}), std::string{"Alt+K"});

    ought(read("Ctrl+R", Key{U'r', Mods{.ctrl = true}}));
    same(ui::to_string(Key{U'r', Mods{.ctrl = true}}), std::string{"Ctrl+R"});
}

TRIAL("Klawisz: wielkość liter w zapisie nie ma znaczenia") {
    ought(read("alt+k", Key{U'k', Mods{.alt = true}}));
    ought(read("CTRL+SHIFT+f7", Key{Code::f7, Mods{.ctrl = true, .shift = true}}));
    ought(read("pgdn", Key{Code::pagedown}));
}

TRIAL("Klawisz: modyfikatory w dowolnej kolejności, zapis w jednej") {
    const Key one{Code::f7, Mods{true, true, true}};

    ought(read("Shift+Alt+Ctrl+F7", one));
    same(ui::to_string(one), std::string{"Ctrl+Alt+Shift+F7"});
}

// Shift przy literze nic nie znaczy — wielką literę robi właśnie on — więc zapis go pomija,
// a odczyt zdejmuje.
TRIAL("Klawisz: Shift przy literze znika") {
    ought(read("Shift+K", Key{U'k'}));
    same(ui::to_string(Key{U'k', Mods{.shift = true}}), std::string{"K"});
}

TRIAL("Klawisz: znak spoza łaciny, spacja i plus") {
    ought(read("Alt+\xc5\x82", Key{U'ł', Mods{.alt = true}}));
    same(ui::to_string(Key{U'ł', Mods{.alt = true}}), std::string{"Alt+\xc5\x82"});

    ought(read("Ctrl+Space", Key{U' ', Mods{.ctrl = true}}));
    ought(read("Shift+Space", Key{U' '}));
    same(ui::to_string(Key{U' ', Mods{.ctrl = true}}), std::string{"Ctrl+Space"});

    ought(read("Ctrl++", Key{U'+', Mods{.ctrl = true}}));
    same(ui::to_string(Key{U'+', Mods{.ctrl = true}}), std::string{"Ctrl++"});
}

// Sam modyfikator nie jest klawiszem, także gdy po nim nic nie ma: `Ctrl+` to zapis urwany.
TRIAL("Klawisz: modyfikator bez klawisza daje pustkę") {
    ought(!Key::from_string("Alt+").has_value());
    ought(!Key::from_string("Ctrl+Alt+").has_value());
}

// Pół skrótu byłoby innym skrótem, więc wszystko, czego nie da się przeczytać w całości, daje pustkę.
TRIAL("Klawisz: zapis krzywy daje pustkę") {
    for (const char* bad : {"", "Ctrl+", "Alt+Alt+K", "Alt+KK", "Insert", "Foo", "Ctrl+\x80", "Alt+\x01"})
        ought(!Key::from_string(bad).has_value());
}

// Plus to też klawisz — w panelu zaznacza po wzorcu — więc sam jest zapisem, a nie resztką po
// modyfikatorze.
TRIAL("Klawisz: sam plus jest klawiszem") { ought(read("+", Key{U'+'})); }

TRIAL("Klawisz: pusty nie ma zapisu") { same(ui::to_string(Key{}), std::string{}); }

TRIAL("Klawisz: alike — litera bez wielkości i Shift, kod razem z Shift") {
    ought(ui::alike(Key{U'k', Mods{.alt = true}}, Key{U'K', Mods{.alt = true, .shift = true}}));
    ought(!ui::alike(Key{U'k', Mods{.alt = true}}, Key{U'k', Mods{.ctrl = true}}));
    ought(!ui::alike(Key{U'k', Mods{.alt = true}}, Key{U'k'}));

    ought(ui::alike(Key{Code::f7}, Key{Code::f7}));
    ought(!ui::alike(Key{Code::f7}, Key{Code::f7, Mods{.shift = true}}));
    ought(!ui::alike(Key{Code::f7}, Key{U'f'}));

    ought(!ui::alike(Key{}, Key{}));
}

// **Składanie proste z Unicode**, w blokach, z których pliki języków biorą litery: łacina z Latin-1
// i Latin Extended-A, greka, cyrylica. Wielka idzie w małą, mała zostaje, a litery, których złożenie
// to dwa znaki („ß", „İ", „ŉ"), zostają same sobą.
TRIAL("Klawisz: folded składa wielkość w łacinie, grece i cyrylicy") {
    const std::vector<std::pair<char32_t, char32_t>> pairs{
        {U'K', U'k'}, {U'Ä', U'ä'}, {U'Þ', U'þ'}, {U'Ą', U'ą'}, {U'Ł', U'ł'}, {U'Ž', U'ž'},
        {U'Ÿ', U'ÿ'}, {U'ſ', U's'}, {U'µ', U'μ'}, {U'Σ', U'σ'}, {U'ς', U'σ'}, {U'Ά', U'ά'},
        {U'Ώ', U'ώ'}, {U'ϐ', U'β'}, {U'Л', U'л'}, {U'Ё', U'ё'}, {U'Ї', U'ї'}, {U'Ґ', U'ґ'},
        {U'Ӏ', U'ӏ'}, {U'Ӂ', U'ӂ'}, {U'Ԯ', U'ԯ'}};

    for (const auto& [upper, lower] : pairs) {
        same(ui::folded(upper), lower);
        same(ui::folded(lower), lower);
    }

    // Oba końce każdego zakresu tablicy — przesunięty koniec zakresu inaczej przeszedłby niezauważony.
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
        same(ui::folded(upper), lower);
        same(ui::folded(lower), lower);
    }

    for (const char32_t kept : {U'k', U'ł', U'л', U'×', U'÷', U'ß', U'İ', U'ı', U'ĸ', U'ŉ', U'ΐ', U'·', U'Ꙁ',
                                U'ẞ', U'Ⴀ', U'0', U'+'})
        same(ui::folded(kept), kept);
}

// Złożone jest już złożone: drugie przejście nic nie zmienia, w żadnym punkcie badanych bloków.
TRIAL("Klawisz: folded drugi raz niczego nie zmienia") {
    for (char32_t one = 0; one < 0x600; ++one) same(ui::folded(ui::folded(one)), ui::folded(one));
}

TRIAL("Klawisz: skrót z cyrylicą — zapis, odczyt i alike") {
    ought(read("Alt+\xd0\x9b", Key{U'л', Mods{.alt = true}}));
    ought(read("Alt+\xd0\xbb", Key{U'л', Mods{.alt = true}}));
    same(ui::to_string(Key{U'л', Mods{.alt = true}}), std::string{"Alt+\xd0\xbb"});

    ought(ui::alike(Key{U'Л', Mods{.alt = true}}, Key{U'л', Mods{.alt = true, .shift = true}}));
    ought(ui::alike(Key{U'Ą', Mods{.alt = true}}, Key{U'ą', Mods{.alt = true}}));
    ought(!ui::alike(Key{U'л', Mods{.alt = true}}, Key{U'k', Mods{.alt = true}}));
}
