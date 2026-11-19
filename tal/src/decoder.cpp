#include "decoder.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "vt.hpp"
#include "xterming.hpp"

// What the bytes from the terminal mean: keys, the mouse, the wheel, pastes and the terminal's
// focus. The syntax is read by `vt::Reader`; here each token gets its meaning, or none when it is
// a terminal's answer or something no key sends. The decoder knows neither the tree nor the clock:
// it gets bytes with the moment they came and gives values, and `dispatched` turns a value into an
// event for a root. `Term` can thus take the current root for every value, and a key in the middle
// of a read that switches the screen sends the rest to the new one.
//
// Keys. A letter is a letter; Esc before it is Alt. A control character is a letter with Ctrl
// (0x01 is Ctrl+A), except Tab, Enter and Backspace, which have their own codes. 0x7F is Backspace
// and 0x08 is Ctrl+Backspace, as xterm, VTE and Windows Terminal send them. The keys with
// sequences come in the forms terminals use: `CSI` with a letter (arrows, Home, End, F1–F4) and
// with a number and a tilde (Insert, Delete, PgUp, PgDn, F1–F20), `SS3` with a letter, the Linux
// console's F1–F5 (`CSI [ A`–`E`), rxvt's modified tilde keys and arrows, Shift+Tab (`CSI Z`), and
// the keys of the kitty keyboard protocol (`CSI code ; mods u`) and of xterm's modifyOtherKeys
// (`CSI 27 ; mods ; code ~`). Nobody turns the last two on yet, but a terminal that has them on
// by default, or a program around us that turned them on, gets its keys understood. The modifiers
// in a sequence are a mask plus one: shift 1, alt 2, ctrl 4.
//
// The mouse comes in three encodings with one mask: SGR (`CSI < b ; x ; y M` or `m`), urxvt
// (`CSI b ; x ; y M`, each number plus 32) and X10 (`CSI M` and three bytes plus 32). The mask
// carries the button in its lowest two bits, shift 4, alt 8, ctrl 16, motion 32, the wheel 64 and
// the extra buttons 128. The wheel's buttons 64 and 65 are up and down, 66 and 67 left and right;
// a step towards the start (up, left) is positive. Buttons 128 and 129 are back and forward; 130
// and 131 have no agreed meaning and are left out. SGR names the released button; urxvt and X10
// say only "released" (3), so the decoder remembers which button went down. X10 cannot encode a
// cell past 223; a report with a coordinate it could not give is dropped, not guessed.
//
// Answers that come late (after the capability probe, over SSH) are not keys: CSI with a private
// marker (DA, DECRPM, the kitty flags), a cursor position report and control strings are
// swallowed. A position report has the shape of F3 with a modifier, but F3 always has a one in
// front.
namespace tal {

namespace {

// A control character as a key. Tab, Enter and Backspace keep their codes; 0x08 is Backspace with
// Ctrl; the rest are letters with Ctrl: 0x01 is Ctrl+A, 0x00 Ctrl+Space, 0x1C–0x1F Ctrl+\ ] ^ _.
Key steering(unsigned char byte, bool alt) {
    Mods mods;
    mods.alt = alt;

    switch (byte) {
        case 0x09: return Key{Code::tab, mods};
        case 0x0D:
        case 0x0A: return Key{Code::enter, mods};
        case 0x7F: return Key{Code::backspace, mods};
        case 0x08: mods.ctrl = true; return Key{Code::backspace, mods};
        default: break;
    }

    mods.ctrl = true;

    if (byte == 0x00) return Key{U' ', mods};
    if (byte <= 0x1A) return Key{static_cast<char32_t>(U'a' + byte - 1), mods};

    return Key{static_cast<char32_t>(byte + 0x40), mods};
}

// The key of `CSI n ~` by its number; nothing for a number that means no key. Besides the numbers
// xterm writes (`xterm::tilded`) come those of other terminals: 1 and 7 for Home, 4 and 8 for End,
// 11–14 for F1–F4 (rxvt), and 25–34 for F13–F20 as the Linux console and VT220 number them.
std::optional<Code> tilde(int given) {
    switch (given) {
        case 1:
        case 7: return Code::home;
        case 4:
        case 8: return Code::end;
        case 11: return Code::f1;
        case 12: return Code::f2;
        case 13: return Code::f3;
        case 14: return Code::f4;
        case 25: return Code::f13;
        case 26: return Code::f14;
        case 28: return Code::f15;
        case 29: return Code::f16;
        case 31: return Code::f17;
        case 32: return Code::f18;
        case 33: return Code::f19;
        case 34: return Code::f20;
        default: return xterm::tilded(given);
    }
}

// rxvt's arrows with a lowercase letter: Shift after CSI, Ctrl after SS3.
std::optional<Code> slanted(char last) {
    switch (last) {
        case 'a': return Code::up;
        case 'b': return Code::down;
        case 'c': return Code::right;
        case 'd': return Code::left;
        default: return std::nullopt;
    }
}

// A key by its Unicode number, as the kitty protocol and modifyOtherKeys give it: the control keys
// by their codes, the keypad and F13–F20 from the private-use numbers kitty gives them, and
// anything else as a letter. A private-use number without a meaning here (the modifier keys
// themselves, media keys) gives nothing.
std::optional<Key> numbered(int code, Mods mods) {
    switch (code) {
        case 9: return Key{Code::tab, mods};
        case 13: return Key{Code::enter, mods};
        case 27: return Key{Code::escape, mods};
        case 8:
        case 127: return Key{Code::backspace, mods};
        default: break;
    }

    if (code >= 57376 && code <= 57383)
        return Key{static_cast<Code>(static_cast<int>(Code::f13) + code - 57376), mods};

    if (code >= 57399 && code <= 57408) return Key{static_cast<char32_t>(U'0' + code - 57399), mods};

    switch (code) {
        case 57409: return Key{U'.', mods};
        case 57410: return Key{U'/', mods};
        case 57411: return Key{U'*', mods};
        case 57412: return Key{U'-', mods};
        case 57413: return Key{U'+', mods};
        case 57414: return Key{Code::enter, mods};
        case 57415: return Key{U'=', mods};
        case 57416: return Key{U',', mods};
        case 57417: return Key{Code::left, mods};
        case 57418: return Key{Code::right, mods};
        case 57419: return Key{Code::up, mods};
        case 57420: return Key{Code::down, mods};
        case 57421: return Key{Code::pageup, mods};
        case 57422: return Key{Code::pagedown, mods};
        case 57423: return Key{Code::home, mods};
        case 57424: return Key{Code::end, mods};
        case 57425: return Key{Code::insert, mods};
        case 57426: return Key{Code::remove, mods};
        default: break;
    }

    if (code < 0x20 || (code >= 0x7F && code < 0xA0) || (code >= 0xE000 && code <= 0xF8FF) ||
        (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF)
        return std::nullopt;

    return Key{static_cast<char32_t>(code), mods};
}

// The keypad in application mode after SS3: Enter, the operators and the digits.
std::optional<Key> keypad(char last) {
    if (last == 'M') return Key{Code::enter};
    if (last == 'X') return Key{U'='};

    static constexpr std::u32string_view signs = U"*+,-./0123456789";

    if (last >= 'j' && last <= 'y') return Key{signs[static_cast<std::size_t>(last - 'j')]};

    return std::nullopt;
}

}

struct Decoder::Held {
    vt::Reader reader;

