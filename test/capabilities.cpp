#include "tal/testing.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "faking.hpp"
#include "talcite/capabilities.hpp"

using namespace talcite;

namespace {

// The line to the fake stands in a base class before `Capabilities`, because that one gets the
// console already at construction. The fake pretends nothing beyond the conversation: the probe
// and the floor go through the real code, so parsing the answers is checked without any terminal
// and on every system.
struct Wire {
    std::shared_ptr<faking::Terminal> line;
};

// A recorded terminal.
class Taped : private Wire, public Capabilities {
public:
    explicit Taped(std::string said, std::size_t piece = 0)
        : Taped(faking::terminal(std::move(said), piece)) {}

    explicit Taped(std::shared_ptr<faking::Terminal> given)
        : Wire{std::move(given)}, Capabilities(Seam<Console>{line}) {}

    // Everything the probe wrote to the terminal.
    [[nodiscard]] const std::string& asked() const { return line->written; }

    // How many times the probe waited for the terminal's answer.
    [[nodiscard]] int rounds() const { return line->rounds; }

    // The fake terminal itself, for its raw-mode counters.
    [[nodiscard]] const faking::Terminal& terminal() const { return *line; }
};

// A terminal that does not answer: not a single byte, not even to the sentinel. It answers for the
// console itself instead of asking the real one; otherwise the result would depend on what the
// program was started from.
std::shared_ptr<faking::Terminal> mute(bool vt = false) {
    auto made = faking::terminal();

    made->sequences = vt;

    return made;
}

// A terminal there is no way to talk to: output redirected to a pipe.
std::shared_ptr<faking::Terminal> apart() {
    auto made = faking::terminal();

    made->alive = false;

    return made;
}

const std::string modern =
    "\x1b[>0;280;0c"
    "\x1b[?1006;2$y"
    "\x1b[?1000;2$y"
    "\x1b[?2004;2$y"
    "\x1b[?1004;2$y"
    "\x1b[?2026;2$y"
    "\x1b[?1049;2$y"
    "\x1bP1+r5463=787465726d2d323536636f6c6f72\x1b\\"
    "\x1b]11;rgb:0000/0000/0000\x1b\\"
    "\x1b[1;3R"
    "\x1b[1;2R"
    "\x1b[1;3R"
    "\x1b[?62;1;2;6;9;15;22;29c";

}

TRIAL("Capabilities: the questions end with a sentinel") {
    const Taped one{modern};

    (void)one.colors();

    const std::string& asked = one.asked();

    OUGHT(!asked.empty());
    OUGHT(asked.rfind("\x1b[c") == asked.size() - 3);
}

TRIAL("Capabilities: a modern terminal reports everything") {
    const Taped one{modern};

    SAME(static_cast<int>(one.mouse()), static_cast<int>(Mouse::sgr));
    SAME(static_cast<int>(one.screen()), static_cast<int>(Screen::apart));
    OUGHT(one.bracketed_paste());
    OUGHT(one.focus_events());
    OUGHT(one.synchronized());
    SAME(static_cast<int>(one.shade()), static_cast<int>(Shade::dark));
    SAME(one.version(), 280);
    SAME(one.named(), std::string{"xterm"});
}

TRIAL("Capabilities: the width measurements come back in order") {
    const Taped one{modern};

    SAME(one.wide(), 2);
    SAME(one.varied(), 1);
    SAME(one.joined(), 2);
}

TRIAL("Capabilities: XTGETTCAP raises color to full") {
    const Taped one{modern};

    SAME(static_cast<int>(one.colors()), static_cast<int>(Colors::rgb));
}

TRIAL("Capabilities: an unknown mode stays unknown") {
    const Taped one{
        "\x1b[?1006;0$y"
        "\x1b[?2004;0$y"
        "\x1b[?2026;0$y"
        "\x1b[?62;1c"};

    SAME(static_cast<int>(one.mouse()), static_cast<int>(Mouse::basic));
    OUGHT(!one.bracketed_paste());
    OUGHT(!one.synchronized());
}

