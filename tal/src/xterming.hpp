#pragma once

#include <optional>
#include <string>

#include "tal/geometry.hpp"
#include "tal/key.hpp"

// The words of xterm's input encoding, each written once: talcite writes them for a program in its
// own terminal (`Terminal`) and for the records of the Windows console (conhost.cpp), and reads
// them from the terminal it runs in (decoder.cpp).

namespace tal::xterm {

// The modifier bits of a key. A sequence carries them as the mask plus one (`modifier`), so one
// means none.
inline constexpr int key_shift = 1;
inline constexpr int key_alt = 2;
inline constexpr int key_ctrl = 4;

// The modifier parameter of a key: one, plus the bits of the modifiers held.
[[nodiscard]] constexpr int modifier(bool shift, bool alt, bool ctrl) {
    return 1 + (shift ? key_shift : 0) + (alt ? key_alt : 0) + (ctrl ? key_ctrl : 0);
}

// The modifiers a modifier parameter carries. Zero and one mean none.
[[nodiscard]] constexpr Mods modifiers(int given) {
    Mods made;

    if (given <= 1) return made;

    const int bits = given - 1;

    made.shift = (bits & key_shift) != 0;
    made.alt = (bits & key_alt) != 0;
    made.ctrl = (bits & key_ctrl) != 0;

    return made;
}

// A key xterm writes with a final letter: `CSI X` without modifiers, or `SS3 X` in application
// mode, and `CSI 1 ; m X` with them.
struct Lettered {
    Code code;
    char letter;
};

// The keys written with a final letter, the same letter after CSI and after SS3.
inline constexpr Lettered letters[] = {{Code::up, 'A'},    {Code::down, 'B'}, {Code::right, 'C'},
                                       {Code::left, 'D'},  {Code::home, 'H'}, {Code::end, 'F'},
                                       {Code::f1, 'P'},    {Code::f2, 'Q'},   {Code::f3, 'R'},
                                       {Code::f4, 'S'}};

// A key xterm writes as `CSI n ~`, with `; m` for modifiers.
struct Tilded {
    Code code;
    int number;
};

// The keys written with a tilde. The gaps in the numbers of the function keys (16, 22) come from
// the VT220, which xterm follows.
inline constexpr Tilded tildes[] = {{Code::insert, 2}, {Code::remove, 3}, {Code::pageup, 5},
                                    {Code::pagedown, 6}, {Code::f5, 15},   {Code::f6, 17},
                                    {Code::f7, 18},      {Code::f8, 19},   {Code::f9, 20},
                                    {Code::f10, 21},     {Code::f11, 23},  {Code::f12, 24}};

// The key of a final letter, or nothing for a letter no key is written with.
[[nodiscard]] constexpr std::optional<Code> lettered(char letter) {
    for (const Lettered& one : letters)
        if (one.letter == letter) return one.code;

    return std::nullopt;
}

// The key of a tilde number, or nothing for a number no key is written with.
[[nodiscard]] constexpr std::optional<Code> tilded(int number) {
    for (const Tilded& one : tildes)
        if (one.number == number) return one.code;

    return std::nullopt;
}

// The bytes xterm writes for a named key: the arrows, Home and End, F1–F12, Insert, Delete, the
// page keys, Enter, Tab, Backspace and Escape. The arrows, Home and End go after SS3 without
// modifiers in application mode (`ss3`); F1–F4 always do. Alt puts an escape before a key of one
// byte; Shift with Tab is `CSI Z`, and Ctrl with Backspace is BS instead of DEL. Any other key
// gives nothing.
[[nodiscard]] inline std::string named(Code code, int modifier, bool ss3) {
    const Mods mods = modifiers(modifier);
    const std::string prefix = mods.alt ? "\x1b" : "";

    switch (code) {
        case Code::enter: return prefix + "\r";
        case Code::tab: return mods.shift ? std::string{"\x1b[Z"} : prefix + "\t";
        case Code::backspace: return prefix + (mods.ctrl ? "\x08" : "\x7f");
        case Code::escape: return prefix + "\x1b";
        default: break;
    }

    for (const Lettered& one : letters) {
        if (one.code != code) continue;

        if (modifier > 1) return "\x1b[1;" + std::to_string(modifier) + one.letter;

        const bool function = code == Code::f1 || code == Code::f2 || code == Code::f3 || code == Code::f4;

        return std::string{ss3 || function ? "\x1bO" : "\x1b["} + one.letter;
    }

    for (const Tilded& one : tildes)
        if (one.code == code)
            return "\x1b[" + std::to_string(one.number) + (modifier > 1 ? ";" + std::to_string(modifier) : std::string{}) + "~";

    return {};
}

// The bits of a mouse report's button number. The lowest two say the button (`mouse_buttons`):
// left 0, middle 1, right 2, and 3 for none or for a release in the encodings that do not say
// which. With the wheel bit they say the direction: up, down, left, right; with the extra bit the
// buttons back and forward.
inline constexpr int mouse_buttons = 3;
inline constexpr int mouse_left = 0;
inline constexpr int mouse_middle = 1;
inline constexpr int mouse_right = 2;
inline constexpr int mouse_none = 3;
inline constexpr int mouse_shift = 4;
inline constexpr int mouse_alt = 8;
inline constexpr int mouse_ctrl = 16;
inline constexpr int mouse_motion = 32;
inline constexpr int mouse_wheel = 64;
inline constexpr int mouse_extra = 128;

// The wheel's buttons: up and down, then left and right.
inline constexpr int wheel_up = mouse_wheel;
inline constexpr int wheel_down = mouse_wheel + 1;
inline constexpr int wheel_left = mouse_wheel + 2;
inline constexpr int wheel_right = mouse_wheel + 3;

// The modifier bits of a mouse report.
[[nodiscard]] constexpr int mouse_mods(bool shift, bool alt, bool ctrl) {
    return (shift ? mouse_shift : 0) | (alt ? mouse_alt : 0) | (ctrl ? mouse_ctrl : 0);
}

// A mouse report in the SGR encoding (1006): `CSI < b ; x ; y M` for a press, a motion or a wheel
// step and `m` for a release, with the cell `at` counted from one.
[[nodiscard]] inline std::string sgr(int button, Spot at, bool released) {
    return "\x1b[<" + std::to_string(button) + ";" + std::to_string(at.x + 1) + ";" + std::to_string(at.y + 1) +
           (released ? "m" : "M");
}

}
