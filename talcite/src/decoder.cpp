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

// Bajty ponad ten zapas przy niedomkniętej sekwencji znaczą, że to nie jest sekwencja, tylko
// śmieci. Bez tego zepsute wejście rosłoby w buforze bez końca.
constexpr std::size_t most = 64;

// Napisy sterujące — OSC (`Esc ]`), DCS (`Esc P`), APC (`Esc _`), PM (`Esc ^`) i SOS (`Esc X`) —
// to odpowiedzi terminala, nie klawisze: kolor tła, XTGETTCAP, odpowiedź protokołu obrazów kitty.
// Kończy je ST (`Esc \`), a OSC także dzwonek. Bywają długie, więc mają własny zapas.
constexpr std::size_t stretched = 4096;

bool stringed(char lead) { return lead == ']' || lead == 'P' || lead == '_' || lead == '^' || lead == 'X'; }

// Koniec napisu sterującego od `from`; `npos`, gdy jeszcze nie przyszedł.
std::size_t ended(const std::string& rest, std::size_t from, char lead) {
    for (std::size_t at = from; at < rest.size(); ++at) {
        if (lead == ']' && rest[at] == '\x07') return at + 1;

        if (rest[at] == esc && at + 1 < rest.size() && rest[at + 1] == '\\') return at + 2;
    }

    return std::string::npos;
}

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

// Maska modyfikatorów z sekwencji jest pomniejszona o jeden: 1 to sam shift, 4 to sam ctrl,
// 5 to shift z ctrl. Zero i jeden znaczą brak.
ui::Mods marked(int given) {
    ui::Mods made;

    if (given <= 1) return made;

    const int bits = given - 1;

    made.shift = (bits & 1) != 0;
    made.alt = (bits & 2) != 0;
    made.ctrl = (bits & 4) != 0;

    return made;
}

// Znak sterujący to w istocie litera z ctrl — 0x01 jest Ctrl+A. Tab, Enter i Backspace mają
// własne kody, więc trzeba je wyjąć, zanim wpadną w tę regułę.
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

bool Decoder::pending() const {
    return !held_->rest.empty() && held_->rest.front() == esc;
}

// Wiszący Esc rozstrzyga ten, kto ma zegar — dekoder tylko mówi, że coś trzyma. Po upływie
// czasu zamieniamy go w klawisz i puszczamy w obieg to, co za nim zostało.
//
// Esc z początkiem napisu sterującego, po którym nic nie domknęło napisu, to jednak Alt z literą —
// Alt+P, Alt+] — i taki klawisz wychodzi, a reszta idzie jako zwykłe znaki.
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

void Decoder::fed(std::string_view bytes, ui::Root& onto) {
    Held& held = *held_;

    held.rest += bytes;

    std::size_t at = 0;

    while (at < held.rest.size()) {
        // Wklejanie w nawiasach: wszystko do znacznika końca jest treścią, a nie klawiszami.
        if (held.pasting) {
            const std::size_t end = held.rest.find("\x1b[201~", at);

            if (end == std::string::npos) {
                // Ogon, który może być początkiem znacznika końca, musi zostać w buforze.
                // Inaczej pocięte „\e[20" trafiłoby do treści i wklejenie nigdy by się nie
                // zamknęło.
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

            // Esc po Escu to dwa naciśnięcia, a nie jedno z modyfikatorem. Drugi zostaje
            // w buforze i rozstrzygnie się sam.
            if (lead == esc) {
                ui::KeyEvent what{ui::Key{ui::Code::escape}};

                (void)onto.dispatch(what);

                ++at;

                continue;
            }

            // **Spóźnione odpowiedzi terminala nie są klawiszami.** Sonda możliwości czeka na nie
            // chwilę; przez SSH albo na wolnym terminalu odpowiedź przychodziła po czasie i szła
            // tutaj — kolor tła z OSC 11 stawał się Alt+] i ciągiem liter wpisanych w pole.
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

                // Mysz w zapisie SGR. Maska niesie przycisk, ruch, kółko i modyfikatory naraz.
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

                // Odpowiedzi ze znacznikiem prywatnym — Primary i Secondary DA, DECRPM, raport
                // położenia kursora DECXCPR, flagi klawiatury kitty — klawiszami nie są nigdy.
                if (!body.empty() && (body.front() == '?' || body.front() == '>' || body.front() == '='))
                    continue;

                const std::vector<int> among = numbers(body);

                // Raport położenia kursora (CPR): `Esc [ wiersz ; kolumna R`. F3 z modyfikatorem
                // ma ten sam kształt, ale zawsze z jedynką na przedzie.
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

            // Esc jako przedrostek Alt. Znak za nim rozbieramy tak jak każdy inny.
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

        // Odczyt potrafi się skończyć w połowie znaku. Ogon zostaje w buforze do następnego
        // karmienia — to jest cały powód, dla którego dekoder jest maszyną stanową.
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