    // The button that went down last, for the encodings whose release does not name it.
    MouseButton pressed = MouseButton::none;

    std::vector<Heard> heard(const std::vector<vt::Token>& tokens);

    std::optional<Heard> heard(const vt::Csi& one);

    std::optional<Heard> heard(const vt::Ss3& one) const;

    std::optional<Heard> pointed(int mask, int x, int y, std::optional<bool> released);
};

// The meanings of the tokens in order; a token without one adds nothing.
std::vector<Heard> Decoder::Held::heard(const std::vector<vt::Token>& tokens) {
    std::vector<Heard> made;

    for (const vt::Token& one : tokens) {
        std::optional<Heard> told;

        if (const auto* plain = std::get_if<vt::Plain>(&one.what)) {
            Mods mods;
            mods.alt = plain->alt;

            told = Struck{Key{plain->letter, mods}};
        } else if (const auto* steered = std::get_if<vt::Steered>(&one.what)) {
            told = Struck{steering(steered->byte, steered->alt)};
        } else if (std::holds_alternative<vt::Lone>(one.what)) {
            told = Struck{Key{Code::escape}};
        } else if (const auto* csi = std::get_if<vt::Csi>(&one.what)) {
            told = heard(*csi);
        } else if (const auto* ss3 = std::get_if<vt::Ss3>(&one.what)) {
            told = heard(*ss3);
        } else if (const auto* tracked = std::get_if<vt::Tracked>(&one.what)) {
            told = pointed(tracked->mask, tracked->x, tracked->y, std::nullopt);
        } else if (const auto* glued = std::get_if<vt::Glued>(&one.what)) {
            told = Pasted{glued->text, glued->whole};
        }

        if (told.has_value()) made.push_back(std::move(*told));
    }

    return made;
}

// A mouse report from its mask and 1-based cell. `released` is whether SGR said the button went
// up; for urxvt and X10 it is empty, and "released" is the button number 3.
std::optional<Heard> Decoder::Held::pointed(int mask, int x, int y, std::optional<bool> released) {
    if (x < 1 || y < 1) return std::nullopt;

    const Spot where{x - 1, y - 1};

    Mods mods;
    mods.shift = (mask & xterm::mouse_shift) != 0;
    mods.alt = (mask & xterm::mouse_alt) != 0;
    mods.ctrl = (mask & xterm::mouse_ctrl) != 0;

    const int low = mask & xterm::mouse_buttons;
    const bool moving = (mask & xterm::mouse_motion) != 0;
    const bool wheeled = (mask & xterm::mouse_wheel) != 0;
    const bool extra = (mask & xterm::mouse_extra) != 0;

    if (wheeled && extra) return std::nullopt;

    if (wheeled) {
        const Axis axis = low < 2 ? Axis::vertical : Axis::horizontal;

        return Wheeled{where, low % 2 == 0 ? 1 : -1, axis, mods};
    }

    MouseButton button = MouseButton::none;

    if (extra) {
        if (low > 1) return std::nullopt;

        button = low == 0 ? MouseButton::back : MouseButton::forward;
    } else if (low < 3) {
        button = low == 0 ? MouseButton::left : low == 1 ? MouseButton::middle : MouseButton::right;
    }

    if (moving) return Moused{where, Press::move, button, mods};

    if (released.has_value()) {
        if (*released) {
            pressed = MouseButton::none;

            return Moused{where, Press::up, button, mods};
        }

        pressed = button;

        return Moused{where, Press::down, button, mods};
    }

    if (button == MouseButton::none) {
        const MouseButton was = pressed;

        pressed = MouseButton::none;

        return Moused{where, Press::up, was, mods};
    }

    pressed = button;

    return Moused{where, Press::down, button, mods};
}

// The meaning of a control sequence.
std::optional<Heard> Decoder::Held::heard(const vt::Csi& one) {
    if (one.lead == '<') {
        if ((one.last != 'M' && one.last != 'm') || one.params.size() < 3) return std::nullopt;

        return pointed(one.at(0), one.at(1), one.at(2), one.last == 'm');
    }

    if (one.lead == '[') {
        if (one.last >= 'A' && one.last <= 'E')
            return Struck{Key{static_cast<Code>(static_cast<int>(Code::f1) + (one.last - 'A'))}};

        return std::nullopt;
    }

    // Answers with a private marker (DA, DECRPM, the kitty flags) and sequences with intermediate
    // bytes are no keys. rxvt's `CSI n $` would be one, but `vt::Reader` gives it `$` as its final
    // byte.
    if (one.lead != 0 || !one.between.empty()) return std::nullopt;

    const Mods mods = one.params.size() > 1 ? xterm::modifiers(one.at(1)) : Mods{};

    switch (one.last) {
        case 'M':
            if (one.params.size() < 3) return std::nullopt;

            return pointed(one.at(0) - 32, one.at(1), one.at(2), std::nullopt);

        case 'I':
            if (!one.params.empty()) return std::nullopt;

            return Windowed{true};

        case 'O':
            if (!one.params.empty()) return std::nullopt;

            return Windowed{false};

        case 'Z': {
            Mods shifted = mods;
            shifted.shift = true;

            return Struck{Key{Code::tab, shifted}};
        }

        case 'u': {
            // The kitty keyboard protocol: the key's number, then the modifiers with the kind of
            // event after a colon (1 press, 2 repeat, 3 release). Only presses and repeats are
            // keys.
            if (one.params.empty() || one.part(1, 1, 1) == 3) return std::nullopt;

            const std::optional<Key> key = numbered(one.at(0), xterm::modifiers(one.at(1)));

            if (!key.has_value()) return std::nullopt;

            return Struck{*key};
        }

        case '~': {
            if (one.params.empty()) return std::nullopt;

            if (one.at(0) == 27) {
                if (one.params.size() < 3) return std::nullopt;

                const std::optional<Key> key = numbered(one.at(2), xterm::modifiers(one.at(1)));

                if (!key.has_value()) return std::nullopt;

                return Struck{*key};
            }

            const std::optional<Code> code = tilde(one.at(0));

            if (!code.has_value()) return std::nullopt;

            return Struck{Key{*code, mods}};
        }

        case '$':
        case '^':
        case '@': {
            // rxvt marks the modifiers of a tilde key by the final byte instead of a parameter.
            if (one.params.size() != 1) return std::nullopt;

            const std::optional<Code> code = tilde(one.at(0));

            if (!code.has_value()) return std::nullopt;

            Mods own;
            own.shift = one.last != '^';
            own.ctrl = one.last != '$';

            return Struck{Key{*code, own}};
        }

        case 'R':
            // A cursor position report, unless it is F3 with a modifier, which has a one in front.
            if (one.params.size() >= 2 && one.at(0) != 1) return std::nullopt;

            return Struck{Key{Code::f3, mods}};

        default: break;
    }

    if (const std::optional<Code> code = xterm::lettered(one.last)) return Struck{Key{*code, mods}};

    if (const std::optional<Code> code = slanted(one.last)) {
        Mods shifted = mods;
        shifted.shift = true;

        return Struck{Key{*code, shifted}};
    }

    return std::nullopt;
}

// The meaning of a single shift: the keys with a letter, rxvt's Ctrl with an arrow and the keypad
// in application mode. A number before the letter is a modifier, as old xterms send it.
std::optional<Heard> Decoder::Held::heard(const vt::Ss3& one) const {
    const Mods mods = xterm::modifiers(one.number);

    if (const std::optional<Code> code = xterm::lettered(one.last)) return Struck{Key{*code, mods}};

    if (const std::optional<Code> code = slanted(one.last)) {
        Mods held = mods;
        held.ctrl = true;

        return Struck{Key{*code, held}};
    }

    if (const std::optional<Key> key = keypad(one.last)) return Struck{Key{*key, mods}};

    return std::nullopt;
}

Decoder::Decoder() : held_(std::make_unique<Held>()) {}

Decoder::~Decoder() = default;

// The meanings of the bytes that came at `now`. What is unfinished waits for more bytes or for
// `lapsed`.
std::vector<Heard> Decoder::fed(std::string_view bytes, std::chrono::steady_clock::time_point now) {
    std::vector<vt::Token> tokens;

    held_->reader.fed(bytes, now, tokens);

    return held_->heard(tokens);
}

// When the decoder wants to be asked again without new bytes: a lone Esc after 50 ms, a paste
// without its end after two seconds; nothing when it holds nothing that time settles.
std::optional<std::chrono::steady_clock::time_point> Decoder::due() const { return held_->reader.due(); }

// What time has settled by `now`: Esc alone, Alt with the second byte of an unfinished escape and
// the bytes after it, a paste closed by silence. Before `due()` it gives nothing.
std::vector<Heard> Decoder::lapsed(std::chrono::steady_clock::time_point now) {
    std::vector<vt::Token> tokens;

    held_->reader.lapsed(now, tokens);

    return held_->heard(tokens);
}

// Delivers a value to the document: a key, the mouse, the wheel (along its axis), a paste or the
// focus of the terminal's window, the last of the values a decoder gives.
void dispatched(const Heard& one, Document& onto) {
    static_assert(std::variant_size_v<Heard> == 5, "every value a decoder gives is delivered here");

    if (const auto* struck = std::get_if<Struck>(&one)) {
        onto.key(struck->key);
    } else if (const auto* moused = std::get_if<Moused>(&one)) {
        onto.mouse(moused->press, moused->button, moused->where, moused->mods);
    } else if (const auto* wheeled = std::get_if<Wheeled>(&one)) {
        onto.mouse(Press::wheel, MouseButton::none, wheeled->where, wheeled->mods, wheeled->steps, wheeled->axis);
    } else if (const auto* pasted = std::get_if<Pasted>(&one)) {
        onto.paste(pasted->text);
    } else {
        onto.window(std::get<Windowed>(one).in);
    }
}

}