TRIAL("Capabilities: a light background is recognized") {
    const Taped one{
        "\x1b]11;rgb:ffff/ffff/ffff\x1b\\"
        "\x1b[?62;1c"};

    SAME(static_cast<int>(one.shade()), static_cast<int>(Shade::light));
}

TRIAL("Capabilities: the sentinel ends the wait at once") {
    const Taped one{"\x1b[?62;1c"};

    (void)one.colors();

    SAME(one.rounds(), 1);
}

TRIAL("Capabilities: a silent terminal stays at the floor") {
    const Taped one{mute()};

    SAME(static_cast<int>(one.screen()), static_cast<int>(Screen::older));
    OUGHT(!one.bracketed_paste());
    OUGHT(!one.synchronized());
    SAME(one.wide(), 0);
    SAME(one.varied(), 0);
    SAME(one.joined(), 0);
}

TRIAL("Capabilities: without a terminal not a single byte is sent") {
    const Taped one{apart()};

    (void)one.colors();

    OUGHT(one.asked().empty());
}

TRIAL("Capabilities: the probe runs once, however many questions there are") {
    const Taped one{modern};

    (void)one.colors();
    (void)one.mouse();
    (void)one.bracketed_paste();
    (void)one.wide();

    SAME(one.rounds(), 1);
}

TRIAL("Capabilities: other bytes are not lost") {
    const Taped one{
        "abc"
        "\x1b[?2004;2$y"
        "q"
        "\x1b[?62;1c"};

    OUGHT(one.bracketed_paste());
    SAME(one.rest(), std::string{"abcq"});
}

TRIAL("Capabilities: a fragmented answer is put back together") {
    const Taped one{modern, 3};

    SAME(static_cast<int>(one.mouse()), static_cast<int>(Mouse::sgr));
    OUGHT(one.bracketed_paste());
    SAME(one.wide(), 2);
}

TRIAL("Capabilities: the short form of the background color works too") {
    const Taped one{
        "\x1b]11;rgb:f/f/f\x1b\\"
        "\x1b[?62;1c"};

    SAME(static_cast<int>(one.shade()), static_cast<int>(Shade::light));
}

TRIAL("Capabilities: pure blue is a dark background, though its component is high") {
    const Taped one{
        "\x1b]11;rgb:0000/0000/ffff\x1b\\"
        "\x1b[?62;1c"};

    SAME(static_cast<int>(one.shade()), static_cast<int>(Shade::dark));
}

TRIAL("Capabilities: a background color ended with BEL counts too") {
    const Taped one{
        "\x1b]11;rgb:ffff/ffff/ffff\x07"
        "\x1b[?62;1c"};

    SAME(static_cast<int>(one.shade()), static_cast<int>(Shade::light));
}

// A tail that is unfinished when the probe ends may just as well be the start of a pressed key.
// Throwing it away would lose the key, so it goes to the other bytes.
TRIAL("Capabilities: an unfinished tail is not lost") {
    const Taped one{
        "\x1b[?2004;2$y"
        "\x1b[?62;1c"
        "\x1b[A"};

    OUGHT(one.bracketed_paste());
    SAME(one.rest(), std::string{"\x1b[A"});
}

TRIAL("Capabilities: tmux and screen are recognized by their family") {
    const Taped one{"\x1b[>84;0;0c\x1b[?62;1c"};
    const Taped two{"\x1b[>83;40500;0c\x1b[?62;1c"};

    SAME(one.named(), std::string{"tmux"});
    SAME(two.named(), std::string{"screen"});
}

TRIAL("Capabilities: keys pressed during startup are not lost") {
    const Taped one{
        "\x1b[?2004;2$y"
        "\x1b[A"
        "\x1b[?62;1c"
        "q"};

    OUGHT(one.bracketed_paste());
    SAME(one.rest(), std::string{"\x1b[A" "q"});
}

