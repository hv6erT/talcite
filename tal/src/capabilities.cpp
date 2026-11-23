#include "tal/capabilities.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "hexing.hpp"
#include "vt.hpp"

namespace tal {

namespace {

// How long the probe waits for the terminal's answers on a local terminal.
constexpr std::chrono::milliseconds patience{250};

// Over SSH the answer comes back after a full network round trip, and that can take longer than a
// quarter of a second, so the answers would arrive after the probe ended. We wait longer; the
// sentinel ends the wait as soon as the terminal answers anyway, so a fast terminal loses nothing.
constexpr std::chrono::milliseconds afar{1000};

// An environment variable from the console, not from the process. Empty and missing mean the same
// thing here.
std::string named_of(const Console& from, std::string_view what) {
    return from.variable(what).value_or(std::string{});
}

// Whether `what` occurs anywhere in `where`.
bool holds(std::string_view where, std::string_view what) {
    return where.find(what) != std::string_view::npos;
}

// Raw mode for the time of the probe, taken off also when parsing the answer throws. The probe
// starts by itself, so it cannot assume someone has already set the mode, and the console counts
// the requests, so it does not take raw mode away from someone who set it earlier.
class Rawly {
public:
    // Puts the console into raw mode for as long as this object lives.
    explicit Rawly(Console& given) : console_(given) { console_.raw(true); }

    // Takes back this object's request for raw mode.
    ~Rawly() { console_.raw(false); }

    Rawly(const Rawly&) = delete;
    Rawly& operator=(const Rawly&) = delete;
    Rawly(Rawly&&) = delete;
    Rawly& operator=(Rawly&&) = delete;

private:
    Console& console_;
};

// The sequence of questions. Primary DA comes last, because every terminal from the VT100 up
// answers it: when its answer arrives, we know that everything before it has been either answered
// or ignored, and there is nothing more to wait for.
std::string asking() {
    std::string made;

    made += "\x1b[>c";
    made += "\x1b[?1006$p";
    made += "\x1b[?1016$p";
    made += "\x1b[?1000$p";
    made += "\x1b[?2004$p";
    made += "\x1b[?1004$p";
    made += "\x1b[?2026$p";
    made += "\x1b[?1049$p";

    // The cell size in pixels. Without it a picture would have to guess the proportion, and a cell
    // ranges from 1:1.8 to 1:2.4, which shows on every circle.
    made += "\x1b[16t";

    // The kitty protocol query: send one pixel and say whether it worked. A terminal that does not
    // know it should swallow the whole APC sequence silently; one that does not swallow it will
    // spit out junk, and that goes back to the caller as pressed keys anyway.
    made += "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\";

    made += "\x1bP+q5463;524742\x1b\\";
    made += "\x1b]11;?\x1b\\";

    // Width measurements. We start from the first column, so the cursor position report is the
    // width itself, and we do not need to know which row we are on.
    made += "\r\xe4\xbd\xa0\x1b[6n\r\x1b[K";
    made += "\r\xe2\x9d\xa4\xef\xb8\x8f\x1b[6n\r\x1b[K";
    made += "\r\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb\x1b[6n\r\x1b[K";

    // A sextant (U+1FB00) and an octant (U+1CD00), the characters we draw pictures in cells with.
    // This measurement answers only whether the character takes one cell. Whether the font has it
    // at all cannot be measured: a missing glyph is drawn as an empty rectangle and takes exactly
    // the same room. DECRQCRA does not help either, because it sums characters, not pixels, so on
    // an empty rectangle it gives the same as on a real glyph.
    made += "\r\xf0\x9f\xac\x80\x1b[6n\r\x1b[K";
    made += "\r\xf0\x9c\xb4\x80\x1b[6n\r\x1b[K";

    made += "\x1b[c";

    return made;
}

}

struct Capabilities::Held {
    explicit Held(Port<Console> with) : console(std::move(with)) {}

    // The seam for talking to the terminal. Tests put a fake here, so the probe and the floor are
    // checked without a terminal and without changing the process environment.
    Port<Console> console;

    Colors colors = Colors::mono;

    // `NO_COLOR` is a measurement, not the application's wish: it speaks about the environment the
    // program stands in. So it does not block the probe (the probe raises `colors` its own way) but
    // covers the result when it is read. Whoever wants color despite the variable says so through
    // `Setup`.
    bool colorless = false;

