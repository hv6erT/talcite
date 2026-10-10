#include "talcite/decoder.hpp"

#include "talcite/encoding.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace talcite {

namespace {

constexpr char esc = '\x1b';

// More bytes than this allowance in an unclosed sequence mean that it is not a sequence but
// garbage. Without this limit broken input would grow in the buffer without end.
constexpr std::size_t most = 64;

// Control strings (OSC `Esc ]`, DCS `Esc P`, APC `Esc _`, PM `Esc ^` and SOS `Esc X`) are terminal
// replies, not keys: the background color, XTGETTCAP, the reply of the kitty image protocol. They
// end with ST (`Esc \`), and OSC also with BEL. They can be long, so they have their own allowance.
constexpr std::size_t stretched = 4096;

// Whether the byte after Esc opens a control string.
bool stringed(char lead) { return lead == ']' || lead == 'P' || lead == '_' || lead == '^' || lead == 'X'; }

// The end of a control string that starts at `from`; `npos` when it has not arrived yet.
std::size_t ended(const std::string& rest, std::size_t from, char lead) {
    for (std::size_t at = from; at < rest.size(); ++at) {
        if (lead == ']' && rest[at] == '\x07') return at + 1;

        if (rest[at] == esc && at + 1 < rest.size() && rest[at + 1] == '\\') return at + 2;
    }

    return std::string::npos;
}

// The numeric parameters of a sequence, split at semicolons; an empty parameter counts as zero.
std::vector<int> numbers(std::string_view body) {
    std::vector<int> made;

    int one = 0;
    bool any = false;

    for (const char sign : body) {
        if (sign >= '0' && sign <= '9') {
            one = one * 10 + (sign - '0');
            any = true;

            continue;
        }

        if (sign == ';') {
            made.push_back(any ? one : 0);

            one = 0;
            any = false;
        }
    }

    if (any) made.push_back(one);

    return made;
}

// The modifier value in a sequence is the mask plus one: after subtracting one, 1 is shift alone,
// 4 is ctrl alone and 5 is shift with ctrl. Zero and one mean no modifiers.
ui::Mods marked(int given) {
    ui::Mods made;

    if (given <= 1) return made;

    const int bits = given - 1;

    made.shift = (bits & 1) != 0;
    made.alt = (bits & 2) != 0;
    made.ctrl = (bits & 4) != 0;

    return made;
}

// A control character is really a letter with ctrl: 0x01 is Ctrl+A. Tab, Enter and Backspace
// have their own codes, so they are taken out before they fall under that rule.
ui::Key steering(unsigned char byte, ui::Mods& mods) {
    switch (byte) {
        case 0x09: return ui::Key{ui::Code::tab};
        case 0x0D:
        case 0x0A: return ui::Key{ui::Code::enter};
        case 0x08:
        case 0x7F: return ui::Key{ui::Code::backspace};
        default: break;
    }

    mods.ctrl = true;

    if (byte == 0x00) return ui::Key{U' '};
    if (byte <= 0x1A) return ui::Key{static_cast<char32_t>(U'a' + byte - 1)};

    return ui::Key{static_cast<char32_t>(byte + 0x40)};
}

// The key of a `Esc [ n ~` sequence, by its number; an empty key for a number that means nothing.
ui::Key tilde(int given) {
    switch (given) {
        case 1:
        case 7: return ui::Key{ui::Code::home};
        case 2: return ui::Key{ui::Code::insert};
        case 3: return ui::Key{ui::Code::remove};
        case 4:
        case 8: return ui::Key{ui::Code::end};
        case 5: return ui::Key{ui::Code::pageup};
        case 6: return ui::Key{ui::Code::pagedown};
        case 11: return ui::Key{ui::Code::f1};
        case 12: return ui::Key{ui::Code::f2};
        case 13: return ui::Key{ui::Code::f3};
        case 14: return ui::Key{ui::Code::f4};
        case 15: return ui::Key{ui::Code::f5};
        case 17: return ui::Key{ui::Code::f6};
        case 18: return ui::Key{ui::Code::f7};
        case 19: return ui::Key{ui::Code::f8};
        case 20: return ui::Key{ui::Code::f9};
        case 21: return ui::Key{ui::Code::f10};
        case 23: return ui::Key{ui::Code::f11};
        case 24: return ui::Key{ui::Code::f12};
        default: return {};
    }
}

// The key of a sequence ending in a letter (arrows, Home, End, F1–F4); an empty key otherwise.
ui::Key lettered(char last) {
    switch (last) {
        case 'A': return ui::Key{ui::Code::up};
        case 'B': return ui::Key{ui::Code::down};
        case 'C': return ui::Key{ui::Code::right};
        case 'D': return ui::Key{ui::Code::left};
        case 'H': return ui::Key{ui::Code::home};
        case 'F': return ui::Key{ui::Code::end};
        case 'P': return ui::Key{ui::Code::f1};
        case 'Q': return ui::Key{ui::Code::f2};
        case 'R': return ui::Key{ui::Code::f3};
        case 'S': return ui::Key{ui::Code::f4};
        default: return {};
    }
}

}