// The probe reads the answers with the decoder's own syntax and hands every other token back as
// the bytes it came in: Alt with a letter, a character, a paste, a single shift, a mouse report,
// and the unfinished tail. An XTGETTCAP answer ended with BEL counts as an answer.
TRIAL("Capabilities: keys of every form come back as they came") {
    const Taped one{
        "\x1bx"
        "ł"
        "\x1b[?1006;2$y"
        "\x1b[200~wklej\x1b[201~"
        "\x1bP1+r5463=78\x07"
        "\x1bOP"
        "\x1b[<0;3;4M"
        "\x1b[?62;1c"
        "\x1b[1;5"};

    SAME(static_cast<int>(one.mouse()), static_cast<int>(Mouse::sgr));
    SAME(static_cast<int>(one.colors()), static_cast<int>(Colors::rgb));
    SAME(one.rest(), std::string{"\x1bx" "ł" "\x1b[200~wklej\x1b[201~" "\x1bOP" "\x1b[<0;3;4M" "\x1b[1;5"});
}

TRIAL("Capabilities: an unknown OSC answer also comes back whole") {
    const Taped one{
        "\x1b]10;rgb:1111/1111/1111\x1b\\"
        "\x1b[?62;1c"};

    SAME(one.rest(), std::string{"\x1b]10;rgb:1111/1111/1111\x1b\\"});
}

// The floor reads the environment from the console, so the trial sets it on the fake instead of
// changing the variables of the whole process, and so does not depend on what the caller left in
// the environment.
TRIAL("Capabilities: NO_COLOR turns color off despite the terminal's answer") {
    auto line = faking::terminal(modern);

    line->environment["NO_COLOR"] = "1";
    line->environment["COLORTERM"] = "truecolor";

    const Taped told{line};

    SAME(static_cast<int>(told.colors()), static_cast<int>(Colors::mono));
}

TRIAL("Capabilities: COLORTERM raises to the full palette") {
    auto line = mute();

    line->environment["COLORTERM"] = "truecolor";
    line->environment["TERM"] = "xterm";

    const Taped told{line};

    SAME(static_cast<int>(told.colors()), static_cast<int>(Colors::rgb));
}

TRIAL("Capabilities: TERM with 256 gives the indexed palette") {
    auto line = mute();

    line->environment["TERM"] = "xterm-256color";

    const Taped told{line};

    SAME(static_cast<int>(told.colors()), static_cast<int>(Colors::palette));
}

TRIAL("Capabilities: TERM dumb means nothing") {
    auto line = mute();

    line->environment["TERM"] = "dumb";

    const Taped told{line};

    SAME(static_cast<int>(told.colors()), static_cast<int>(Colors::mono));
    SAME(static_cast<int>(told.screen()), static_cast<int>(Screen::none));
    SAME(static_cast<int>(told.mouse()), static_cast<int>(Mouse::none));
    OUGHT(!told.titles());
}

// Hyperlinks have no question a terminal answers, so the floor decides: every terminal that skips
// an OSC it does not know takes them, and dumb takes nothing. The Linux console prints what follows
// `Esc ]` unless it is a palette command, so there they would come out as text. On Windows the
// console decides: only one that takes escape sequences gets them.
TRIAL("Capabilities: hyperlinks wherever an unknown OSC is skipped, not on the Linux console") {
#ifdef _WIN32
    const bool sequences = true;
#else
    const bool sequences = false;
#endif

    auto line = mute(sequences);

    line->environment["TERM"] = "xterm-256color";

    OUGHT(Taped{line}.links());

    auto kernel = mute(sequences);

    kernel->environment["TERM"] = "linux";

#ifdef _WIN32
    OUGHT(Taped{kernel}.links());
#else
    OUGHT(!Taped{kernel}.links());
#endif

    auto dumb = mute(sequences);

    dumb->environment["TERM"] = "dumb";

    OUGHT(!Taped{dumb}.links());

#ifdef _WIN32
    auto old = mute(false);

    old->environment["TERM"] = "xterm-256color";

    OUGHT(!Taped{old}.links());
#endif
}