    Mouse mouse = Mouse::none;
    bool pixel_mouse = false;
    Screen screen = Screen::none;
    Shade shade = Shade::neutral;

    bool bracketed_paste = false;
    bool focus_events = false;
    bool synchronized = false;
    bool unicode = false;
    bool titles = false;
    bool links = false;

    // The measured widths of the test characters, as in the cursor reports. Zero means no answer.
    int wide = 0;
    int varied = 0;
    int joined = 0;

    Graphics graphics = Graphics::none;

    // Inside tmux or screen; see `ground`.
    bool muxed = false;

    // Zero means "unknown", not "zero pixels". Terminals are allowed not to answer.
    Size cell;

    // The measured widths of the characters we want to draw with. Zero means no answer.
    int sextant = 0;
    int octant = 0;

    std::string named;
    int version = 0;

    std::string rest;

    // The answers are read with the decoder's syntax; the reader keeps each token's bytes, so what
    // is not an answer goes back to the caller as it came.
    vt::Reader reader{true};

    std::vector<int> measured;

    bool asked = false;
    bool done = false;

    bool answered(const vt::Csi& one);

    bool answered(const vt::Told& one);
};

// A probe of the terminal behind the console; nothing is asked until the first question.
Capabilities::Capabilities(Port<Console> with) : held_(std::make_unique<Held>(std::move(with))) {}

Capabilities::~Capabilities() = default;

// The floor from the environment. This is the state we stay in when the terminal answers nothing,
// and that is why it is conservative. The answers that arrive only raise it.
void Capabilities::ground() const {
    Held& held = *held_;

    const Console& from = *held.console;

    const std::string term = named_of(from, "TERM");
    const std::string rich = named_of(from, "COLORTERM");
    const std::string lang = named_of(from, "LANG") + named_of(from, "LC_ALL") + named_of(from, "LC_CTYPE");

    // An empty TERM is the normal state on Windows: the system does not set it. Treating it as a
    // terminal without capabilities would cost all color in Windows Terminal.
#ifdef _WIN32
    const bool console = term != "dumb";
#else
    const bool console = !term.empty() && term != "dumb";
#endif

    held.colorless = !named_of(from, "NO_COLOR").empty();

    // Inside tmux or screen the outer terminal's variables lie. A session started in kitty or in
    // Windows Terminal carries `KITTY_WINDOW_ID`, `WT_SESSION` and its `TERM_PROGRAM` into every
    // window, even when the client already sits in a completely different terminal, and the
    // multiplexer itself does not pass kitty images through. Only `TERM` and the probe tell what it
    // can do here.
    const bool muxed = !named_of(from, "TMUX").empty() || !named_of(from, "STY").empty() ||
                       term.rfind("tmux", 0) == 0 || term.rfind("screen", 0) == 0;

    held.muxed = muxed;

    if (rich == "truecolor" || rich == "24bit" || (!muxed && !named_of(from, "WT_SESSION").empty())) {
        held.colors = Colors::rgb;
    } else if (holds(term, "256")) {
        held.colors = Colors::palette;
    } else if (console) {
        held.colors = from.vt() ? Colors::rgb : Colors::ansi;
    }

#ifdef _WIN32
    held.unicode = true;
#else
    held.unicode = holds(lang, "UTF-8") || holds(lang, "utf8") || holds(lang, "UTF8");
#endif

    // Kitty and ghostty name themselves in `TERM`, and kitty adds a variable of its own. The probe
    // confirms it anyway, but the guess from the environment also works where there is no probe: in
    // a pipe, in a test, with redirected output.
    if (holds(term, "kitty") || holds(term, "ghostty") ||
        (!muxed && !named_of(from, "KITTY_WINDOW_ID").empty()))
        held.graphics = Graphics::kitty;

    if (console) held.mouse = Mouse::basic;

    held.named = muxed ? std::string{} : named_of(from, "TERM_PROGRAM");
    if (held.named.empty()) held.named = term;

    if (console) {
        held.screen = Screen::older;
        held.titles = true;
    }

    // Hyperlinks (OSC 8) have no question a terminal answers, so the floor decides alone. A
    // terminal that does not know them skips the whole control string, as it skips every OSC it
    // does not know, and the text stays plain; inside tmux and screen the multiplexer does the
    // same, unless it passes them on. The one known exception is the Linux console: after `Esc ]`
    // it waits only for its palette commands and prints whatever else follows. On Windows only a
    // console that takes escape sequences gets them; the old one never sees a control string at
    // all.
#ifdef _WIN32
    held.links = console && from.vt();
#else
    held.links = console && term != "linux";
#endif
}

// Sets the floor and runs the probe once, before the first answer is read. A console that is not
// live gets only the floor.
void Capabilities::ready() const {
    if (held_->asked) return;

    held_->asked = true;

    ground();

    if (!held_->console->lively()) return;

    probe();
}

// Asks the terminal every question in raw mode and reads the answers until Primary DA arrives or
// the patience runs out, then stores the measured widths.
void Capabilities::probe() const {
    Console& console = *held_->console;

    const Rawly rawly{console};

    console.write(asking());

    const auto began = std::chrono::steady_clock::now();

    const bool remote = !named_of(console, "SSH_CONNECTION").empty() || !named_of(console, "SSH_TTY").empty();
    const std::chrono::milliseconds waited = remote ? afar : patience;

    while (!held_->done) {
        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - began);

        if (spent >= waited) break;

        const std::string given = console.hear(waited - spent);

        // Empty means the time ran out; going around again would be an idle loop.
        if (given.empty()) break;

        fed(given);
    }