struct Decoder::Held {
    std::string rest;
    std::string pasted;
    bool pasting = false;
};

Decoder::Decoder() : held_(std::make_unique<Held>()) {}

Decoder::~Decoder() = default;

// True when the buffer holds an Esc that may still be a key on its own or the start of a sequence;
// the caller waits a moment and then calls `flush`.
bool Decoder::pending() const {
    return !held_->rest.empty() && held_->rest.front() == esc;
}

// A pending Esc is settled by whoever has the clock; the decoder only says that it holds
// something. When the time is up, the Esc becomes a key and what was left behind it is fed again.
//
// An Esc with the start of a control string that nothing closed is Alt with a letter after all
// (Alt+P, Alt+]): that key goes out, and the rest goes as ordinary characters.
void Decoder::flush(ui::Root& onto) {
    if (!pending()) return;

    if (held_->rest.size() >= 2 && stringed(held_->rest[1])) {
        ui::Mods mods;
        mods.alt = true;

        ui::KeyEvent what{ui::Key{ui::Key{static_cast<char32_t>(held_->rest[1])}, mods}};

        held_->rest.erase(0, 2);

        (void)onto.dispatch(what);

        std::string spare;
        spare.swap(held_->rest);

        fed(spare, onto);

        return;
    }

    held_->rest.erase(0, 1);

    ui::KeyEvent what{ui::Key{ui::Code::escape}};

    (void)onto.dispatch(what);

    std::string spare;
    spare.swap(held_->rest);

    fed(spare, onto);
}

