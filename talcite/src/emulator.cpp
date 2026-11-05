#include "talcite/emulator.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "talcite/encoding.hpp"

#include "hexing.hpp"

// The screen of a terminal, as a program in it sees the screen: the bytes it writes (text, control
// characters and escape sequences of xterm, ECMA-48 and the DEC terminals) turn into a grid of
// cells, a cursor and the terminal's modes. This is pure logic, without input or output: whoever
// holds a pseudoterminal feeds it what the program wrote (`feed`) and draws the grid; what the
// program asked of the terminal (answers to its questions, the bell, the title, the folder the
// shell stands in, the marks of a shell's prompt) comes back from each feeding (`Fed`).
//
// The parser follows the state machine of the DEC terminals: text, an escape sequence, a control
// sequence (CSI) with its parameters, and strings (OSC, which carries the title, the folder and the
// marks; DCS, APC, PM and SOS, which are read and dropped). What it does not know it skips whole,
// so an unknown sequence never shows up as letters. The two screens are those of xterm: the
// primary one with its history, and the alternate one that full-screen programs (vim, less, htop)
// take and give back.
namespace talcite::ui {

namespace {

// The highest value of one parameter; what is larger is taken as this, so that a long run of
// digits cannot overflow.
constexpr int most = 65535;

// The DEC special graphics, the line-drawing set full-screen programs switch to with `ESC ( 0`:
// the characters from `_` to `~` become the corners, lines and symbols below.
constexpr std::u32string_view graphics = U" ◆▒␉␌␍␊°±␤␋┘┐┌└┼⎺⎻─⎼⎽├┤┴┬│≤≥π≠£·";

// A character through the DEC special graphics, when that set is in use.
char32_t drawn(char32_t one, bool special) {
    if (!special || one < U'_' || one > U'~') return one;

    return graphics[static_cast<std::size_t>(one - U'_')];
}

// Appends a decimal number to bytes for an answer.
void numbered(Bytes& into, int one) {
    for (const char each : std::to_string(one)) into.push_back(static_cast<std::byte>(each));
}

// Appends text to bytes for an answer.
void told(Bytes& into, std::string_view said) {
    for (const char each : said) into.push_back(static_cast<std::byte>(each));
}

}

// An empty screen of the given size, with the cursor in the top left corner, and room for `kept`
// lines of history above it.
Emulator::Emulator(Size size, std::size_t kept) : size_{std::max(size.wide, 1), std::max(size.high, 1)}, kept_(kept) {
    primary_.assign(static_cast<std::size_t>(size_.high), blank());
    alternate_.assign(static_cast<std::size_t>(size_.high), blank());

    bottom_ = size_.high - 1;

    tabs_.assign(static_cast<std::size_t>(size_.wide), false);

    for (int at = 8; at < size_.wide; at += 8) tabs_[static_cast<std::size_t>(at)] = true;
}

// The screen being written to: the alternate one while a full-screen program holds it.
std::vector<Emulator::Line>& Emulator::screen() { return modes_.alternate ? alternate_ : primary_; }

// The same, for reading.
const std::vector<Emulator::Line>& Emulator::screen() const { return modes_.alternate ? alternate_ : primary_; }

// How many lines of history lie above the screen. The alternate screen has none: a full-screen
// program draws its whole picture itself.
std::size_t Emulator::history() const { return modes_.alternate ? 0 : history_.size(); }

// A line of the screen, from 0 at the top, or of the history above it, from -1 for the newest. A
// row outside both is an empty line.
const Emulator::Line& Emulator::line(int row) const {
    static const Line none;

    if (row >= 0) return row < size_.high ? screen()[static_cast<std::size_t>(row)] : none;

    const auto back = static_cast<std::size_t>(-row);

    if (back > history()) return none;

    return history_[history_.size() - back];
}

// A line of empty cells in the screen's width.
Emulator::Line Emulator::blank() const { return Line(static_cast<std::size_t>(size_.wide), Cell{}); }

// The cell an erase leaves: a space in the default colors, with the background in use, as xterm
// erases (back color erase).
Cell Emulator::erased() const {
    Cell made;

    made.style.background = style_.background;

    return made;
}

// Whether the shell in the terminal waits at its prompt: it marked the prompt (OSC 133) and has not
// started a command since.
bool Emulator::waiting() const { return stage_ == Stage::prompt || stage_ == Stage::input; }

// The row on the screen where the prompt the shell waits at begins, when it waits and that row is
// on the screen. Whoever shows the terminal under a line of its own for typing commands can leave
// that row and those under it out, so the prompt is not shown twice.
std::optional<int> Emulator::prompt() const {
    if (!waiting() || !prompt_ || modes_.alternate) return std::nullopt;

    if (*prompt_ < scrolled_) return std::nullopt;

    const std::uint64_t row = *prompt_ - scrolled_;

    if (row >= static_cast<std::uint64_t>(size_.high)) return std::nullopt;

    return static_cast<int>(row);
}

// Feeds what the program wrote. Text in UTF-8 may break off in the middle of a character at the end
// of one chunk and go on in the next, so the unfinished bytes wait for it (`utf8::span`, the one
// rule). What the rule does not let through becomes one replacement character for its maximal
// subpart, and the byte that broke it off is read anew.
Fed Emulator::feed(std::span<const std::byte> bytes) {
    Fed fed;

    for (const std::byte each : bytes) {
        const auto one = static_cast<unsigned char>(each);

        // A byte that breaks off an unfinished character is read once more, on its own.
        for (bool anew = true; anew;) {
            anew = false;

            if (partial_.empty() && one < 0x80) {
                step(one, fed);

                break;
            }

            partial_.push_back(static_cast<char>(one));

            const utf8::Span got = utf8::span(partial_);

            if (got.kind == utf8::Span::Kind::partial) break;

            if (got.kind == utf8::Span::Kind::whole) {
                std::size_t at = 0;

                step(utf8::decode(partial_, at), fed);

                partial_.clear();

                break;
            }

            anew = got.size < partial_.size();

            partial_.clear();

            step(U'\uFFFD', fed);
        }
    }

    return fed;
}

// One character through the state machine.
void Emulator::step(char32_t one, Fed& fed) {
    // In a string the escape may start its terminator, `ESC \`. Anything else after it ends the
    // string too, and starts its own escape sequence.
    if (state_ == State::osc || state_ == State::ignored) {
        if (string_escape_) {
            string_escape_ = false;

            stringed(fed);

            state_ = State::ground;

            if (one == U'\\') return;

            state_ = State::escape;
            intermediates_.clear();

            escaped(one);

            return;
        }

        if (one == 0x1B) {
            string_escape_ = true;

            return;
        }

        if (one == 0x07) {
            stringed(fed);

            state_ = State::ground;

            return;
        }

        if (one == 0x18 || one == 0x1A) {
            string_.clear();
            state_ = State::ground;

            return;
        }

        if (state_ == State::osc && one >= 0x20 && string_.size() < 4096) utf8::encode(one, string_);

        return;
    }

    if (one == 0x1B) {
        state_ = State::escape;
        intermediates_.clear();

        return;
    }

    // Cancel and substitute break off whatever sequence was under way.
    if (one == 0x18 || one == 0x1A) {
        state_ = State::ground;

        return;
    }

    if (one < 0x20 || one == 0x7F) {
        if (one != 0x7F) control(one, fed);

        return;
    }

    switch (state_) {
        case State::ground: print(one); return;
        case State::escape:
        case State::intermediate: escaped(one); return;
        case State::csi:
            if (one >= U'0' && one <= U'9') {
                if (fresh_) params_.emplace_back(1, 0), fresh_ = false;

                int& value = params_.back().back();

                value = std::min(value * 10 + static_cast<int>(one - U'0'), most);

                return;
            }

            if (one == U';') {
                if (fresh_) params_.emplace_back(1, 0);

                fresh_ = true;

                return;
            }

            if (one == U':') {
                if (fresh_) params_.emplace_back(1, 0), fresh_ = false;

                params_.back().push_back(0);

                return;
            }

            if (one >= U'<' && one <= U'?') {
                if (params_.empty() && fresh_ && prefix_ == 0 && intermediates_.empty()) prefix_ = static_cast<char>(one);

                return;
            }

            if (one >= 0x20 && one <= 0x2F) {
                intermediates_ += static_cast<char>(one);

                return;
            }

            if (one >= 0x40 && one <= 0x7E) {
                state_ = State::ground;

                dispatched(one, fed);

                return;
            }

            return;
        case State::osc:
        case State::ignored: return;
    }
}

// A control character, wherever it comes: in the middle of a sequence it acts at once, and the
// sequence goes on.
void Emulator::control(char32_t one, Fed& fed) {
    switch (one) {
        case 0x07: ++fed.bells; return;
        case 0x08:
            pending_ = false;

            if (x_ > 0) --x_;

            return;
        case 0x09: tab(1); return;
        case 0x0A:
        case 0x0B:
        case 0x0C:
            index();

            if (modes_.newline) x_ = 0;

            return;
        case 0x0D:
            x_ = 0;
            pending_ = false;

            return;
        case 0x0E: shifted_ = true; return;
        case 0x0F: shifted_ = false; return;
        default: return;
    }
}

// A printable character at the cursor. A character waiting for a wrap (the cursor stood past the
// last column) first goes to the next line; one that takes two cells and has one left goes there
// too. In insert mode what stands to the right moves along. The cursor stops at the last column
// and waits there for the next character, as xterm does, so a line exactly as wide as the screen
// leaves no empty line under it.
void Emulator::print(char32_t one) {
    one = drawn(one, shifted_ ? spare_ : special_);

    const int much = width(one);

    if (much == 0) {
        joined(one);

        return;
    }

    if (pending_ && modes_.wrap) newline();

    if (much == 2 && x_ == size_.wide - 1) {
        if (modes_.wrap) {
            screen()[static_cast<std::size_t>(y_)][static_cast<std::size_t>(x_)] = erased();

            newline();
        } else {
            return;
        }
    }

    Line& row = screen()[static_cast<std::size_t>(y_)];

    if (modes_.insert) {
        for (int at = size_.wide - 1; at >= x_ + much; --at)
            row[static_cast<std::size_t>(at)] = row[static_cast<std::size_t>(at - much)];
    }

    broke(x_);

    if (much == 2) broke(x_ + 1);

    Cell made;

    made.letter = concealed_ ? U' ' : one;
    made.style = style_;

    row[static_cast<std::size_t>(x_)] = made;

    if (much == 2 && x_ + 1 < size_.wide) {
        made.letter = 0;

        row[static_cast<std::size_t>(x_ + 1)] = made;
    }

    last_ = one;

    x_ += much;

    if (x_ >= size_.wide) {
        x_ = size_.wide - 1;
        pending_ = modes_.wrap;
    }
}

// A character that takes no cell (a combining mark, a joiner, a selector) joins the glyph before
// the cursor, as `Sheet::put` joins it; one that would change the glyph's width is left out.
void Emulator::joined(char32_t one) {
    int at = pending_ ? x_ : x_ - 1;

    Line& row = screen()[static_cast<std::size_t>(y_)];

    if (at >= 0 && row[static_cast<std::size_t>(at)].letter == 0) --at;

    if (at < 0) return;

    Cell& cell = row[static_cast<std::size_t>(at)];

    if (cell.letter == 0 || cell.letter == U' ') return;

    const char32_t made = glyph(spelled(cell.letter) + utf8::encode(one));

    if (made != 0 && width(made) == width(cell.letter)) cell.letter = made;
}

// A cell about to be written over: when it is half of a wide character, the other half becomes a
// space, so no half character stays on the screen.
void Emulator::broke(int x) {
    if (x < 0 || x >= size_.wide) return;

    Line& row = screen()[static_cast<std::size_t>(y_)];

    if (row[static_cast<std::size_t>(x)].letter == 0 && x > 0) {
        row[static_cast<std::size_t>(x - 1)].letter = U' ';
    } else if (x + 1 < size_.wide && row[static_cast<std::size_t>(x + 1)].letter == 0 &&
               width(row[static_cast<std::size_t>(x)].letter) == 2) {
        row[static_cast<std::size_t>(x + 1)].letter = U' ';
    }
}

// A line down, scrolling the region when the cursor stands on its last line.
void Emulator::index() {
    pending_ = false;

    if (y_ == bottom_) scroll_up(1);
    else if (y_ < size_.high - 1) ++y_;
}

// A line up, scrolling the region down when the cursor stands on its first line.
void Emulator::reverse() {
    pending_ = false;

    if (y_ == top_) scroll_down(1);
    else if (y_ > 0) --y_;
}

// The start of the next line, as after a wrap.
void Emulator::newline() {
    x_ = 0;

    index();
}

// Scrolls the region up: its top lines leave, empty ones come in at its bottom. Lines that leave
// the top of the whole primary screen go into the history.
void Emulator::scroll_up(int many) {
    many = std::clamp(many, 0, bottom_ - top_ + 1);

    std::vector<Line>& lines = screen();

    for (int turn = 0; turn < many; ++turn) {
        Line gone = std::move(lines[static_cast<std::size_t>(top_)]);

        lines.erase(lines.begin() + top_);
        lines.insert(lines.begin() + bottom_, blank());

        for (Cell& one : lines[static_cast<std::size_t>(bottom_)]) one = erased();

        if (!modes_.alternate && top_ == 0) {
            remembered(std::move(gone));

            ++scrolled_;
        }
    }
}

// Scrolls the region down: empty lines come in at its top, its bottom lines leave.
void Emulator::scroll_down(int many) {
    many = std::clamp(many, 0, bottom_ - top_ + 1);

    std::vector<Line>& lines = screen();

    for (int turn = 0; turn < many; ++turn) {
        lines.erase(lines.begin() + bottom_);
        lines.insert(lines.begin() + top_, blank());

        for (Cell& one : lines[static_cast<std::size_t>(top_)]) one = erased();
    }
}

// A line into the history, the oldest leaving when it is full.
void Emulator::remembered(Line one) {
    if (kept_ == 0) return;

    history_.push_back(std::move(one));

    while (history_.size() > kept_) history_.pop_front();
}

// The final character of an escape sequence, after its intermediates.
void Emulator::escaped(char32_t final) {
    if (final >= 0x20 && final <= 0x2F) {
        intermediates_ += static_cast<char>(final);
        state_ = State::intermediate;

        return;
    }

    state_ = State::ground;

    if (!intermediates_.empty()) {
        // The character sets: `ESC ( 0` takes the line-drawing set for G0, `ESC ( B` gives ASCII
        // back; the same with `)` for G1. `ESC # 8` fills the screen with E, the DEC alignment
        // test.
        if (intermediates_ == "(") special_ = final == U'0';
        else if (intermediates_ == ")") spare_ = final == U'0';
        else if (intermediates_ == "#" && final == U'8') {
            for (Line& row : screen())
                for (Cell& one : row) one = Cell{U'E', Style{}};
        }

        intermediates_.clear();

        return;
    }

    switch (final) {
        case U'[':
            state_ = State::csi;
            params_.clear();
            prefix_ = 0;
            fresh_ = true;

            return;
        case U']':
            state_ = State::osc;
            string_.clear();
            osc_ = true;

            return;
        case U'P':
        case U'X':
        case U'^':
        case U'_':
            state_ = State::ignored;
            string_.clear();
            osc_ = false;

            return;
        case U'7': saved(); return;
        case U'8': restored(); return;
        case U'D': index(); return;
        case U'E': newline(); return;
        case U'M': reverse(); return;
        case U'H':
            if (x_ < size_.wide) tabs_[static_cast<std::size_t>(x_)] = true;

            return;
        case U'c': reset(); return;
        case U'=': modes_.keypad = true; return;
        case U'>': modes_.keypad = false; return;
        default: return;
    }
}

// The value of a parameter of a control sequence; a missing one or zero is the fallback, as the
// sequences define it.
int Emulator::number(std::size_t at, int fallback) const {
    if (at >= params_.size() || params_[at].empty() || params_[at].front() == 0) return fallback;

    return params_[at].front();
}

// A control sequence.
void Emulator::dispatched(char32_t final, Fed& fed) {
    const int one = number(0, 1);

    if (prefix_ == '?') {
        if (final == U'h' || final == U'l') moded(final == U'h', fed);

        return;
    }

    if (prefix_ == '>') {
        // The secondary device attributes: a VT220-like terminal of the first version.
        if (final == U'c') told(fed.replies, "\x1b[>1;10;0c");

        return;
    }

    if (prefix_ != 0) return;

    if (intermediates_ == "!" && final == U'p') {
        // A soft reset: the modes, the region and the style, not the screen.
        modes_.insert = false;
        modes_.origin = false;
        modes_.wrap = true;
        modes_.cursor = true;
        modes_.cursor_keys = false;
        modes_.keypad = false;
        top_ = 0;
        bottom_ = size_.high - 1;
        style_ = Style{};
        concealed_ = false;
        special_ = spare_ = shifted_ = false;
        intermediates_.clear();

        return;
    }

    if (intermediates_ == " " && final == U'q') {
        // The cursor's shape the program asks for (DECSCUSR): 0 the terminal's usual one, 1 and 2
        // a block, 3 and 4 an underline, 5 and 6 a bar, the odd ones blinking.
        const int asked = number(0, 0);

        modes_.caret = asked >= 1 && asked <= 6
                           ? CaretShape{asked <= 2 ? Caret::block : asked <= 4 ? Caret::underline : Caret::bar, asked % 2 == 1}
                           : CaretShape{};

        intermediates_.clear();

        return;
    }

    if (!intermediates_.empty()) {
        // Anything else with an intermediate the screen does not keep, so it is read and dropped.
        intermediates_.clear();

        return;
    }

    const int high = size_.high;
    const int wide = size_.wide;
    const int origin = modes_.origin ? top_ : 0;

    switch (final) {
        case U'@': {
            Line& row = screen()[static_cast<std::size_t>(y_)];
            const int many = std::min(one, wide - x_);

            row.erase(row.end() - many, row.end());
            row.insert(row.begin() + x_, static_cast<std::size_t>(many), erased());
            pending_ = false;

            return;
        }
        case U'A': placed(x_, std::max(y_ - one, y_ >= top_ ? top_ : 0)); return;
        case U'B':
        case U'e': placed(x_, std::min(y_ + one, y_ <= bottom_ ? bottom_ : high - 1)); return;
        case U'C':
        case U'a': placed(std::min(x_ + one, wide - 1), y_); return;
        case U'D': placed(std::max(x_ - one, 0), y_); return;
        case U'E': placed(0, std::min(y_ + one, y_ <= bottom_ ? bottom_ : high - 1)); return;
        case U'F': placed(0, std::max(y_ - one, y_ >= top_ ? top_ : 0)); return;
        case U'G':
        case U'`': placed(std::clamp(one - 1, 0, wide - 1), y_); return;
        case U'H':
        case U'f': {
            const int row = std::clamp(number(0, 1) - 1 + origin, origin, modes_.origin ? bottom_ : high - 1);

            placed(std::clamp(number(1, 1) - 1, 0, wide - 1), row);

            return;
        }
        case U'I': tab(one); return;
        case U'J': erase_display(number(0, 0)); return;
        case U'K': erase_line(number(0, 0)); return;
        case U'L': inserted_lines(one); return;
        case U'M': deleted_lines(one); return;
        case U'P': {
            Line& row = screen()[static_cast<std::size_t>(y_)];
            const int many = std::min(one, wide - x_);

            row.erase(row.begin() + x_, row.begin() + x_ + many);
            row.insert(row.end(), static_cast<std::size_t>(many), erased());
            pending_ = false;

            return;
        }
        case U'S': scroll_up(one); return;
        case U'T': scroll_down(one); return;
        case U'X': {
            Line& row = screen()[static_cast<std::size_t>(y_)];

            for (int at = x_; at < std::min(x_ + one, wide); ++at) row[static_cast<std::size_t>(at)] = erased();

            pending_ = false;

            return;
        }
        case U'Z':
            for (int turn = 0; turn < one && x_ > 0; ++turn) {
                --x_;

                while (x_ > 0 && !tabs_[static_cast<std::size_t>(x_)]) --x_;
            }

            pending_ = false;

            return;
        case U'b':
            if (last_ != 0)
                for (int turn = 0; turn < std::min(one, wide * high); ++turn) print(last_);

            return;
        case U'c':
            // The primary device attributes: a VT220 with color (22), which is what programs need
            // to hear to use colors and the line-drawing set.
            if (number(0, 0) == 0) told(fed.replies, "\x1b[?62;22c");

            return;
        case U'd': placed(x_, std::clamp(one - 1 + origin, 0, high - 1)); return;
        case U'g':
            if (number(0, 0) == 0 && x_ < wide) tabs_[static_cast<std::size_t>(x_)] = false;
            else if (number(0, 0) == 3) std::fill(tabs_.begin(), tabs_.end(), false);

            return;
        case U'h':
        case U'l': {
            const bool on = final == U'h';

            for (std::size_t at = 0; at < params_.size(); ++at) {
                if (number(at, 0) == 4) modes_.insert = on;
                if (number(at, 0) == 20) modes_.newline = on;
            }

            return;
        }
        case U'm': graphic(); return;
        case U'n':
            if (number(0, 0) == 5) told(fed.replies, "\x1b[0n");

            if (number(0, 0) == 6) {
                told(fed.replies, "\x1b[");
                numbered(fed.replies, y_ - origin + 1);
                told(fed.replies, ";");
                numbered(fed.replies, x_ + 1);
                told(fed.replies, "R");
            }

            return;
        case U'r': {
            const int first = number(0, 1) - 1;
            const int last = std::min(number(1, high), high) - 1;

            if (first < last) {
                top_ = first;
                bottom_ = last;

                placed(0, modes_.origin ? top_ : 0);
            }

            return;
        }
        case U's': saved(); return;
        case U'u': restored(); return;
        default: return;
    }
}

// The private modes, `CSI ? Ps h` and `CSI ? Ps l`, several in one sequence.
void Emulator::moded(bool on, Fed& fed) {
    for (std::size_t at = 0; at < params_.size(); ++at) {
        switch (number(at, 0)) {
            case 1: modes_.cursor_keys = on; break;
            case 6:
                modes_.origin = on;

                placed(0, on ? top_ : 0);

                break;
            case 7: modes_.wrap = on; break;
            case 25: modes_.cursor = on; break;
            case 47: switched(on, false, fed); break;
            case 1047: switched(on, true, fed); break;
            case 1048:
                if (on) saved();
                else restored();

                break;
            case 1049:
                if (on) {
                    saved();
                    switched(true, true, fed);
                } else {
                    switched(false, false, fed);
                    restored();
                }

                break;
            case 1000: modes_.mouse = on ? Mousing::clicks : Mousing::none; break;
            case 1002: modes_.mouse = on ? Mousing::drags : Mousing::none; break;
            case 1003: modes_.mouse = on ? Mousing::moves : Mousing::none; break;
            case 1004: modes_.focus = on; break;
            case 1006: modes_.sgr_mouse = on; break;
            case 2004: modes_.paste = on; break;
            default: break;
        }
    }
}

// The color of an extended color parameter at `at`: `5;n` from the palette of 256, `2;r;g;b` by its
// components, written either way xterm takes them, with semicolons or with colons. `at` moves past
// what it used.
Color Emulator::colored(std::size_t& at) const {
    const std::vector<int>& group = params_[at];

    // With colons everything is in one parameter: 38:5:n or 38:2::r:g:b (the empty one is the color
    // space, which may also be left out).
    if (group.size() > 1) {
        if (group[1] == 5 && group.size() > 2) return Color::indexed(group[2]).value_or(Color{});

        if (group[1] == 2) {
            const std::size_t from = group.size() >= 6 ? 3 : 2;

            if (group.size() >= from + 3)
                return Color{false, static_cast<std::uint8_t>(std::clamp(group[from], 0, 255)),
                             static_cast<std::uint8_t>(std::clamp(group[from + 1], 0, 255)),
                             static_cast<std::uint8_t>(std::clamp(group[from + 2], 0, 255))};
        }

        return Color{};
    }

    const auto value = [this](std::size_t which) {
        return which < params_.size() && !params_[which].empty() ? params_[which].front() : 0;
    };

    if (value(at + 1) == 5) {
        const int which = value(at + 2);

        at += 2;

        return Color::indexed(which).value_or(Color{});
    }

    if (value(at + 1) == 2) {
        const Color made{false, static_cast<std::uint8_t>(std::clamp(value(at + 2), 0, 255)),
                         static_cast<std::uint8_t>(std::clamp(value(at + 3), 0, 255)),
                         static_cast<std::uint8_t>(std::clamp(value(at + 4), 0, 255))};

        at += 4;

        return made;
    }

    return Color{};
}

// The style of what is written next (SGR). An empty sequence is a reset. Colors 0–7 and their
// bright halves come from the palette of 256; the default colors stay transparent, so whoever draws
// the screen gives them its own.
void Emulator::graphic() {
    if (params_.empty()) {
        style_ = Style{};
        concealed_ = false;

        return;
    }

    for (std::size_t at = 0; at < params_.size(); ++at) {
        const int one = params_[at].empty() ? 0 : params_[at].front();

        switch (one) {
            case 0:
                style_ = Style{};
                concealed_ = false;

                break;
            case 1: style_.bold = true; break;
            case 2: style_.faint = true; break;
            case 3: style_.italic = true; break;
            case 4: style_.under = params_[at].size() < 2 || params_[at][1] != 0; break;
            case 7: style_.reverse = true; break;
            case 8: concealed_ = true; break;
            case 9: style_.crossed = true; break;
            case 21: style_.under = true; break;
            case 22: style_.bold = style_.faint = false; break;
            case 23: style_.italic = false; break;
            case 24: style_.under = false; break;
            case 27: style_.reverse = false; break;
            case 28: concealed_ = false; break;
            case 29: style_.crossed = false; break;
            case 38: style_.foreground = colored(at); break;
            case 39: style_.foreground = Color{}; break;
            case 48: style_.background = colored(at); break;
            case 49: style_.background = Color{}; break;
            default:
                if (one >= 30 && one <= 37) style_.foreground = Color::indexed(one - 30).value_or(Color{});
                else if (one >= 40 && one <= 47) style_.background = Color::indexed(one - 40).value_or(Color{});
                else if (one >= 90 && one <= 97) style_.foreground = Color::indexed(one - 90 + 8).value_or(Color{});
                else if (one >= 100 && one <= 107) style_.background = Color::indexed(one - 100 + 8).value_or(Color{});

                break;
        }
    }
}

// Erases in the display: from the cursor to the end (0), from the start to the cursor (1), the
// whole screen (2), or the history (3).
void Emulator::erase_display(int how) {
    std::vector<Line>& lines = screen();

    pending_ = false;

    if (how == 3) {
        history_.clear();

        return;
    }

    for (int y = 0; y < size_.high; ++y) {
        const bool whole = how == 2 || (how == 0 && y > y_) || (how == 1 && y < y_);

        if (whole) {
            for (Cell& one : lines[static_cast<std::size_t>(y)]) one = erased();
        }
    }

    if (how == 0 || how == 1) erase_line(how);
}

// Erases in the line: from the cursor to its end (0), from its start to the cursor (1), or the
// whole line (2).
void Emulator::erase_line(int how) {
    Line& row = screen()[static_cast<std::size_t>(y_)];

    pending_ = false;

    const int from = how == 0 ? x_ : 0;
    const int to = how == 1 ? x_ + 1 : size_.wide;

    for (int at = from; at < std::min(to, size_.wide); ++at) row[static_cast<std::size_t>(at)] = erased();
}

// Inserts empty lines at the cursor, within the scroll region; the region's last lines leave.
void Emulator::inserted_lines(int many) {
    if (y_ < top_ || y_ > bottom_) return;

    const int was = top_;

    top_ = y_;

    scroll_down(many);

    top_ = was;
    x_ = 0;
    pending_ = false;
}

// Deletes lines at the cursor, within the scroll region; empty ones come in at its bottom.
void Emulator::deleted_lines(int many) {
    if (y_ < top_ || y_ > bottom_) return;

    const int was = top_;

    top_ = y_;

    // The lines deleted here go nowhere: they are not the top of the screen leaving.
    std::vector<Line>& lines = screen();

    many = std::clamp(many, 0, bottom_ - top_ + 1);

    for (int turn = 0; turn < many; ++turn) {
        lines.erase(lines.begin() + top_);
        lines.insert(lines.begin() + bottom_, blank());

        for (Cell& one : lines[static_cast<std::size_t>(bottom_)]) one = erased();
    }

    top_ = was;
    x_ = 0;
    pending_ = false;
}

// To the next tab stop, `many` times, or to the last column.
void Emulator::tab(int many) {
    pending_ = false;

    for (int turn = 0; turn < many; ++turn) {
        ++x_;

        while (x_ < size_.wide - 1 && !tabs_[static_cast<std::size_t>(x_)]) ++x_;

        if (x_ >= size_.wide - 1) {
            x_ = size_.wide - 1;

            return;
        }
    }
}

// The cursor to a cell of the screen; it waits for no wrap there.
void Emulator::placed(int x, int y) {
    x_ = std::clamp(x, 0, size_.wide - 1);
    y_ = std::clamp(y, 0, size_.high - 1);
    pending_ = false;
}

// Saves the cursor (DECSC): its place, the style, the origin mode and the character sets, one set
// for each screen.
void Emulator::saved() {
    Saved& into = modes_.alternate ? alternate_saved_ : primary_saved_;

    into = Saved{x_, y_, pending_, style_, concealed_, modes_.origin, special_, spare_, shifted_};
}

// Restores what `saved` kept; without a save, the top left corner and the default style.
void Emulator::restored() {
    const Saved& from = modes_.alternate ? alternate_saved_ : primary_saved_;

    placed(from.x, from.y);

    pending_ = from.pending;
    style_ = from.style;
    concealed_ = from.concealed;
    modes_.origin = from.origin;
    special_ = from.special;
    spare_ = from.spare;
    shifted_ = from.shifted;
}

// A full reset (RIS): an empty primary screen, the history kept, the modes and the style as at
// the start.
void Emulator::reset() {
    primary_.assign(static_cast<std::size_t>(size_.high), Line(static_cast<std::size_t>(size_.wide), Cell{}));
    alternate_ = primary_;

    modes_ = Modes{};
    style_ = Style{};
    concealed_ = false;
    special_ = spare_ = shifted_ = false;
    top_ = 0;
    bottom_ = size_.high - 1;
    primary_saved_ = alternate_saved_ = Saved{};

    std::fill(tabs_.begin(), tabs_.end(), false);

    for (int at = 8; at < size_.wide; at += 8) tabs_[static_cast<std::size_t>(at)] = true;

    placed(0, 0);
}

// To the alternate screen and back. `clearing` empties the alternate screen on the way in; the
// primary one waits under it unchanged.
void Emulator::switched(bool to, bool clearing, Fed& fed) {
    if (modes_.alternate == to) return;

    if (to && clearing)
        for (Line& row : alternate_)
            for (Cell& one : row) one = Cell{};

    modes_.alternate = to;
    fed.switched = true;
    pending_ = false;
}

// The end of a string. An OSC carries the title (0 and 2), the folder the shell stands in (7, as an
// address; 9;9, as a path, the way Windows Terminal takes it from cmd and PowerShell), and the
// marks of a shell's prompt (133). The other strings are dropped.
void Emulator::stringed(Fed& fed) {
    const std::string said = std::move(string_);

    string_.clear();

    if (!osc_) return;

    const std::size_t semicolon = said.find(';');
    const std::string_view kind = std::string_view{said}.substr(0, semicolon);
    const std::string rest = semicolon == std::string::npos ? std::string{} : said.substr(semicolon + 1);

    if (kind == "0" || kind == "2") {
        title_ = rest;
        fed.titled = true;

        return;
    }

    if (kind == "7") {
        const std::string was = folder_;

        foldered(rest);

        fed.moved = folder_ != was;

        return;
    }

    if (kind == "9" && rest.starts_with("9;")) {
        std::string path = rest.substr(2);

        if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);

        fed.moved = path != folder_;
        folder_ = path;

        return;
    }