    // Bytes we did not recognize are most likely keys pressed during startup. They have to be
    // handed to the caller, not thrown away; otherwise the first key after starting would sometimes
    // get lost, in a way that cannot be reproduced.
    held_->rest += held_->reader.drained();

    if (held_->measured.size() > 0) held_->wide = held_->measured[0];
    if (held_->measured.size() > 1) held_->varied = held_->measured[1];
    if (held_->measured.size() > 2) held_->joined = held_->measured[2];
    if (held_->measured.size() > 3) held_->sextant = held_->measured[3];
    if (held_->measured.size() > 4) held_->octant = held_->measured[4];
}

namespace {

// A mode state from DECRPM: zero means "I do not know such a mode", anything else that the terminal
// knows it. This is exactly the difference that guessing from the terminal's name never gives.
bool knowing(int state) { return state != 0; }

// A color component from the background report, as a fraction from 0 to 1, or -1 when there are no
// hex digits. The background color comes in hex with a varying number of digits (rgb:R/G/B,
// rgb:RR/GG/BB, up to four digits per component), so each one is scaled by its own range.
double parted(std::string_view said) {
    std::uint32_t made = 0;
    int digits = 0;

    for (const char sign : said) {
        const int value = hex::digit(sign);

        if (value < 0) break;

        made = made * 16 + static_cast<std::uint32_t>(value);
        ++digits;
    }

    if (digits == 0) return -1.0;

    const double most = static_cast<double>((1u << (4 * digits)) - 1);

    return static_cast<double>(made) / most;
}

// Whether the background reported by OSC 11 is light or dark. Brightness is computed with
// perceptual weights, not as a sum: the eye sees green several times more strongly than blue, and a
// background of 0,0,255 is dark despite a sizable sum.
Shade shading(std::string_view body) {
    const std::size_t at = body.find("rgb");
    if (at == std::string_view::npos) return Shade::neutral;

    const std::size_t from = body.find(':', at);
    if (from == std::string_view::npos) return Shade::neutral;

    std::string_view rest = body.substr(from + 1);

    double among[3] = {-1.0, -1.0, -1.0};

    for (double& one : among) {
        one = parted(rest);
        if (one < 0.0) return Shade::neutral;

        const std::size_t slash = rest.find('/');
        if (slash == std::string_view::npos) break;

        rest = rest.substr(slash + 1);
    }

    if (among[1] < 0.0 || among[2] < 0.0) return Shade::neutral;

    const double light = 0.2126 * among[0] + 0.7152 * among[1] + 0.0722 * among[2];

    return light > 0.5 ? Shade::light : Shade::dark;
}

// The terminal family named by the first number of Secondary DA, or empty. The table is short and
// deliberately conservative: an unknown number leaves the name from the environment, because a
// made-up one would be worse than none.
std::string calling(int family) {
    switch (family) {
        case 0: return "xterm";
        case 1: return "vte";
        case 41: return "vt420";
        case 61:
        case 64:
        case 65: return "vt520";
        case 77: return "mintty";
        case 83: return "screen";
        case 84: return "tmux";
        case 85: return "rxvt";
        default: return {};
    }
}

}

