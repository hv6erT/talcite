#include "talcite/event.hpp"

#include <array>
#include <cstddef>
#include <string>

#include "talcite/encoding.hpp"

#include "folding.hpp"

namespace talcite::ui {

namespace {

struct Named {
    Code code;
    std::string_view name;
};

// **Jedna pisownia na klawisz.** Zapis czyta się bez względu na wielkość liter, ale synonimów nie
// ma: `Ins`, a nie także `Insert`. Dwie pisownie tego samego klawisza w plikach języków to dwa
// miejsca, w których próba zderzeń musiałaby wiedzieć, że chodzi o jedno.
constexpr std::array<Named, 26> names{{
    {Code::escape, "Esc"},     {Code::enter, "Enter"},   {Code::tab, "Tab"},
    {Code::backspace, "Backspace"}, {Code::insert, "Ins"}, {Code::remove, "Del"},
    {Code::up, "Up"},          {Code::down, "Down"},     {Code::left, "Left"},
    {Code::right, "Right"},    {Code::home, "Home"},     {Code::end, "End"},
    {Code::pageup, "PgUp"},    {Code::pagedown, "PgDn"}, {Code::f1, "F1"},
    {Code::f2, "F2"},          {Code::f3, "F3"},         {Code::f4, "F4"},
    {Code::f5, "F5"},          {Code::f6, "F6"},         {Code::f7, "F7"},
    {Code::f8, "F8"},          {Code::f9, "F9"},         {Code::f10, "F10"},
    {Code::f11, "F11"},        {Code::f12, "F12"},
}};

bool same(std::string_view one, std::string_view two) {
    if (one.size() != two.size()) return false;

    for (std::size_t at = 0; at < one.size(); ++at)
        if (folded(static_cast<unsigned char>(one[at])) != folded(static_cast<unsigned char>(two[at])))
            return false;

    return true;
}

// Przedrostek modyfikatora zdejmuje się z początku i od razu mówi, który to był. Kolejność
// w zapisie jest dowolna, ale każdy modyfikator wolno podać raz.
bool stripped(std::string_view& rest, std::string_view prefix, bool& into) {
    if (!same(rest.substr(0, prefix.size()), prefix)) return false;

    if (into) return false;

    into = true;
    rest.remove_prefix(prefix.size());

    return true;
}

}

// **Zapis klawisza, jaki widzi człowiek**: modyfikatory `Ctrl+`, `Alt+`, `Shift+` i klawisz —
// nazwa z tabeli, `Space` albo jeden znak. Litera wychodzi złożona (`folded`, więc mała także
// w cyrylicy: „Alt+Л" to `л`), bo `Alt+K` i `Alt+k` to ten sam klawisz (`alike` nie patrzy na
// wielkość), a Shift przy literze nic nie znaczy.
// Wszystko, czego nie da się przeczytać w całości, daje pustkę: pół skrótu byłoby innym skrótem.
std::optional<Key> Key::from_string(std::string_view written) {
    Mods with;

    std::string_view rest = written;

    while (stripped(rest, "Ctrl+", with.ctrl) || stripped(rest, "Alt+", with.alt) ||
           stripped(rest, "Shift+", with.shift)) {
    }

    if (rest.empty()) return std::nullopt;

    for (const Named& one : names)
        if (same(rest, one.name)) return Key{one.code, with};

    if (same(rest, "Space")) return Key{U' ', Mods{with.ctrl, with.alt, false}};

    std::size_t at = 0;

    const char32_t letter = utf8::decode(rest, at);

    if (at != rest.size() || letter == 0xFFFD || letter < 0x21 || letter == 0x7F) return std::nullopt;

    return Key{folded(letter), Mods{with.ctrl, with.alt, false}};
}

// Odwrotność `from_string` dla każdego klawisza, który ona oddaje: litera łacińska wielka, jak na
// klawiszu, a Shift przy literze pominięty. Poza łaciną podstawową litera zostaje, jaka przyszła —
// odwrotność składania nie jest jednoznaczna („σ" ma dwie wielkie drogi: „Σ" i „ς"), a zapis
// i tak wraca do tego samego klawisza.
std::string to_string(Key one) {
    if (one.empty()) return {};

    std::string made;

    if (one.mods().ctrl) made += "Ctrl+";
    if (one.mods().alt) made += "Alt+";

    if (const char32_t letter = one.letter(); letter != 0) {
        if (letter == U' ') return made + "Space";

        utf8::encode(letter >= U'a' && letter <= U'z' ? letter - (U'a' - U'A') : letter, made);

        return made;
    }

    if (one.mods().shift) made += "Shift+";

    for (const Named& named : names)
        if (named.code == one.code()) return made + std::string{named.name};

    return made;
}

// **Cała reguła dopasowania skrótu stoi tutaj** — dla `Keys` i dla każdego programu, który porównuje
// skróty sam. Trzy części, każda z osobnym powodem.
//
// **Wielkość liter składamy.** `Key::operator==` zostaje dokładne, bo pole tekstowe stoi na różnicy
// między „T" a „t" — ale skrót zgłoszony jako `U'T'` ma odpowiadać na jedno i drugie.
//
// **Ctrl i Alt porównujemy dokładnie**: zbiór trzymający zwykłe `F5` sam z siebie nie odpowie na
// Ctrl+F5, a trzymający `Alt+H` — wyłącznie na `Alt+H`.
//
// **Shift przy literze przymykamy**, bo jest już zakodowany w samej literze; **przy klawiszu
// kodowanym porównujemy** — Shift+F7 i F7 to w Midnight Commanderze dwa różne klawisze (powtórz
// szukanie i szukaj), a pole paska z `Key{f7}` brało oba i okno otwierało się obok powtórzonego
// szukania.
bool alike(Key one, Key two) {
    const Mods here = one.mods();
    const Mods there = two.mods();

    if (here.ctrl != there.ctrl || here.alt != there.alt) return false;

    if (one.code() != Code::none || two.code() != Code::none)
        return one.code() == two.code() && here.shift == there.shift;

    const char32_t first = one.letter();
    const char32_t second = two.letter();

    return first != 0 && second != 0 && folded(first) == folded(second);
}

}