// On this path the console decides, not TERM, and only here, because all the earlier branches
// (NO_COLOR, COLORTERM, 256, dumb) end before the console is asked. The fake supplies the console's
// answer, so both sides of this decision are checked on every system, and the result does not
// depend on what the program was started from.
TRIAL("Capabilities: a plain TERM with a console without sequences gives sixteen colors") {
    auto line = mute(false);

    line->environment["TERM"] = "vt220";

    const Taped told{line};

    SAME(static_cast<int>(told.colors()), static_cast<int>(Colors::ansi));
}

TRIAL("Capabilities: a plain TERM with a console with sequences gives the full palette") {
    auto line = mute(true);

    line->environment["TERM"] = "vt220";

    const Taped told{line};

    SAME(static_cast<int>(told.colors()), static_cast<int>(Colors::rgb));
}

TRIAL("Capabilities: COLORTERM 24bit also raises to the full palette") {
    auto line = mute();

    line->environment["COLORTERM"] = "24bit";

    const Taped told{line};

    SAME(static_cast<int>(told.colors()), static_cast<int>(Colors::rgb));
}

// Windows Terminal sets neither COLORTERM nor TERM, but it has the full palette and announces
// itself with its own variable.
TRIAL("Capabilities: WT_SESSION gives the full palette without TERM") {
    auto line = mute();

    line->environment.erase("TERM");
    line->environment["WT_SESSION"] = "1";

    const Taped told{line};

    SAME(static_cast<int>(told.colors()), static_cast<int>(Colors::rgb));
}

// The NO_COLOR convention says "present and not empty". An empty variable turns nothing off.
TRIAL("Capabilities: an empty NO_COLOR does not turn color off") {
    auto line = mute();

    line->environment["NO_COLOR"] = "";

    const Taped told{line};

    SAME(static_cast<int>(told.colors()), static_cast<int>(Colors::palette));
}

TRIAL("Capabilities: KITTY_WINDOW_ID guesses kitty images without a probe") {
    auto line = apart();

    line->environment["KITTY_WINDOW_ID"] = "1";

    const Taped told{line};

    SAME(static_cast<int>(told.graphics()), static_cast<int>(Graphics::kitty));
}

TRIAL("Capabilities: the name comes from TERM_PROGRAM, and without it from TERM") {
    auto one = apart();
    auto two = apart();

    one->environment["TERM_PROGRAM"] = "WezTerm";

    const Taped first{one};
    const Taped second{two};

    SAME(first.named(), std::string{"WezTerm"});
    SAME(second.named(), std::string{"xterm-256color"});
}

// On Windows the floor always assumes unicode, so the variables decide only on POSIX.
TRIAL("Capabilities: LANG, LC_ALL or LC_CTYPE decide unicode") {
    auto plain = apart();
    auto fine = apart();

    plain->environment["LANG"] = "C";
    fine->environment["LANG"] = "C";
    fine->environment["LC_CTYPE"] = "pl_PL.utf8";

    const Taped one{plain};
    const Taped two{fine};

#ifdef _WIN32

    OUGHT(one.unicode());

#else

    OUGHT(!one.unicode());

#endif

    OUGHT(two.unicode());
}

TRIAL("Capabilities: the probe runs in raw mode and leaves it afterwards") {
    const Taped one{modern};

    (void)one.colors();

    SAME(one.terminal().entered, 1);
    SAME(one.terminal().depth, 0);
}

TRIAL("Capabilities: without a terminal there is no raw mode") {
    const Taped one{apart()};

    (void)one.colors();

    SAME(one.terminal().entered, 0);
}

// --- Images -------------------------------------------------------------------------------------
//
// Sixel needs no question of its own: the primary DA is sent as the probe's sentinel, and a 4 in
// its list means "I can do sixel". Kitty answers in an APC. The cell size in pixels comes from 16t.