// Parsing the answers. What is recognized is taken, what is unfinished waits in the reader, and
// what is foreign is set aside as someone else's bytes: a key pressed during startup looks like
// any other sequence and goes back whole.
void Capabilities::fed(std::string_view bytes) const {
    Held& held = *held_;

    std::vector<vt::Token> tokens;

    held.reader.fed(bytes, std::chrono::steady_clock::now(), tokens);

    for (const vt::Token& one : tokens) {
        if (const auto* csi = std::get_if<vt::Csi>(&one.what)) {
            if (!held.answered(*csi)) held.rest += one.raw;

            continue;
        }

        if (const auto* told = std::get_if<vt::Told>(&one.what)) {
            if (!held.answered(*told)) held.rest += one.raw;

            continue;
        }

        held.rest += one.raw;
    }
}

// Takes an answer in a control sequence: a mode report (DECRPM), a cursor position report (the
// width measurements), Secondary DA (the terminal's family and version), the cell size and, as the
// end of the probe, Primary DA. False for a sequence that is not one of these.
bool Capabilities::Held::answered(const vt::Csi& one) {
    if (one.last == 'y' && one.lead == '?') {
        if (one.params.size() >= 2 && knowing(one.at(1))) switch (one.at(0)) {
                case 1006: mouse = Mouse::sgr; break;
                case 1016: pixel_mouse = true; break;
                case 1000:
                    if (mouse == Mouse::none) mouse = Mouse::basic;
                    break;
                case 2004: bracketed_paste = true; break;
                case 1004: focus_events = true; break;
                case 2026: synchronized = true; break;
                case 1049: screen = Screen::apart; break;
                default: break;
            }

        return true;
    }

    if (one.last == 'R') {
        if (one.params.size() >= 2) measured.push_back(std::max(1, one.at(1) - 1));

        return true;
    }

    if (one.last == 'c' && one.lead == '>') {
        if (one.params.size() >= 2) version = one.at(1);

        if (!one.params.empty()) {
            const std::string mine = calling(one.at(0));

            if (!mine.empty()) named = mine;
        }

        return true;
    }

    // The answer to `16t` reads `6;height;width`; other window reports start with another number
    // and are not ours.
    if (one.last == 't') {
        if (one.params.size() >= 3 && one.at(0) == 6) cell = Size{one.at(2), one.at(1)};

        return true;
    }

    // Sixel is recognized from an answer we get anyway. Primary DA is sent as the probe's
    // terminator, and a 4 in its list means "I can do sixel".
    //
    // Outside a multiplexer only. tmux accepts sixel since version 3.4 and answers with a 4, but it
    // keeps the picture its own way: it loses it when switching windows and scrolling, and with a
    // split screen it can draw it in the neighboring pane. Pixels in tmux are therefore an explicit
    // wish of the application (`Setup::graphics`), not a conclusion from the probe.
    if (one.last == 'c') {
        const bool sixel = std::any_of(one.params.begin(), one.params.end(),
                                       [](const vt::Param& each) { return each.part(0) == 4; });

        if (!muxed && sixel && graphics < Graphics::sixel) graphics = Graphics::sixel;

        done = true;

        return true;
    }

    return false;
}

// Takes an answer in a control string: the kitty graphics reply (APC, the only one we expect, so
// every APC is taken), XTGETTCAP (DCS) and the background color (OSC 11). False for a string that
// is not one of these.
bool Capabilities::Held::answered(const vt::Told& one) {
    if (one.lead == '_') {
        if (holds(one.body, "OK")) graphics = Graphics::kitty;

        return true;
    }

    if (one.lead == 'P' && one.body.rfind("1+r", 0) == 0) {
        if (holds(one.body, "5463") || holds(one.body, "524742")) colors = Colors::rgb;

        return true;
    }

    if (one.lead == 'P' && one.body.rfind("0+r", 0) == 0) return true;

    if (one.lead == ']' && one.body.rfind("11;", 0) == 0) {
        const Shade told = shading(std::string_view{one.body}.substr(3));

        if (told != Shade::neutral) shade = told;

        return true;
    }

    return false;
}