    if (kind == "133") marked(rest, fed);
}

// The folder from an address `file://host/path`, its escapes undone. On Windows the path comes as
// `/C:/...`, and the slash before the drive goes.
void Emulator::foldered(const std::string& said) {
    constexpr std::string_view scheme = "file://";

    if (!said.starts_with(scheme)) return;

    const std::size_t slash = said.find('/', scheme.size());

    if (slash == std::string::npos) return;

    std::string path = hex::unescaped(std::string_view{said}.substr(slash));

    if (path.size() >= 3 && path[0] == '/' && path[2] == ':') path.erase(0, 1);

    folder_ = path;
}

// A mark of a shell's prompt (OSC 133): A where the prompt begins, B where the command line does,
// C where the command's output does, D with the exit code when it ends. The screen keeps where the
// last prompt began, so the prompt can be left out of the picture while the shell waits there.
void Emulator::marked(const std::string& said, Fed& fed) {
    if (said.empty()) return;

    ShellMark made;

    switch (said.front()) {
        case 'A':
            made.kind = Marked::prompt;
            stage_ = Stage::prompt;

            if (!modes_.alternate) prompt_ = scrolled_ + static_cast<std::uint64_t>(y_);

            break;
        case 'B':
            made.kind = Marked::input;
            stage_ = Stage::input;

            break;
        case 'C':
            made.kind = Marked::output;
            stage_ = Stage::running;

            break;
        case 'D': {
            made.kind = Marked::done;
            stage_ = Stage::unknown;

            if (said.size() > 2 && said[1] == ';') {
                int code = 0;
                bool any = false;

                for (std::size_t at = 2; at < said.size() && said[at] >= '0' && said[at] <= '9'; ++at) {
                    code = std::min(code * 10 + (said[at] - '0'), most);
                    any = true;
                }

                if (any) made.code = code;
            }

            break;
        }
        default: return;
    }

    fed.marks.push_back(made);
}