namespace {

// A recorded answer with five width measurements in order (wide character, combining mark,
// joiner, sextant, octant), each digit in `wides` being a width, then `more` and the DA `da`.
std::string taped(const char* wides, const char* da, const char* more = "") {
    std::string made;

    made += "\x1b[>0;280;0c";
    made += more;

    for (const char* at = wides; *at != '\0'; ++at) {
        made += "\x1b[1;";
        made += static_cast<char>('0' + (*at - '0') + 1);
        made += "R";
    }

    made += da;

    return made;
}

const char* const plain = "\x1b[?62;1;2;6;22c";
const char* const drawing = "\x1b[?62;1;2;4;6;22c";

}

TRIAL("Capabilities: sixel is recognized from the primary DA") {
    const Taped one{taped("21211", drawing)};

    SAME(static_cast<int>(one.graphics()), static_cast<int>(Graphics::sixel));
}

TRIAL("Capabilities: without a 4 in DA there is no sixel") {
    const Taped one{taped("21211", plain)};

    SAME(static_cast<int>(one.graphics()), static_cast<int>(Graphics::none));
}

// tmux from 3.4 on answers with a 4, but it loses images and draws them into other panes, so
// pixels in it are an explicit wish of the application, not a conclusion from the probe. The same
// terminal without tmux does have sixel.
TRIAL("Capabilities: in tmux and in screen a 4 in DA gives no sixel") {
    for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{
             {"TMUX", "/tmp/tmux-1000/default,1234,0"}, {"STY", "1234.pts-0.host"}}) {
        auto line = faking::terminal(taped("21211", drawing));

        line->environment[name] = value;

        const Taped one{line};

        SAME(static_cast<int>(one.graphics()), static_cast<int>(Graphics::none));
    }

    auto line = faking::terminal(taped("21211", drawing));

    line->environment["TERM"] = "tmux-256color";

    SAME(static_cast<int>(Taped{line}.graphics()), static_cast<int>(Graphics::none));
}

// Kitty is better than sixel (full color instead of a palette), so when a terminal can do both,
// it wins.
TRIAL("Capabilities: kitty wins over sixel") {
    const Taped one{taped("21211", drawing, "\x1b_Gi=31;OK\x1b\\")};

    SAME(static_cast<int>(one.graphics()), static_cast<int>(Graphics::kitty));
}

TRIAL("Capabilities: the kitty answer does not come back as pressed keys") {
    const Taped one{taped("21211", plain, "\x1b_Gi=31;OK\x1b\\")};

    SAME(static_cast<int>(one.graphics()), static_cast<int>(Graphics::kitty));
    SAME(one.rest(), std::string{});
}

TRIAL("Capabilities: the cell size comes from 16t") {
    const Taped one{taped("21211", plain, "\x1b[6;20;10t")};

    SAME(one.cell().wide, 10);
    SAME(one.cell().high, 20);
}

// Zero means "unknown", not "zero pixels", and the caller is entitled to tell them apart, because
// whether to guess the cell's aspect ratio depends on it.
TRIAL("Capabilities: an unknown cell size is zero") {
    const Taped one{taped("21211", plain)};

    SAME(one.cell().wide, 0);
    SAME(one.cell().high, 0);
}

TRIAL("Capabilities: another window report is not the cell size") {
    const Taped one{taped("21211", plain, "\x1b[8;24;80t")};

    SAME(one.cell().wide, 0);
}

// --- Block characters ---------------------------------------------------------------------------
//
// The measurement answers only the question "does this character take one cell". Whether the font
// has it cannot be measured: a missing glyph is drawn as an empty rectangle and takes the same
// room. That is why `blocks()` gives a ceiling, not a choice: it says what the terminal will not
// scatter yet. `Term` makes the choice, taking the smaller of the wish and the ceiling.

TRIAL("Blocks: both characters one cell wide raise the ceiling to octants") {
    const Taped one{taped("21211", plain)};

    SAME(static_cast<int>(one.blocks()), static_cast<int>(ui::Blocks::octants));
}

TRIAL("Blocks: a misaligned sextant lowers the ceiling to block characters") {
    const Taped one{taped("21221", plain)};

    SAME(static_cast<int>(one.blocks()), static_cast<int>(ui::Blocks::basic));
}

