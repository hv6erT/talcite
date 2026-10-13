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

// One spelling per key. The written form is read regardless of case, but there are no synonyms:
// Ins, and not also Insert. Two spellings of the same key in the language files would be two
// places where the collision test would have to know that they mean one key.
constexpr std::array<Named, 34> names{{
    {Code::escape, "Esc"},     {Code::enter, "Enter"},   {Code::tab, "Tab"},
    {Code::backspace, "Backspace"}, {Code::insert, "Ins"}, {Code::remove, "Del"},
    {Code::up, "Up"},          {Code::down, "Down"},     {Code::left, "Left"},
    {Code::right, "Right"},    {Code::home, "Home"},     {Code::end, "End"},
    {Code::pageup, "PgUp"},    {Code::pagedown, "PgDn"}, {Code::f1, "F1"},
    {Code::f2, "F2"},          {Code::f3, "F3"},         {Code::f4, "F4"},
    {Code::f5, "F5"},          {Code::f6, "F6"},         {Code::f7, "F7"},
    {Code::f8, "F8"},          {Code::f9, "F9"},         {Code::f10, "F10"},
    {Code::f11, "F11"},        {Code::f12, "F12"},       {Code::f13, "F13"},
    {Code::f14, "F14"},        {Code::f15, "F15"},       {Code::f16, "F16"},
    {Code::f17, "F17"},        {Code::f18, "F18"},       {Code::f19, "F19"},
    {Code::f20, "F20"},
}};

// Whether two ASCII spellings are equal ignoring case.
bool same(std::string_view one, std::string_view two) {
    if (one.size() != two.size()) return false;

    for (std::size_t at = 0; at < one.size(); ++at)
        if (folded(static_cast<unsigned char>(one[at])) != folded(static_cast<unsigned char>(two[at])))
            return false;

    return true;
}

// Strips a modifier prefix from the front of the text and records which modifier it was. The
// order in the written form is free, but each modifier may be given only once: a repeated one
// returns false and is left in place, so the whole key fails to read.
bool stripped(std::string_view& rest, std::string_view prefix, bool& into) {
    if (!same(rest.substr(0, prefix.size()), prefix)) return false;

    if (into) return false;

    into = true;
    rest.remove_prefix(prefix.size());

    return true;
}

}

// Reads a key as a person writes it: the modifiers Ctrl+, Alt+, Shift+ and the key, which is a
// name from the table, Space or a single character. A letter comes out case-folded (`folded`, so
// lowercase in Cyrillic too: "Alt+Л" gives л), because Alt+K and Alt+k are the same key (`alike`
// ignores case), and Shift means nothing next to a letter. Anything that cannot be read in full
// gives nothing: half a shortcut would be a different shortcut.
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

// The inverse of `from_string` for every key it returns: a basic Latin letter is written in
// uppercase, as on the keycap, and Shift next to a letter is left out. Outside basic Latin the
// letter stays as it came, because the inverse of case folding is not unique ("σ" has two ways
// back, "Σ" and "ς"), and the written form reads back to the same key anyway.
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

// Whether two keys count as the same shortcut. The whole matching rule lives here, for `Keys` and
// for every program that compares shortcuts itself. It has three parts, each with its own reason.
//
// Letter case is folded. The equality operator of `Key` stays exact, because a text field depends
// on the difference between "T" and "t", but a shortcut registered as U'T' has to answer to both.
//
// Ctrl and Alt are compared exactly: a set holding plain F5 does not answer to Ctrl+F5 by itself,
// and one holding Alt+H answers only to Alt+H.
//
// Shift is ignored next to a letter, because it is already encoded in the letter itself, but it
// is compared for a coded key: Shift+F7 and F7 are two different keys in Midnight Commander
// (repeat search and search), and a bar field registered for F7 must not also open on Shift+F7.
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