// A new size. Lines keep their cells; a narrower screen cuts them, a wider one adds empty cells. A
// lower primary screen first drops the empty lines under its cursor and then sends its top lines
// into the history, so the cursor and what is around it stay in sight; a higher one takes lines
// back from the history, so what scrolled away comes back, as in xterm. While a full-screen program
// holds the alternate screen, the primary one does the same under it, around the cursor it will
// get back. The alternate screen grows and shrinks at its bottom: its program draws it anew at once
// anyway. The scroll region goes back to the whole screen.
void Emulator::resize(Size size) {
    size.wide = std::max(size.wide, 1);
    size.high = std::max(size.high, 1);

    if (size == size_) return;

    const auto cut = [&size](Line& one) {
        one.resize(static_cast<std::size_t>(size.wide), Cell{});

        // A wide character cut in half at the new edge leaves a space.
        if (!one.empty() && width(one.back().letter) == 2) one.back().letter = U' ';
    };

    for (Line& one : primary_) cut(one);
    for (Line& one : alternate_) cut(one);
    for (Line& one : history_) cut(one);

    // The cursor of the primary screen: its own while it is shown, the one saved for it while a
    // full-screen program holds the alternate screen.
    int& row = modes_.alternate ? primary_saved_.y : y_;

    const Line empty(static_cast<std::size_t>(size.wide), Cell{});

    const auto vacant = [](const Line& one) { return std::all_of(one.begin(), one.end(), [](const Cell& each) { return each == Cell{}; }); };

    if (size.high < size_.high) {
        int excess = size_.high - size.high;

        while (excess > 0 && static_cast<int>(primary_.size()) - 1 > row && vacant(primary_.back())) {
            primary_.pop_back();
            --excess;
        }

        while (excess > 0) {
            remembered(std::move(primary_.front()));

            primary_.erase(primary_.begin());

            ++scrolled_;
            --excess;
            --row;
        }
    } else {
        for (int room = size.high - size_.high; room > 0 && !history_.empty(); --room) {
            primary_.insert(primary_.begin(), std::move(history_.back()));
            history_.pop_back();

            if (scrolled_ > 0) --scrolled_;

            ++row;
        }
    }

    primary_.resize(static_cast<std::size_t>(size.high), empty);
    alternate_.resize(static_cast<std::size_t>(size.high), empty);

    tabs_.resize(static_cast<std::size_t>(size.wide), false);

    for (int at = (size_.wide + 7) / 8 * 8; at < size.wide; at += 8) tabs_[static_cast<std::size_t>(at)] = true;

    size_ = size;
    top_ = 0;
    bottom_ = size_.high - 1;

    row = std::clamp(row, 0, size_.high - 1);

    x_ = std::clamp(x_, 0, size_.wide - 1);
    y_ = std::clamp(y_, 0, size_.high - 1);
    pending_ = false;
}

}