TRIAL("Blocks: a misaligned octant lowers the ceiling to sextants") {
    const Taped one{taped("21212", plain)};

    SAME(static_cast<int>(one.blocks()), static_cast<int>(ui::Blocks::sextants));
}

// A silent terminal has disproved nothing, so the ceiling stays high. No measurement is not the
// same as a measurement saying "no".
TRIAL("Blocks: without a measurement the ceiling stays highest") {
    const Taped one{mute()};

    SAME(static_cast<int>(one.blocks()), static_cast<int>(ui::Blocks::octants));
}

TRIAL("Capabilities: the probe asks for the cell and for kitty, and the sentinel is still last") {
    const Taped one{taped("21211", plain)};

    (void)one.graphics();

    const std::string& asked = one.asked();

    OUGHT(asked.find("\x1b[16t") != std::string::npos);
    OUGHT(asked.find("\x1b_Gi=31") != std::string::npos);
    OUGHT(asked.rfind("\x1b[c") == asked.size() - 3);
}


// In tmux the outer terminal's variables guess nothing: a session started in kitty carries
// KITTY_WINDOW_ID into every window, and tmux does not pass kitty images through. TERM remains.
TRIAL("Capabilities: in tmux the outer terminal's variables guess nothing") {
    auto line = apart();

    line->environment["TMUX"] = "/tmp/tmux-1000/default,1234,0";
    line->environment["TERM"] = "tmux-256color";
    line->environment["KITTY_WINDOW_ID"] = "1";
    line->environment["WT_SESSION"] = "x";
    line->environment["TERM_PROGRAM"] = "WezTerm";

    const Taped told{line};

    SAME(static_cast<int>(told.graphics()), static_cast<int>(Graphics::none));
    SAME(static_cast<int>(told.colors()), static_cast<int>(Colors::palette));
    SAME(told.named(), std::string{"tmux-256color"});

    // screen and tmux recognized in other ways: by screen's variable, by TERM alone.
    for (const auto& [name, value] : std::vector<std::pair<std::string, std::string>>{
             {"TMUX", "/tmp/tmux-1000/default,1234,0"},
             {"STY", "1234.pts-0.host"},
             {"TERM", "screen-256color"},
             {"TERM", "tmux"}}) {
        auto other = apart();

        other->environment[name] = value;
        other->environment["KITTY_WINDOW_ID"] = "1";
        other->environment["TERM_PROGRAM"] = "WezTerm";

        const Taped muxed{other};

        SAME(static_cast<int>(muxed.graphics()), static_cast<int>(Graphics::none));
        OUGHT(muxed.named() != "WezTerm");
    }

    // The same set without tmux: the variables speak again.
    auto plain = apart();

    plain->environment["KITTY_WINDOW_ID"] = "1";
    plain->environment["TERM_PROGRAM"] = "WezTerm";

    const Taped bare{plain};

    SAME(static_cast<int>(bare.graphics()), static_cast<int>(Graphics::kitty));
    SAME(bare.named(), std::string{"WezTerm"});
}

// Over SSH the probe waits longer, because the answer comes back after a full network round trip.
TRIAL("Capabilities: over SSH the probe waits longer") {
    auto near = mute();
    auto far = mute();

    far->environment["SSH_CONNECTION"] = "10.0.0.2 50000 10.0.0.1 22";

    const Taped here{near};
    const Taped there{far};

    (void)here.colors();
    (void)there.colors();

    OUGHT(!near->patience.empty() && near->patience.front().has_value());
    OUGHT(!far->patience.empty() && far->patience.front().has_value());

    OUGHT(*near->patience.front() <= std::chrono::milliseconds{250});
    OUGHT(*far->patience.front() > std::chrono::milliseconds{900});

    auto tty = mute();

    tty->environment["SSH_TTY"] = "/dev/pts/3";

    const Taped through{tty};

    (void)through.colors();

    OUGHT(!tty->patience.empty() && tty->patience.front().has_value() &&
          *tty->patience.front() > std::chrono::milliseconds{900});
}