// Turns the bytes from the terminal into events dispatched to `onto`: keys, mouse and wheel
// events, and pastes. An incomplete sequence or character stays in the buffer for the next call;
// terminal replies are swallowed.
void Decoder::fed(std::string_view bytes, ui::Root& onto) {
    Held& held = *held_;

    held.rest += bytes;

    std::size_t at = 0;

    while (at < held.rest.size()) {
        // Bracketed paste: everything up to the end marker is content, not keys.
        if (held.pasting) {
            const std::size_t end = held.rest.find("\x1b[201~", at);

            if (end == std::string::npos) {
                // A tail that may be the start of the end marker has to stay in the buffer.
                // Otherwise a "\e[20" cut off by the read would go into the content and the paste
                // would never close.
                const std::string_view rest{held.rest};
                const std::string_view mark{"\x1b[201~"};

                std::size_t keep = held.rest.size();

                for (std::size_t back = std::min<std::size_t>(mark.size() - 1,
                                                              held.rest.size() - at);
                     back >= 1; --back)
                    if (rest.substr(held.rest.size() - back) == mark.substr(0, back)) {
                        keep = held.rest.size() - back;

                        break;
                    }

                held.pasted.append(held.rest, at, keep - at);
                at = keep;

                break;
            }

            held.pasted.append(held.rest, at, end - at);
            at = end + 6;

            held.pasting = false;

            ui::PasteEvent what{std::move(held.pasted)};
            held.pasted.clear();

            (void)onto.dispatch(what);

            continue;
        }

        const auto byte = static_cast<unsigned char>(held.rest[at]);

        if (byte == 0x1B) {
            if (at + 1 >= held.rest.size()) break;

            const char lead = held.rest[at + 1];

            // Esc after Esc is two presses, not one with a modifier. The second one stays in the
            // buffer and settles itself.
            if (lead == esc) {
                ui::KeyEvent what{ui::Key{ui::Code::escape}};

                (void)onto.dispatch(what);

                ++at;

                continue;
            }

            // Late terminal replies are not keys. The capability probe waits for them only a
            // moment; over SSH or on a slow terminal the reply comes later and arrives here, and
            // without this the background color from OSC 11 would become Alt+] and a string of
            // letters typed into a field.
            if (stringed(lead)) {
                const std::size_t end = ended(held.rest, at + 2, lead);

                if (end != std::string::npos) {
                    at = end;

                    continue;
                }

                if (held.rest.size() - at <= stretched) break;
            }

            if (lead == '[' || lead == 'O') {
                std::size_t end = at + 2;

                while (end < held.rest.size() &&
                       !(held.rest[end] >= 0x40 && held.rest[end] <= 0x7E))
                    ++end;

                if (end >= held.rest.size()) {
                    if (held.rest.size() - at > most) {
                        at = held.rest.size();

                        break;
                    }

                    break;
                }

                const std::string_view body{held.rest.data() + at + 2, end - at - 2};
                const char last = held.rest[end];

                at = end + 1;

                // Mouse in the SGR encoding. The mask carries the button, motion, the wheel and the
                // modifiers at once.
                if (!body.empty() && body.front() == '<') {
                    const std::vector<int> among = numbers(body.substr(1));

                    if (among.size() < 3) continue;

                    const int mask = among[0];
                    const ui::Spot where{among[1] - 1, among[2] - 1};

                    ui::Mods mods;
                    mods.shift = (mask & 4) != 0;
                    mods.alt = (mask & 8) != 0;
                    mods.ctrl = (mask & 16) != 0;

                    if ((mask & 64) != 0) {
                        ui::WheelEvent what{where, (mask & 1) != 0 ? -1 : 1, mods};

                        (void)onto.dispatch(what);

                        continue;
                    }

                    const ui::Mouse button = (mask & 3) == 0   ? ui::Mouse::left
                                              : (mask & 3) == 1 ? ui::Mouse::middle
                                              : (mask & 3) == 2 ? ui::Mouse::right
                                                                : ui::Mouse::none;

                    const ui::Press press = last == 'm'          ? ui::Press::up
                                            : (mask & 32) != 0 ? ui::Press::move
                                                               : ui::Press::down;

                    ui::MouseEvent what{where, press, button, mods};

                    (void)onto.dispatch(what);

                    continue;
                }

                // Replies with a private marker (Primary and Secondary DA, DECRPM, the DECXCPR cursor
                // position report, kitty keyboard flags) are never keys.
                if (!body.empty() && (body.front() == '?' || body.front() == '>' || body.front() == '='))
                    continue;

                const std::vector<int> among = numbers(body);

                // The cursor position report (CPR) is `Esc [ row ; column R`. F3 with a modifier has
                // the same shape, but always with a one in front.
                if (last == 'R' && among.size() >= 2 && among[0] != 1) continue;

                if (last == '~') {
                    if (!among.empty() && among[0] == 200) {
                        held.pasting = true;

                        continue;
                    }

                    if (!among.empty() && among[0] == 201) continue;

                    const ui::Key key = among.empty() ? ui::Key{} : tilde(among[0]);

                    if (key.empty()) continue;

                    ui::KeyEvent what{ui::Key{key, among.size() > 1 ? marked(among[1]) : ui::Mods{}}};

                    (void)onto.dispatch(what);

                    continue;
                }

                const ui::Key key = lettered(last);

                if (key.empty()) continue;

                ui::KeyEvent what{ui::Key{key, among.size() > 1 ? marked(among[1]) : ui::Mods{}}};

                (void)onto.dispatch(what);

                continue;
            }

            // Esc as the Alt prefix. The character after it is taken apart like any other.
            const auto next = static_cast<unsigned char>(lead);

            ui::Mods mods;
            mods.alt = true;

            if (next < 0x80) {
                const ui::Key key = next < 0x20 || next == 0x7F
                                        ? steering(next, mods)
                                        : ui::Key{static_cast<char32_t>(next)};

                ui::KeyEvent what{ui::Key{key, mods}};

                (void)onto.dispatch(what);

                at += 2;

                continue;
            }

            const std::size_t more = utf8::trailing(next);

            if (more == 0) {
                at += 2;

                continue;
            }

            if (at + 2 + more > held.rest.size()) break;

            char32_t code = next & (0x3F >> more);

            for (std::size_t step = 0; step < more; ++step)
                code = (code << 6) |
                       (static_cast<unsigned char>(held.rest[at + 2 + step]) & 0x3F);

            ui::KeyEvent what{ui::Key{code, mods}};

            (void)onto.dispatch(what);

            at += 2 + more;

            continue;
        }

        if (byte < 0x20 || byte == 0x7F) {
            ui::Mods mods;

            const ui::Key key = steering(byte, mods);

            ui::KeyEvent what{ui::Key{key, mods}};

            (void)onto.dispatch(what);

            ++at;

            continue;
        }

        if (byte < 0x80) {
            ui::KeyEvent what{ui::Key{static_cast<char32_t>(byte)}};

            (void)onto.dispatch(what);

            ++at;

            continue;
        }

        const std::size_t more = utf8::trailing(byte);

        if (more == 0) {
            ++at;

            continue;
        }

        // A read can end in the middle of a character. The tail stays in the buffer until the next
        // feed; that is the whole reason the decoder is a state machine.
        if (at + 1 + more > held.rest.size()) break;

        char32_t code = byte & (0x3F >> more);

        for (std::size_t step = 0; step < more; ++step)
            code = (code << 6) | (static_cast<unsigned char>(held.rest[at + 1 + step]) & 0x3F);

        ui::KeyEvent what{ui::Key{code}};

        (void)onto.dispatch(what);

        at += 1 + more;
    }

    held.rest.erase(0, at);
}

}
