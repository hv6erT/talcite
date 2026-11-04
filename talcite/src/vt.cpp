#include "vt.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

#include "talcite/encoding.hpp"

// Reading the bytes a terminal sends: the syntax of ECMA-48 and of the few forms terminals add to
// it, without meaning. A token is a letter, a control character, a lone Esc, a control sequence
// (CSI), a single shift (SS3), a control string (OSC, DCS, APC, PM, SOS), a mouse report in the
// old X10 form or a bracketed paste. What a token means (a key, a click, a terminal's answer) is
// decided by whoever reads the tokens: the decoder and the capability probe read the same syntax
// and must not read it two ways.
//
// A CSI carries its private marker (`?`, `>`, `=`, `<`), its parameters split at semicolons, each
// with up to four parts split at colons (the kitty keyboard protocol and SGR colors use them), its
// intermediate bytes and its final byte. A parameter that was not written stays empty (-1), so
// "CSI ; 5 H" and "CSI 0 ; 5 H" can be told apart; `Csi::at` gives the fallback for an empty one.
// Numbers saturate at 65535: no sequence means anything larger, and a long run of digits from a
// broken or hostile terminal must not overflow. At most sixteen parameters are kept.
//
// Limits: a CSI longer than 64 bytes and a control string longer than 64 KiB are garbage. They
// are read to their end and dropped whole, so their tail does not turn into keys. A paste keeps at
// most 16 MiB; anything past that is read to the end marker and dropped, and the paste says it is
// not whole.
//
// Time. A terminal sends a key's sequence in one write, so an Esc with nothing after it for 50 ms
// is the Esc key, and an unfinished sequence after 50 ms is Alt with its second byte: `Esc [` is
// Alt+[, `Esc O` is Alt+O, `Esc P` is Alt+P, and whatever followed goes on as ordinary input. A
// paste whose end marker has not come for two seconds is closed with what came (a terminal that
// died, a broken SSH connection), instead of holding the program in a paste for good. The time
// counts from the last bytes fed, and it is given by the caller, so trials need no clock.
//
// The forms outside ECMA-48 that are read here because they are syntax, not meaning:
//
// - X10 mouse: `CSI M` and three raw bytes (button, column, row, each plus 32). The three bytes are
//   not text, so only the reader can know where the report ends.
// - The Linux console's F1–F5: `CSI [ A` to `CSI [ E`. The second `[` is a final byte by the
//   standard, and one more byte follows it. It comes out as a CSI with the lead `[`.
// - rxvt's Shift with the tilde keys, `CSI 2 $`: `$` is an intermediate byte, so the sequence has
//   no final byte at all. It is recognized when the time is up: digits and a `$` that nothing
//   followed are a CSI with the final `$`.
//
// A control string ends with ST (`Esc \`) or with BEL. The standard gives BEL to OSC only, but
// terminals end their answers either way, and a BEL never stands inside an answer's text.
//
// Inside a CSI, CAN and SUB cancel it, Esc starts a new escape and drops the unfinished one, DEL
// is ignored and any other control character takes effect at once, as in a terminal's own parser.
// A byte from outside ASCII cancels it. Text is UTF-8: an overlong form, a surrogate, a value
// past U+10FFFF and a broken sequence are dropped, and a byte that does not continue a character
// starts anew.
//
// `raw` keeps each token's bytes, for the probe, which hands what it does not understand to the
// decoder as bytes; the decoder does not ask for them.
namespace talcite::vt {

namespace {

constexpr unsigned char esc = 0x1B;
constexpr unsigned char bel = 0x07;
constexpr unsigned char can = 0x18;
constexpr unsigned char sub = 0x1A;
constexpr unsigned char del = 0x7F;

constexpr std::size_t longest_sequence = 64;
constexpr std::size_t longest_string = std::size_t{64} * 1024;
constexpr std::size_t longest_paste = std::size_t{16} * 1024 * 1024;
constexpr std::size_t longest_shift = 8;

constexpr std::chrono::milliseconds hesitating{50};
constexpr std::chrono::milliseconds silence{2000};

constexpr std::string_view paste_start = "\x1b[200~";
constexpr std::string_view paste_end = "\x1b[201~";

// Whether the byte after Esc opens a control string: OSC, DCS, APC, PM or SOS.
bool stringed(unsigned char byte) { return byte == ']' || byte == 'P' || byte == '_' || byte == '^' || byte == 'X'; }

// Whether the byte ends a control sequence.
bool final_byte(unsigned char byte) { return byte >= 0x40 && byte <= 0x7E; }

// Whether the byte may start a UTF-8 character, by the one rule (`utf8::span`): a lead that the
// rest of the character has yet to follow.
bool leading(unsigned char byte) { return utf8::span(std::string(1, static_cast<char>(byte))).kind == utf8::Span::Kind::partial; }

// A control sequence from the bytes between `CSI` and the final byte.
Csi parsed(std::string_view body, char last) {
    Csi made;
    made.last = last;

    std::size_t at = 0;

    if (!body.empty() && body.front() >= 0x3C && body.front() <= 0x3F) {
        made.lead = body.front();
        at = 1;
    }

    Param one;
    std::size_t part = 0;
    bool written = false;

    const auto kept = [&made, &one, &part]() {
        if (made.params.size() < most_params) made.params.push_back(one);

        one = Param{};
        part = 0;
    };

    for (; at < body.size(); ++at) {
        const char sign = body[at];

        if (sign >= '0' && sign <= '9') {
            written = true;

            if (part >= most_parts) continue;

            int& value = one.parts[part];

            value = std::min((value < 0 ? 0 : value) * 10 + (sign - '0'), most_value);

            continue;
        }

        if (sign == ':') {
            written = true;
            ++part;

            continue;
        }

        if (sign == ';') {
            written = true;

            kept();

            continue;
        }

        if (sign >= 0x20 && sign <= 0x2F) made.between += sign;
    }

    if (written) kept();

    return made;
}

}

// A reader; with `raw` each token keeps the bytes it was read from.
Reader::Reader(bool raw) : raw_(raw) {}

// Reads the bytes that came at `now` and appends the finished tokens. An unfinished token waits
// for the next feed or for `lapsed`.
void Reader::fed(std::string_view bytes, Clock::Moment now, std::vector<Token>& into) {
    if (bytes.empty()) return;

    for (const char one : bytes) stepped(static_cast<unsigned char>(one), now, into);

    since_ = now;
}

// When the reader wants to settle what it holds: 50 ms after the last bytes for an unfinished
// escape, two seconds for an unfinished paste and for an overlong string being read to its end,
// and never for plain text or a character cut in its middle, which the next byte settles. An
// overlong string that stops coming is given up after the longer wait, so that keys typed after a
// broken terminal's garbage are not swallowed for good, and not sooner, so that the rest of a slow
// one does not come out as keys.
std::optional<Clock::Moment> Reader::due() const {
    switch (state_) {
        case State::ground:
        case State::lettered: return std::nullopt;
        case State::glued:
        case State::told_ignored: return since_ + silence;
        default: return since_ + hesitating;
    }
}

// Settles what has waited long enough by `now`: a lone Esc, an unfinished escape as Alt with its
// second byte and the rest as ordinary input, rxvt's `CSI n $`, and a paste without its end. What
// is not due yet stays.
void Reader::lapsed(Clock::Moment now, std::vector<Token>& into) {
    const std::optional<Clock::Moment> when = due();

    if (!when.has_value() || now < *when) return;

    const State was = state_;
    std::string held = std::move(pending_);

    const auto reset = [this]() {
        state_ = State::ground;
        pending_.clear();
        alt_ = false;
        glued_.clear();
        tail_.clear();
        overflowed_ = false;
    };

    if (was == State::glued) {
        Glued made{std::move(glued_), false};

        made.text += tail_;

        std::string raw = raw_ ? std::string{paste_start} + made.text : std::string{};

        reset();
        sent(Token{std::move(made), std::move(raw)}, into);

        return;
    }

    reset();

    if (was == State::escaped) {
        sent(Token{Lone{}, std::move(held)}, into);

        return;
    }

    if (was == State::csi_ignored || was == State::told_ignored || held.size() < 2) return;

    if (was == State::csi && held.size() > 3 && held.back() == '$' &&
        std::all_of(held.begin() + 2, held.end() - 1, [](char one) { return one >= '0' && one <= '9'; })) {
        sent(Token{parsed(std::string_view{held}.substr(2, held.size() - 3), '$'), held}, into);

        return;
    }

    sent(Token{Plain{static_cast<unsigned char>(held[1]), true}, held.substr(0, 2)}, into);

    for (std::size_t at = 2; at < held.size(); ++at) stepped(static_cast<unsigned char>(held[at]), now, into);

    since_ = now;
}

// Gives back the bytes of whatever is unfinished and starts afresh: the probe hands them to the
// decoder when it stops listening.
std::string Reader::drained() {
    std::string made = state_ == State::glued ? std::string{paste_start} + glued_ + tail_ : std::move(pending_);

    state_ = State::ground;
    pending_.clear();
    alt_ = false;
    glued_.clear();
    tail_.clear();
    overflowed_ = false;

    return made;
}

// Appends a finished token, without its bytes unless they were asked for.
void Reader::sent(Token made, std::vector<Token>& into) {
    if (!raw_) made.raw.clear();

    into.push_back(std::move(made));
}

// A byte outside any sequence: Esc starts one, a control character and ASCII are tokens at once,
// a UTF-8 lead starts a character (`leading`), and anything else is dropped. `alt` marks a
// character that came right after Esc.
void Reader::grounded(unsigned char byte, bool alt, std::vector<Token>& into) {
    std::string raw = alt ? std::string{static_cast<char>(esc)} : std::string{};

    raw += static_cast<char>(byte);

    if (byte == esc && !alt) {
        state_ = State::escaped;
        pending_ = std::move(raw);

        return;
    }

    if (byte < 0x20 || byte == del) {
        state_ = State::ground;
        pending_.clear();

        sent(Token{Steered{byte, alt}, std::move(raw)}, into);

        return;
    }

    if (byte < 0x80) {
        state_ = State::ground;
        pending_.clear();

        sent(Token{Plain{byte, alt}, std::move(raw)}, into);

        return;
    }

    if (!leading(byte)) {
        state_ = State::ground;
        pending_.clear();

        return;
    }

    state_ = State::lettered;
    pending_ = std::move(raw);
    alt_ = alt;
}

// The final byte of a CSI: an X10 mouse report goes on for three bytes, the start of a paste
// switches to the paste, anything else is a finished sequence.
void Reader::finished(unsigned char last, std::vector<Token>& into) {
    const std::string_view body = std::string_view{pending_}.substr(2);

    if (body.empty() && last == 'M') {
        state_ = State::tracked;
        pending_ += static_cast<char>(last);

        return;
    }

    Csi made = parsed(body, static_cast<char>(last));

    pending_ += static_cast<char>(last);

    if (made.lead == 0 && made.between.empty() && last == '~' && made.params.size() == 1 && made.at(0) == 200) {
        state_ = State::glued;
        pending_.clear();
        glued_.clear();
        tail_.clear();
        overflowed_ = false;

        return;
    }

    std::string raw = std::move(pending_);

    state_ = State::ground;
    pending_.clear();

    sent(Token{std::move(made), std::move(raw)}, into);
}

// A byte of a paste. The end marker may come cut between feeds, so its possible beginning waits in
// `tail_`; only bytes that cannot start the marker go into the text. The marker starts with Esc and
// holds no other Esc, so dropping the first byte of a tail that stopped matching is enough.
void Reader::glued(unsigned char byte, std::vector<Token>& into) {
    tail_ += static_cast<char>(byte);

    while (!tail_.empty() && paste_end.substr(0, tail_.size()) != tail_) {
        if (glued_.size() < longest_paste) glued_ += tail_.front();
        else overflowed_ = true;

        tail_.erase(0, 1);
    }

    if (tail_ != paste_end) return;

    Glued made{std::move(glued_), !overflowed_};

    std::string raw = raw_ ? std::string{paste_start} + made.text + std::string{paste_end} : std::string{};

    state_ = State::ground;
    glued_.clear();
    tail_.clear();
    overflowed_ = false;

    sent(Token{std::move(made), std::move(raw)}, into);
}

// One byte, in the state the reader is in.
void Reader::stepped(unsigned char byte, Clock::Moment now, std::vector<Token>& into) {
    switch (state_) {
        case State::ground: grounded(byte, false, into); return;

        // The bytes of a character, by the one rule (`utf8::span`): what the rule does not let
        // through is dropped, since no key types it, and the byte that broke it off is read anew.
        case State::lettered: {
            pending_ += static_cast<char>(byte);

            const std::size_t from = alt_ ? 1 : 0;
            const std::string_view letter = std::string_view{pending_}.substr(from);
            const utf8::Span got = utf8::span(letter);

            if (got.kind == utf8::Span::Kind::partial) return;

            const bool alt = alt_;
            const bool broken = got.kind == utf8::Span::Kind::bad && got.size < letter.size();

            std::size_t at = 0;
            const char32_t made = got.kind == utf8::Span::Kind::whole ? utf8::decode(letter, at) : 0;

            std::string raw = std::move(pending_);

            state_ = State::ground;
            pending_.clear();
            alt_ = false;

            if (got.kind == utf8::Span::Kind::whole) sent(Token{Plain{made, alt}, std::move(raw)}, into);
            else if (broken) stepped(byte, now, into);

            return;
        }

        case State::escaped: {
            if (byte == esc) {
                sent(Token{Lone{}, std::string{static_cast<char>(esc)}}, into);

                return;
            }

            if (byte == '[') {
                state_ = State::csi;
                pending_ += static_cast<char>(byte);

                return;
            }

            if (byte == 'O') {
                state_ = State::ss3;
                pending_ += static_cast<char>(byte);

                return;
            }

            if (stringed(byte)) {
                state_ = State::told;
                pending_ += static_cast<char>(byte);

                return;
            }

            if (byte < 0x80 || leading(byte)) {
                grounded(byte, true, into);

                return;
            }

            state_ = State::ground;
            pending_.clear();

            sent(Token{Lone{}, std::string{static_cast<char>(esc)}}, into);

            return;
        }

        case State::csi: {
            if (pending_.size() == 2 && byte == '[') {
                state_ = State::linux_keyed;
                pending_ += static_cast<char>(byte);

                return;
            }

            if (final_byte(byte)) {
                finished(byte, into);

                return;
            }

            if (byte >= 0x20 && byte <= 0x3F) {
                pending_ += static_cast<char>(byte);

                if (pending_.size() > longest_sequence) {
                    state_ = State::csi_ignored;
                    pending_.clear();
                }

                return;
            }

            if (byte == esc) {
                state_ = State::escaped;
                pending_ = std::string{static_cast<char>(esc)};

                return;
            }

            if (byte == can || byte == sub) {
                state_ = State::ground;
                pending_.clear();

                return;
            }

            if (byte == del) return;

            if (byte < 0x20) {
                sent(Token{Steered{byte, false}, std::string{static_cast<char>(byte)}}, into);

                return;
            }

            state_ = State::ground;
            pending_.clear();

            grounded(byte, false, into);

            return;
        }

        case State::csi_ignored: {
            if (final_byte(byte) || byte == can || byte == sub) {
                state_ = State::ground;

                return;
            }

            if (byte == esc) {
                state_ = State::escaped;
                pending_ = std::string{static_cast<char>(esc)};
            }

            return;
        }

        case State::linux_keyed: {
            if (final_byte(byte)) {
                Csi made;
                made.lead = '[';
                made.last = static_cast<char>(byte);

                std::string raw = std::move(pending_) + static_cast<char>(byte);

                state_ = State::ground;
                pending_.clear();

                sent(Token{std::move(made), std::move(raw)}, into);

                return;
            }

            state_ = State::ground;
            pending_.clear();

            stepped(byte, now, into);

            return;
        }

        case State::ss3: {
            if ((byte >= '0' && byte <= '9') || byte == ';') {
                pending_ += static_cast<char>(byte);

                if (pending_.size() > longest_shift) {
                    state_ = State::ground;
                    pending_.clear();
                }

                return;
            }

            if (final_byte(byte)) {
                Ss3 made;
                made.last = static_cast<char>(byte);

                for (std::size_t at = 2; at < pending_.size(); ++at) {
                    if (pending_[at] == ';') made.number = 0;
                    else made.number = std::min(made.number * 10 + (pending_[at] - '0'), most_value);
                }

                std::string raw = std::move(pending_) + static_cast<char>(byte);

                state_ = State::ground;
                pending_.clear();

                sent(Token{made, std::move(raw)}, into);

                return;
            }

            // Not a single shift after all: Alt+O typed, and whatever came after it goes on as input.
            const std::string held = std::move(pending_);

            state_ = State::ground;
            pending_.clear();

            sent(Token{Plain{U'O', true}, held.substr(0, 2)}, into);

            for (std::size_t at = 2; at < held.size(); ++at) stepped(static_cast<unsigned char>(held[at]), now, into);

            stepped(byte, now, into);

            return;
        }

        case State::told: {
            if (byte == bel) {
                Told made{pending_[1], pending_.substr(2)};

                std::string raw = std::move(pending_) + static_cast<char>(byte);

                state_ = State::ground;
                pending_.clear();

                sent(Token{std::move(made), std::move(raw)}, into);

                return;
            }

            if (byte == can || byte == sub) {
                state_ = State::ground;
                pending_.clear();

                return;
            }

            pending_ += static_cast<char>(byte);

            if (byte == esc) {
                state_ = State::told_escaped;

                return;
            }

            if (pending_.size() > longest_string) {
                state_ = State::told_ignored;
                pending_.clear();
            }

            return;
        }

        case State::told_escaped: {
            if (byte == '\\') {
                if (pending_.size() >= 3) {
                    Told made{pending_[1], pending_.substr(2, pending_.size() - 3)};

                    std::string raw = std::move(pending_) + static_cast<char>(byte);

                    state_ = State::ground;
                    pending_.clear();

                    sent(Token{std::move(made), std::move(raw)}, into);

                    return;
                }

                state_ = State::ground;
                pending_.clear();

                return;
            }

            // Esc in a string without the backslash ends the string unfinished and starts a new
            // escape, as in a terminal's own parser.
            state_ = State::escaped;
            pending_ = std::string{static_cast<char>(esc)};

            stepped(byte, now, into);

            return;
        }

        case State::told_ignored: {
            if (byte == bel || byte == can || byte == sub) {
                state_ = State::ground;

                return;
            }

            if (byte == esc) {
                state_ = State::told_escaped;
                pending_.clear();
            }

            return;
        }

        case State::tracked: {
            pending_ += static_cast<char>(byte);

            if (pending_.size() < 6) return;

            Tracked made{static_cast<unsigned char>(pending_[3]) - 32, static_cast<unsigned char>(pending_[4]) - 32,
                         static_cast<unsigned char>(pending_[5]) - 32};

            std::string raw = std::move(pending_);

            state_ = State::ground;
            pending_.clear();

            sent(Token{made, std::move(raw)}, into);

            return;
        }

        case State::glued: glued(byte, into); return;
    }
}

}