// How many colors the terminal shows; `NO_COLOR` makes it monochrome.
Colors Capabilities::colors() const {
    ready();

    return held_->colorless ? Colors::mono : held_->colors;
}

// Which mouse reporting the terminal supports: in pixels (SGR-Pixels, 1016) where it knows SGR and
// that mode and says the size of its cell, without which pixels could not be turned into cells.
Mouse Capabilities::mouse() const {
    ready();

    if (held_->mouse == Mouse::sgr && held_->pixel_mouse && held_->cell.wide > 0 && held_->cell.high > 0) return Mouse::pixels;

    return held_->mouse;
}

// Which alternate screen the terminal has.
Screen Capabilities::screen() const {
    ready();

    return held_->screen;
}

// Which pixel graphics protocol the terminal speaks, if any.
Graphics Capabilities::graphics() const {
    ready();

    return held_->graphics;
}

// A ceiling, not a choice. The method says how dense a grid the terminal will not scatter, not
// which grid we should draw with. `Term` makes the choice, taking the smaller of the two: the wish
// and this ceiling.
//
// The probe answers only the question "does this character take one cell". It does not answer "does
// the font have it", because a missing glyph is drawn as an empty rectangle and takes exactly the
// same room. That is why the ceiling promises nothing: raised without a wish, it would give half of
// the users a picture made of empty frames, and by default we stay with block characters anyway,
// which have been in Unicode since 1.1 and which every terminal font has.
//
// A missing measurement (zero) does not lower it. A silent terminal has disproved nothing, and that
// is different from a terminal saying "this character takes two cells".
Blocks Capabilities::blocks() const {
    ready();

    if (held_->sextant != 0 && held_->sextant != 1) return Blocks::basic;

    if (held_->octant != 0 && held_->octant != 1) return Blocks::sextants;

    return Blocks::octants;
}

// The size of a cell in pixels; zero in either dimension means the terminal did not say.
Size Capabilities::cell() const {
    ready();

    return held_->cell;
}

// Whether the terminal's background is light or dark, or neutral when it did not say.
Shade Capabilities::shade() const {
    ready();

    return held_->shade;
}

// Whether the terminal knows bracketed paste (mode 2004).
bool Capabilities::bracketed_paste() const {
    ready();

    return held_->bracketed_paste;
}

// Whether the terminal reports focus changes (mode 1004).
bool Capabilities::focus_events() const {
    ready();

    return held_->focus_events;
}

// Whether the terminal knows synchronized output (mode 2026).
bool Capabilities::synchronized() const {
    ready();

    return held_->synchronized;
}

// Whether the terminal takes UTF-8: always on Windows, from the locale variables elsewhere.
bool Capabilities::unicode() const {
    ready();

    return held_->unicode;
}

// Whether the terminal takes a window title.
bool Capabilities::titles() const {
    ready();

    return held_->titles;
}

// Whether the terminal takes hyperlinks (OSC 8); see `ground`.
bool Capabilities::links() const {
    ready();

    return held_->links;
}

// The measured width, in columns, of a Chinese character, the first of three test characters;
// zero when the terminal did not answer.
int Capabilities::wide() const {
    ready();

    return held_->wide;
}

// The measured width, in columns, of an emoji with a variation selector; zero when the terminal
// did not answer.
int Capabilities::varied() const {
    ready();

    return held_->varied;
}

// The measured width, in columns, of an emoji joined with a zero-width joiner; zero when the
// terminal did not answer.
int Capabilities::joined() const {
    ready();

    return held_->joined;
}

// The terminal's name: from Secondary DA when the family is known, else from the environment.
const std::string& Capabilities::named() const {
    ready();

    return held_->named;
}

// The terminal's version from Secondary DA, or zero.
int Capabilities::version() const {
    ready();

    return held_->version;
}

// Bytes that arrived during startup and were not an answer: leftovers, not a buffer.
const std::string& Capabilities::rest() const {
    ready();

    return held_->rest;
}

}
