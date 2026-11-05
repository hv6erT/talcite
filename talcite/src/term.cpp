#include "talcite/term.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "talcite/base64.hpp"
#include "talcite/encoding.hpp"
#include "talcite/decoder.hpp"
#include "talcite/pixels.hpp"
#include "hexing.hpp"
#include "sheet.hpp"
#include "unicode.hpp"

namespace talcite {

using detail::Placement;

namespace {

// How long one turn waits for the loop's bell while the terminal is handed over.
constexpr std::chrono::milliseconds glancing{100};

// What the terminal shows after the last frame, cell by cell. A frame compares with it only the
// rows the root painted (`Root::painted`) and takes those rows in; the others stand as they were,
// as they do on the root's sheet.
class Standing {
public:
    Standing() = default;

    // A blank screen of `size`, as after clearing it.
    explicit Standing(ui::Size size)
        : size_(size), cells_(static_cast<std::size_t>(std::max(size.wide, 0)) * static_cast<std::size_t>(std::max(size.high, 0))) {}

    // The size.
    [[nodiscard]] ui::Size size() const { return size_; }

    // The cell at `where`; an empty one outside, as `Snapshot::at` gives.
    [[nodiscard]] Cell at(ui::Spot where) const {
        if (where.x < 0 || where.y < 0 || where.x >= size_.wide || where.y >= size_.high) return Cell{};

        return cells_[static_cast<std::size_t>(where.y) * static_cast<std::size_t>(size_.wide) + static_cast<std::size_t>(where.x)];
    }

    // The rows [from, upto) of a frame of the same size, as they now stand on the screen.
    void took(const Snapshot& made, int from, int upto) {
        for (int y = std::max(from, 0); y < upto && y < size_.high; ++y)
            for (int x = 0; x < size_.wide; ++x)
                cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(size_.wide) + static_cast<std::size_t>(x)] = made.at({x, y});
    }

private:
    ui::Size size_;
    std::vector<Cell> cells_;
};

}

// The terminal's state, behind a pointer so the public header stays free of it.
struct Term::Held {
    std::unique_ptr<Loop> mine;
    Loop* loop = nullptr;

    explicit Held(Seam<Console> with) : console(with), compat(std::move(with)) {}

    // Everything that reaches the terminal goes through this seam: frames, the title, the mouse
    // and the screen, and also input, raw mode, the size and the double-click time. `Term` has not
    // a single line of system code, so tests with a fake console check all of it.
    Seam<Console> console;

    // Const, because it has no non-const method: it measures the terminal and decides nothing. It
    // asks through the same console we draw on.
    const Capabilities compat;
    Decoder decoder;

    // The application's wish. Nothing is written into `Capabilities` (it only measures), so the
    // effective value is formed at every read as wish.value_or(measurement).
    Setup wish;

    // A fresh configuration invalidates the screen image just as a change of the window size does.
    bool shifted = false;

    ui::Root* onto = nullptr;

    std::string spare;

    // How emoji sequences were drawn before this terminal, given back when it goes.
    Rendering before = rendering();

    bool running = true;
    bool entered = false;
    bool fresh = true;

    std::chrono::milliseconds twice{0};

    Standing standing;

    // The root's count of frames at the last one drawn (`Root::framed`). A frame in between, which
    // nobody drew, painted rows this one does not name, so the next frame compares the whole
    // screen.
    std::uint64_t framed = 0;

    // Kitty images: what stands on screen after the last frame, and which images the terminal holds.
    // The terminal keeps an image as long as someone on our side holds its pixels.
    std::vector<Placement> placed;
    std::vector<std::pair<std::uint32_t, std::weak_ptr<const Pixels>>> sent;

    Style now;
    std::uint32_t linked = 0;
    ui::Spot at{-1, -1};
    bool showing = false;

    // The cursor's shape the terminal was last asked for; at the start its usual one. Nothing when
    // it is not known: another program had the terminal meanwhile.
    std::optional<ui::CaretShape> shaped = ui::CaretShape{};

    int wide = 80;
    int high = 25;

    // How many times we have stepped out of the terminal's way. A counter, not a flag, because
    // nested stepping aside is allowed and means one thing: the first one leaves, the last one
    // returns. Undoing the state twice would send a second set of sequences and, for the mouse,
    // lose the intermediate state.
    int away = 0;
};

namespace {

// The squared distance between two channel values.
int apart(int one, int two) { return (one - two) * (one - two); }

// The nearest entry of the 256-color palette: the 6x6x6 cube or the grey ramp. The closer of the
// two wins, because a grey from the cube may be farther than the separate grey ladder.
int indexed(Color one) {
    // The cube's steps from the palette (`Color::indexed`): the red of numbers 16, 52, 88, 124,
    // 160, 196.
    static const std::array<int, 6> steps = [] {
        std::array<int, 6> made{};

        for (int which = 0; which < 6; ++which) made[static_cast<std::size_t>(which)] = Color::indexed(16 + 36 * which)->red;

        return made;
    }();

    const auto closest = [](int given) {
        int best = 0;

        for (int which = 1; which < 6; ++which)
            if (apart(given, steps[which]) < apart(given, steps[best])) best = which;

        return best;
    };

    const int red = closest(one.red);
    const int green = closest(one.green);
    const int blue = closest(one.blue);

    const int cube = apart(one.red, steps[red]) + apart(one.green, steps[green]) +
                     apart(one.blue, steps[blue]);

    int grey = (one.red + one.green + one.blue) / 3;
    int level = (grey - 8) / 10;

    level = level < 0 ? 0 : (level > 23 ? 23 : level);
    grey = 8 + 10 * level;

    const int plain =
        apart(one.red, grey) + apart(one.green, grey) + apart(one.blue, grey);

    if (plain < cube) return 232 + level;

    return 16 + 36 * red + 6 * green + blue;
}

// The nearest of the palette's sixteen basic colors (`Color::indexed`). The values are xterm's;
// other terminals paint them a little differently, but choosing the nearest is an approximation
// anyway.
int shortened(Color one) {
    int best = 0;
    int least = -1;

    for (int which = 0; which < 16; ++which) {
        const Color basic = *Color::indexed(which);
        const int much = apart(one.red, basic.red) + apart(one.green, basic.green) + apart(one.blue, basic.blue);

        if (least < 0 || much < least) {
            least = much;
            best = which;
        }
    }

    return best;
}

// Appends the SGR parameters for a foreground or background color at the terminal's color level:
// 24-bit, 256-color, or the sixteen basic ones. Nothing for a transparent color or a mono terminal.
void tinted(Color one, bool foreground, Colors how, std::string& into) {
    if (one.transparent || how == Colors::mono) return;

    if (how == Colors::rgb) {
        into += foreground ? ";38;2;" : ";48;2;";
        into += std::to_string(one.red) + ";" + std::to_string(one.green) + ";" +
                std::to_string(one.blue);

        return;
    }

    if (how == Colors::palette) {
        into += foreground ? ";38;5;" : ";48;5;";
        into += std::to_string(indexed(one));

        return;
    }

    const int which = shortened(one);

    if (which < 8) into += ";" + std::to_string((foreground ? 30 : 40) + which);
    else into += ";" + std::to_string((foreground ? 90 : 100) + which - 8);
}

// Appends the SGR sequence for a style, always written from a reset. Computing the difference
// between two styles would save a few bytes but cost remembering which attribute turns off what;
// the cursor jumps are the expensive part anyway, not those few characters.
void painted(const Style& what, Colors how, std::string& into) {
    into += "\x1b[0";

    if (what.bold) into += ";1";
    if (what.faint) into += ";2";
    if (what.italic) into += ";3";
    if (what.under) into += ";4";
    if (what.reverse) into += ";7";
    if (what.crossed) into += ";9";

    tinted(what.foreground, true, how, into);
    tinted(what.background, false, how, into);

    into += "m";
}

// Appends a cursor jump to `where` (zero-based).
void placed(ui::Spot where, std::string& into) {
    into += "\x1b[" + std::to_string(where.y + 1) + ";" + std::to_string(where.x + 1) + "H";
}

// The kitty graphics protocol: appends the transmission of one image. An image goes to the terminal
// once, under a number (the image mark from the cells), and after that it is only placed and
// removed. Raw pixels (f=32, RGBA) in base64, in pieces of 4096 characters; the following pieces
// carry only m and q. q=2 silences the replies that would otherwise arrive as keys. A placement
// gives columns and rows (c, r), so the terminal fits the image into the rectangle of cells even
// when we know the cell size wrong, and C=1 leaves the cursor in place, which is why with kitty the
// last screen row may hold an image.
void kitty(const Picture& one, std::string& into) {
    const std::string coded = base64(one.pixels->stills()[one.still].bytes);
    const ui::Size size = one.pixels->size();

    constexpr std::size_t piece = 4096;

    for (std::size_t at = 0;; at += piece) {
        const bool more = at + piece < coded.size();

        into += "\x1b_G";

        if (at == 0)
            into += "a=t,f=32,s=" + std::to_string(size.wide) + ",v=" + std::to_string(size.high) +
                    ",i=" + std::to_string(one.mark) + ",";

        into += more ? "q=2,m=1;" : "q=2,m=0;";
        into.append(coded, at, piece);
        into += "\x1b\\";

        if (!more) return;
    }
}

// Appends the placement of an image already sent, at its cell rectangle.
void stood(const Placement& one, std::string& into) {
    placed(one.at, into);

    into += "\x1b_Ga=p,i=" + std::to_string(one.picture.mark) + ",c=" + std::to_string(one.picture.size.wide) +
            ",r=" + std::to_string(one.picture.size.high) + ",C=1,q=2\x1b\\";
}

// Removing from the screen (d=i, the data stays) or releasing (d=I, the terminal forgets the
// image).
void unstood(std::uint32_t mark, bool forget, std::string& into) {
    into += "\x1b_Ga=d,d=";
    into += forget ? 'I' : 'i';
    into += ",i=" + std::to_string(mark) + ",q=2\x1b\\";
}

// Whether two placements are the same image at the same cell.
bool same_place(const Placement& one, const Placement& two) {
    return one.picture.mark == two.picture.mark && one.at == two.at;
}

using Sent = std::vector<std::pair<std::uint32_t, std::weak_ptr<const Pixels>>>;

// Removes and forgets every kitty image, on exit and when handing the terminal over, because the
// other program gets a clean screen and our numbers could collide with its own.
void forgotten(Console& out, std::vector<Placement>& placed, Sent& sent) {
    if (placed.empty() && sent.empty()) return;

    out.write("\x1b_Ga=d,d=A,q=2\x1b\\");

    placed.clear();
    sent.clear();
}

// The first handle of a cluster: a cell's letter from here on stands for several characters.
constexpr char32_t clustered = 0x110000;

// Appends what is written to the terminal for one cell: the UTF-8 of a character, or the text of a
// cluster.
void written(char32_t letter, std::string& into) {
    if (letter < clustered) utf8::encode(letter, into);
    else into += spelled(letter);
}

// The longest address that goes to the terminal as a hyperlink, in bytes once escaped. Terminals
// keep a link's address in their own limits and cut or drop a longer one without a word; this is
// the limit of the strictest of them (VTE), and what is longer stays plain text.
constexpr std::size_t farthest = 2083;

// The address as a hyperlink may carry it, or nothing for an empty one or one above `farthest`.
// OSC 8 takes only printable ASCII, so every other byte (a space, a control character, each byte of
// a multibyte character) goes as %XX, the way an address is written anyway; an escape or a BEL can
// thus never end the control string early.
std::optional<std::string> addressed(std::string_view address) {
    if (address.empty()) return std::nullopt;

    std::string made;

    for (const char letter : address) {
        const auto one = static_cast<unsigned char>(letter);

        if (one > 0x20 && one < 0x7F) {
            made += letter;
        } else {
            hex::escaped(one, made);
        }
    }

    if (made.size() > farthest) return std::nullopt;

    return made;
}

// The hyperlinks of a frame the terminal is given, by mark, with their addresses as OSC 8 carries
// them; none when the terminal does not take them.
using Addresses = std::map<std::uint32_t, std::string>;

// The addresses of the frame's hyperlinks (`addressed`), when links go to the terminal at all.
Addresses addresses_of(const Snapshot& made, bool linking) {
    Addresses found;

    if (!linking) return found;

    for (const Hyperlink& one : made.links())
        if (std::optional<std::string> written = addressed(one.address)) found.emplace(one.mark, std::move(*written));

    return found;
}

// The hyperlink a cell is written under: its mark when the frame has an address for it, otherwise
// none.
std::uint32_t link_of(const Cell& one, const Addresses& known) { return known.contains(one.link) ? one.link : 0; }

// Appends the switch to the hyperlink `mark` (OSC 8 with the mark as its id), or its end for zero.
// A link is a state of the terminal like the style: it covers every character written until the
// next switch, wherever the cursor jumps in between.
void linking(std::uint32_t mark, const Addresses& known, std::string& into) {
    if (mark == 0) {
        into += "\x1b]8;;\x1b\\";

        return;
    }

    into += "\x1b]8;id=" + std::to_string(mark) + ";" + known.at(mark) + "\x1b\\";
}

// The longest gap between changed cells that is rewritten rather than jumped over.
constexpr int nearby = 4;

// Rewrites the cells [from, upto) of row `y` instead of jumping over them, when that is possible;
// returns whether it did. A cursor jump costs several bytes, a character one. A short gap between
// changed cells is cheaper to rewrite than to jump over, provided the rewritten cells come out
// exactly as they already are: the same style and the same hyperlink as the current ones, no
// double-width character and nothing that is only its continuation.
bool bridged(const Snapshot& made, int y, int from, int upto, const Style& now, std::uint32_t linked, const Addresses& known,
             std::string& into) {
    if (upto - from > nearby) return false;

    for (int x = from; x < upto; ++x) {
        const Cell one = made.at({x, y});

        if (one.letter == 0 || width(one.letter) == 2) return false;

        if (!(one.style == now) || link_of(one, known) != linked) return false;
    }

    for (int x = from; x < upto; ++x) written(made.at({x, y}).letter, into);

    return true;
}

// Whether the terminal may count the cell's letter in other cells than talcite does: a cluster of
// several characters, which the terminal cuts and measures by its own rules (the conjuncts of Indic
// scripts, the vowels of Thai, the emoji sequences), a spacing mark or a prepended character, which
// a terminal following UAX #29 joins to the character beside it, unlike talcite. The terminal
// moves its cursor by its own count, so what is written after such a letter in one piece lands
// elsewhere than in its cells.
bool uncertain(char32_t letter) {
    return letter >= clustered || unicode::spacing(letter) || unicode::prepending(letter);
}

// The end of the stretch of row `y` that starts at `x`: a space alone, or a word, the cells up to
// the next space. The terminal joins and shapes characters only within a word.
int worded(const Snapshot& made, int y, int x) {
    if (made.at({x, y}).letter == U' ') return x + 1;

    int end = x + 1;

    while (end < made.size().wide && made.at({end, y}).letter != U' ') ++end;

    return end;
}

// Whether a cell in [from, upto) of row `y` has a letter the terminal may count otherwise.
bool unsure(const Snapshot& made, int y, int from, int upto) {
    for (int x = from; x < upto; ++x)
        if (uncertain(made.at({x, y}).letter)) return true;

    return false;
}

// Whether a cell in [from, upto) of row `y` differs from what the terminal has.
bool altered(const Snapshot& made, const Standing& standing, int y, int from, int upto) {
    for (int x = from; x < upto; ++x)
        if (!(made.at({x, y}) == standing.at({x, y}))) return true;

    return false;
}

// The settings the terminal cannot guess from a frame: it has to be told them with a sequence and
// told to undo them with another. `Capabilities` has no setter for them, so the effective value is
// formed here.
struct Sending {
    Screen screen = Screen::none;
    Mouse mouse = Mouse::none;
    bool hover = false;
    bool bracketed_paste = false;
};

// The effective screen, mouse, hover and bracketed-paste settings. Tracking motion without a pressed
// button has nothing to measure in the terminal: no reply says whether ?1003 is understood. So the
// wish alone decides, and the default is off, because when on, it makes the terminal report every
// twitch of the pointer, even when nobody cares, and over ssh that is real traffic on the link.
Sending sending(const Capabilities& compat, const Setup& wish) {
    Sending made;

    made.screen = wish.screen.value_or(compat.screen());
    made.mouse = wish.mouse.value_or(compat.mouse());
    made.hover = wish.hover.value_or(false);
    made.bracketed_paste = wish.bracketed_paste.value_or(compat.bracketed_paste());

    return made;
}

// The effective block-glyph density. The only field where the wish does not win outright:
// `Capabilities::blocks()` returns a ceiling, the density the terminal does not yet break up. A glyph
// wider than a cell breaks `Sheet`, mouse hits and writing the difference, so the smaller of the
// two is taken.
ui::Blocks blocked(const Capabilities& compat, const Setup& wish) {
    const ui::Blocks want = wish.blocks.value_or(ui::Blocks::basic);
    const ui::Blocks most = compat.blocks();

    return want < most ? want : most;
}

// Whether frames are wrapped in synchronized output (mode 2026).
bool synchronized(const Capabilities& compat, const Setup& wish) {
    return wish.synchronized.value_or(compat.synchronized());
}

// How this terminal draws emoji sequences, from the probe's widths: a programmer (woman, ZWJ,
// laptop) two columns wide means it joins the sequence, a red heart with the emoji selector two
// columns wide means the selector widens. Without an answer the setting stays as it was.
Rendering drawn(const Capabilities& compat, Rendering was) {
    Rendering made = was;

    if (compat.joined() > 0) made.joined = compat.joined() <= 2;

    if (compat.varied() > 0) made.presented = compat.varied() >= 2;

    return made;
}

// Whether hyperlinks go to the terminal.
bool links(const Capabilities& compat, const Setup& wish) { return wish.links.value_or(compat.links()); }

// Whether the window title may be set.
bool titles(const Capabilities& compat, const Setup& wish) {
    return wish.titles.value_or(compat.titles());
}

// Turns mouse reporting on. ?1003 is added to tracking that is already on, and only when it is on:
// without mouse reporting at all nobody would send that motion. It is turned off first (in
// `unmoused`), because it covers what ?1002 does; turned off last, it would leave the terminal with
// the wider tracking for a moment.
//
// A terminal without SGR (1006) gets drags (1002) too, or selecting with the mouse would not work
// at all, and the urxvt encoding (1015), which writes cells in decimal: one that knows it reports
// past the 223rd column, where X10 cannot, and one that does not ignores the request.
void mousing(Console& out, Mouse given, bool hover) {
    if (given == Mouse::sgr) out.write("\x1b[?1000h\x1b[?1002h\x1b[?1006h");
    else if (given == Mouse::basic) out.write("\x1b[?1000h\x1b[?1002h\x1b[?1015h");

    if (given != Mouse::none && hover) out.write("\x1b[?1003h");
}

// Turns mouse reporting off, in the reverse order of `mousing`.
void unmoused(Console& out, Mouse given, bool hover) {
    if (given != Mouse::none && hover) out.write("\x1b[?1003l");

    if (given == Mouse::sgr) out.write("\x1b[?1006l\x1b[?1002l\x1b[?1000l");
    else if (given == Mouse::basic) out.write("\x1b[?1015l\x1b[?1002l\x1b[?1000l");
}

// Switches to the alternate screen (1049, or the older 47).
void screened(Console& out, Screen given) {
    if (given == Screen::apart) out.write("\x1b[?1049h");
    else if (given == Screen::older) out.write("\x1b[?47h");
}

// Switches back from the alternate screen.
void unscreened(Console& out, Screen given) {
    if (given == Screen::apart) out.write("\x1b[?1049l");
    else if (given == Screen::older) out.write("\x1b[?47l");
}

// The sequence that asks the terminal for a cursor's shape (DECSCUSR, `CSI Ps SP q`): 0 for its
// usual one, 1 and 2 a block, 3 and 4 an underline, 5 and 6 a bar, the odd ones blinking.
std::string shaping(ui::CaretShape shape) {
    int number = 0;

    switch (shape.form) {
        case ui::Caret::usual: number = 0; break;
        case ui::Caret::block: number = shape.blink ? 1 : 2; break;
        case ui::Caret::underline: number = shape.blink ? 3 : 4; break;
        case ui::Caret::bar: number = shape.blink ? 5 : 6; break;
    }

    return "\x1b[" + std::to_string(number) + " q";
}

// The terminal's usual cursor back, when it was asked for another one; afterwards the shape is
// not known, since whoever takes the terminal may change it.
void unshaped(Console& out, std::optional<ui::CaretShape>& shaped) {
    if (shaped != ui::CaretShape{}) out.write(shaping(ui::CaretShape{}));

    shaped.reset();
}

// Dressing the terminal: the screen, a hidden cursor, no autowrap, a cleared screen, the mouse and
// bracketed paste. Dressing and undressing are two halves of one thing, so they stand side by side
// and have one place: the constructor, the destructor and stepping aside for another program all
// use them. Written out in three places, they would drift apart at the first change.
void dressed(Console& out, const Sending& now) {
    screened(out, now.screen);

    out.write("\x1b[?25l\x1b[?7l\x1b[2J\x1b[H");

    mousing(out, now.mouse, now.hover);

    if (now.bracketed_paste) out.write("\x1b[?2004h");
}

// Undressing the terminal, in the reverse order of `dressed`.
void undressed(Console& out, const Sending& now) {
    if (now.bracketed_paste) out.write("\x1b[?2004l");

    unmoused(out, now.mouse, now.hover);

    out.write("\x1b[?7h\x1b[?25h");

    unscreened(out, now.screen);
}

// Sends only the difference between two sets of settings, not the whole: repeating ?1049h
// unconditionally on a terminal already on the alternate screen would cost a flicker, and for the
// mouse it would lose the intermediate state.
void shifting(Console& out, const Sending& was, const Sending& now) {
    if (was.bracketed_paste != now.bracketed_paste) out.write(now.bracketed_paste ? "\x1b[?2004h" : "\x1b[?2004l");

    if (was.mouse != now.mouse || was.hover != now.hover) {
        unmoused(out, was.mouse, was.hover);
        mousing(out, now.mouse, now.hover);
    }

    if (was.screen != now.screen) {
        unscreened(out, was.screen);
        screened(out, now.screen);

        out.write("\x1b[?25l\x1b[?7l\x1b[2J\x1b[H");
    }
}

// Reads the size from the console; true when it changed. A console that does not know its size (a
// pipe, a test) keeps the previous one, 80x25 at the start.
bool measured(const Console& from, int& wide, int& high) {
    const std::optional<ui::Size> now = from.size();

    if (!now.has_value() || now->wide <= 0 || now->high <= 0) return false;

    if (now->wide == wide && now->high == high) return false;

    wide = now->wide;
    high = now->high;

    return true;
}

// The writing of one frame's cells (`made`) into `into`, keeping the terminal's style, cursor and
// open link (`now`, `at`, `linked`) as they go; links the terminal is not given (`known`) stay
// plain text.
struct Pen {
    const Snapshot& made;
    const Addresses& known;
    const Colors& palette;
    std::string& into;
    Style& now;
    ui::Spot& at;
    std::uint32_t& linked;

    // Brings the cursor to (x, y): rewriting a short stretch of cells when that is cheaper than a
    // jump, otherwise a jump.
    void reached(int x, int y) {
        if (at == ui::Spot{x, y}) return;

        if (at.y != y || at.x < 0 || at.x > x || !bridged(made, y, at.x, x, now, linked, known, into)) placed({x, y}, into);

        at = ui::Spot{x, y};
    }

    // Switches the terminal to the style, unless it has it already.
    void dressed(const Style& style) {
        if (style == now) return;

        painted(style, palette, into);

        now = style;
    }

    // Writes the cell at (x, y), where the cursor stands, after switching the style and the link
    // where they differ from the terminal's.
    void lettered(int x, int y) {
        const Cell one = made.at({x, y});

        dressed(one.style);

        if (const std::uint32_t link = link_of(one, known); link != linked) {
            linking(link, known, into);

            linked = link;
        }

        written(one.letter, into);

        const bool broad = x + 1 < made.size().wide && made.at({x + 1, y}).letter == 0;

        at.x += broad ? 2 : 1;

        // Autowrap is off, so at the last column the cursor stands in an undefined place; the
        // next character gets its own jump.
        if (at.x >= made.size().wide) at = ui::Spot{-1, -1};
    }

    // Row `y` goes out by stretches: spaces and words. A word with a letter the terminal may count
    // otherwise (`uncertain`) goes out whole whenever any of its cells changed, because the
    // terminal joins its characters by its own rules: its cells are erased first (ECH), so that
    // nothing of the previous picture stays where the terminal draws it narrower, and after it the
    // cursor jumps to the next cell instead of trusting the terminal's count. A terminal that drew
    // it wider covered the cells after it, so once such a word went out, the rest of the row goes
    // out again too (`covered`, the column from which it did; the row's width means nowhere).
    // Elsewhere only the cells that differ from what stands go.
    void rowed(int y, const Standing& standing, int& covered) {
        bool covering = false;

        for (int x = 0; x < made.size().wide;) {
            const int end = worded(made, y, x);

            if (unsure(made, y, x, end)) {
                if (covering || altered(made, standing, y, x, end)) {
                    reached(x, y);

                    // ECH erases in the background of the current style, so the word's own goes
                    // first.
                    dressed(made.at({x, y}).style);

                    into += "\x1b[" + std::to_string(end - x) + "X";

                    for (int on = x; on < end; ++on)
                        if (made.at({on, y}).letter != 0) lettered(on, y);

                    if (!covering) covered = x;

                    covering = true;
                }

                // Where the terminal's cursor stands after the word is not known, and the word
                // must not be rewritten as a mere gap either.
                at = ui::Spot{-1, -1};
                x = end;

                continue;
            }

            for (; x < end; ++x) {
                const Cell one = made.at({x, y});

                // The continuation of a wide character went out together with its first half.
                if (one.letter == 0) continue;

                if (!covering && one == standing.at({x, y})) continue;

                reached(x, y);
                lettered(x, y);
            }
        }
    }
};

// The images after the cells: the sixels to send (`sixels`), and the kitty images standing now
// (`kitties`) that did not stand so after the last frame (`before`), each sent first when the
// terminal does not hold its pixels yet (`sent`). After an image the cursor stands wherever the
// terminal put it; the next character gets its own jump. Kitty's C=1 leaves it in place, but a
// terminal that does not know the key would move it, so the same holds there.
void pictured_out(const std::vector<Placement>& sixels, const std::vector<Placement>& kitties, const std::vector<Placement>& before,
                  std::vector<std::pair<std::uint32_t, std::weak_ptr<const Pixels>>>& sent, ui::Spot& at, std::string& into) {
    for (const Placement& one : sixels) {
        if (!one.picture.sixel) continue;

        placed(one.at, into);

        into += *one.picture.sixel;

        at = ui::Spot{-1, -1};
    }

    for (const Placement& one : kitties) {
        if (std::any_of(before.begin(), before.end(), [&one](const Placement& was) { return same_place(was, one); })) continue;

        if (std::none_of(sent.begin(), sent.end(), [&one](const auto& was) { return was.first == one.picture.mark; })) {
            kitty(one.picture, into);

            sent.emplace_back(one.picture.mark, one.picture.pixels);
        }

        stood(one, into);

        at = ui::Spot{-1, -1};
    }
}

// The cursor after a frame: where the root wants it (`Root::caret`), in its shape, shown; hidden
// when the root wants none. The shape goes before the cursor shows, so it never shows in the shape
// of the focus before. We ask after composing, because only then are the rectangles fresh.
void careted(const ui::Root& root, std::optional<ui::CaretShape>& shaped, bool& showing, ui::Spot& at, std::string& into) {
    if (const std::optional<ui::Spot> spot = root.caret(); spot.has_value()) {
        placed(*spot, into);

        at = *spot;

        if (const ui::CaretShape shape = root.caret_shape(); shaped != shape) {
            into += shaping(shape);

            shaped = shape;
        }

        if (!showing) {
            into += "\x1b[?25h";

            showing = true;
        }
    } else if (showing) {
        into += "\x1b[?25l";

        showing = false;
    }
}

}

// Takes over the terminal: raw mode, the probe of its capabilities, then the dressing (alternate
// screen, hidden cursor, mouse) according to the wish.
Term::Term(Setup given, Seam<Console> with) : held_(std::make_unique<Held>(std::move(with))) {
    Held& held = *held_;

    held.wish = std::move(given);

    // The loop belongs to the thread and there can be only one. If it exists, we adopt it; if not,
    // we make our own, because without it `Term` has nothing to wait on.
    if (Loop* standing = Loop::current()) {
        held.loop = standing;
    } else {
        held.mine = std::make_unique<Loop>();
        held.loop = held.mine.get();
    }

    Console& console = held.console.provider();

    console.raw(true);

    held.twice = console.double_click();

    // The probe has to happen on a prepared terminal, but before entering the alternate screen;
    // otherwise the probe characters would flicker on the application's clean screen.
    (void)held.compat.colors();

    held.spare = held.compat.rest();

    held.before = rendering();

    rendered(drawn(held.compat, held.before));

    dressed(console, sending(held.compat, held.wish));

    held.entered = true;

    // The size only after entering the alternate screen: on Windows it is a different buffer, and
    // we measure the one we draw on.
    (void)measured(console, held.wide, held.high);
}

// Gives the terminal back. We clean up in reverse order, however we got here. A terminal left in
// raw mode without a cursor is unusable, and the user has no way to undo it.
Term::~Term() {
    Held& held = *held_;

    // The current state is undone, not the initial one: after `reconfigure` the mouse or the
    // alternate screen may stand differently than in the constructor, and what has to be turned
    // off is what is really on.
    //
    // When the terminal is handed to someone else right now, there is nothing to undo: `Pause`
    // removed both the dressing and raw mode on entry.
    rendered(held.before);

    if (held.away > 0) return;

    forgotten(held.console.provider(), held.placed, held.sent);

    if (held.entered) {
        unshaped(held.console.provider(), held.shaped);

        undressed(held.console.provider(), sending(held.compat, held.wish));
    }

    held.console.provider().raw(false);
}

// Stepping out of the way. We undo exactly what the constructor set up, in reverse order: first the
// sequences, then the console mode. The program that starts now finds the terminal as we found it:
// with echo, with Enter, with Ctrl-C, on the buffer we started on.
//
// A class rather than a pair of public methods, because a pair of methods is a contract the
// compiler does not enforce: one could return without leaving, or leave and never return. Here
// leaving and returning are one thing, so the return happens even when an exception flies on the
// way. The nested class reaches `Held` without any friend, and the public surface of `Term` grows by
// one notion, not two verbs.
Term::Pause::Pause(Term& given) : term_(given) {
    Held& held = *term_.held_;

    if (held.away++ > 0) return;

    forgotten(held.console.provider(), held.placed, held.sent);

    if (held.entered) {
        unshaped(held.console.provider(), held.shaped);

        undressed(held.console.provider(), sending(held.compat, held.wish));
    }

    held.console.provider().raw(false);
}

// Takes the terminal back: raw mode and the dressing again, and a full repaint at the next frame.
Term::Pause::~Pause() {
    Held& held = *term_.held_;

    if (--held.away > 0) return;

    Console& console = held.console.provider();

    console.raw(true);

    if (held.entered) dressed(console, sending(held.compat, held.wish));

    // After the other program nothing of our frame is left on screen, and with `Screen::none` there
    // is no alternate screen that could have kept it. To `draw()`, `shifted` means exactly what a size change
    // means: compare with a blank sheet, that is, repaint everything. The dressing hid the cursor
    // and nobody knows where the other program left it, so the next frame shows it and places it
    // anew; otherwise the editor would go on without a visible cursor.
    held.shifted = true;
    held.fresh = true;
    held.showing = false;
    held.at = ui::Spot{-1, -1};

    // The size may have changed while we were away; the changes went to someone else then.
    (void)measured(console, held.wide, held.high);
}

// The event loop the terminal waits on.
Loop& Term::loop() const { return *held_->loop; }

// Applies a new wish. The change goes through the loop, not at once. The call may come from any
// thread (from a settings window, from a loaded configuration file), and switching the terminal in
// the middle of composing a frame would give half a picture in the old grid and half in the new.
// The loop runs pending work in `turn()`, right before drawing, so the change always comes in at a
// frame boundary and always on the right thread.
//
// The pending call holds a raw pointer to the state of `Term`, so destroying it with a
// `reconfigure` still queued is the caller's error, as with any other `Loop::post` that reaches an
// object.
void Term::reconfigure(Setup given) {
    held_->loop->post([held = held_.get(), given = std::move(given)]() mutable {
        const Sending was = sending(held->compat, held->wish);

        held->wish = std::move(given);

        shifting(held->console.provider(), was, sending(held->compat, held->wish));

        // Another screen hides the cursor; the next frame shows and places it again.
        held->showing = false;
        held->at = ui::Spot{-1, -1};

        if (held->onto != nullptr) {
            held->onto->blocks(blocked(held->compat, held->wish));
            held->onto->cell(held->wish.cell.value_or(held->compat.cell()));
            held->onto->colors(held->wish.colors.value_or(held->compat.colors()));
            held->onto->unicode(held->wish.unicode.value_or(held->compat.unicode()));
            held->onto->graphics(held->wish.graphics.value_or(held->compat.graphics()));
        }

        held->shifted = true;
    });
}

// The effective color level: the wish, else the measurement.
Colors Term::colors() const { return held_->wish.colors.value_or(held_->compat.colors()); }

// The effective mouse protocol: the wish, else the measurement.
Mouse Term::mouse() const { return held_->wish.mouse.value_or(held_->compat.mouse()); }

// Whether the background is dark or light: the wish, else the measurement.
Shade Term::shade() const { return held_->wish.shade.value_or(held_->compat.shade()); }

// The effective graphics protocol: the wish, else the measurement.
Graphics Term::graphics() const {
    return held_->wish.graphics.value_or(held_->compat.graphics());
}

// The effective block-glyph density, capped by what the terminal can show.
ui::Blocks Term::blocks() const { return blocked(held_->compat, held_->wish); }

// The cell size in pixels: the wish, else the measurement.
ui::Size Term::cell() const { return held_->wish.cell.value_or(held_->compat.cell()); }

// Whether Unicode is shown: the wish, else the measurement.
bool Term::unicode() const { return held_->wish.unicode.value_or(held_->compat.unicode()); }

// Shows `given` as the root. The facts about the cell are left on the root at once, because the
// image widget reads them from there when painting, and the first frame may be drawn before
// anything else happens.
//
// The root can be switched while running: this is how screens switch (panels, editor, viewer), and
// the key that switches arrives in the middle of delivery. The rest of the chunk read goes to the
// new root, because `fed` takes the current root for every byte. Each root keeps its own focus and
// its own wait for a second click, so a click on the old screen and a click on the new one do not
// add up to a double click, and returning finds the focus where it was left. The root gets the
// double-click window from here, from the system setting, together with the other facts about the
// terminal.
void Term::show(ui::Root& given) {
    held_->onto = &given;

    given.double_click(held_->twice);
    given.blocks(blocks());
    given.cell(cell());
    given.colors(colors());
    given.unicode(unicode());
    given.graphics(graphics());
}

// The root being shown, or null before `show`.
ui::Root* Term::shown() const { return held_->onto; }

// The terminal's size in cells, as last measured.
ui::Size Term::size() const { return {held_->wide, held_->high}; }

// Sets the window title (OSC 0), when titles are enabled.
void Term::title(std::string_view said) {
    if (!titles(held_->compat, held_->wish)) return;

    std::string made = "\x1b]0;";
    made += said;
    made += "\x1b\\";

    held_->console.provider().write(made);
}

// Draws a frame. The root paints what changed on the sheet it keeps (`Root::frame`), and only the
// difference from the screen goes to the terminal, in a single write: a visible half frame is worse
// than a frame late by a microsecond. Throws std::logic_error when no root is shown.
void Term::draw() {
    Held& held = *held_;

    if (held.onto == nullptr)
        throw std::logic_error("talcite::Term: no root set");

    // The screen now belongs to another program. Drawing on it would be writing over someone
    // else's picture, and our memory of the frame is out of date anyway.
    if (held.away > 0) return;

    const std::uint64_t before = held.framed;
    const detail::Sheet& sheet = held.onto->frame({held.wide, held.high});

    held.framed = held.onto->framed();

    const ui::Size grid = cell();
    const Graphics kind = graphics();

    // The root's own sheet, read as it is. Only with pictures to find whole (`bared`) a copy, since
    // that takes the marks off the others, and the root's sheet must stay as it painted it.
    const bool imaged = !sheet.pictures().empty() && ((kind == Graphics::sixel && grid.wide > 0 && grid.high > 0) || kind == Graphics::kitty);

    std::optional<detail::Sheet> bare;

    if (imaged) bare.emplace(sheet);

    const detail::Sheet& made = bare.has_value() ? *bare : sheet;

    std::string into;

    // The rows compared with the screen: those the root painted in this frame. The rest of its
    // sheet is what the screen already shows.
    std::vector<std::pair<int, int>> rows = held.onto->painted();

    // A size change invalidates the previous frame: after clearing the screen we compare with a
    // blank sheet, so everything comes out anew.
    //
    // A fresh configuration does exactly the same, and that is what forcing a full repaint is. The
    // root's sheet always holds the whole picture, however little of it was painted this frame; the
    // only memory of what the screen shows is `standing`, and these lines take it away.
    const bool reset = held.shifted || !(held.standing.size() == made.size());

    if (reset) {
        // Kitty images do not belong to the text, so clearing the screen does not remove them; we
        // do it ourselves. The data stays: the repaint places them without sending them again.
        if (!held.placed.empty()) into += "\x1b_Ga=d,d=a,q=2\x1b\\";

        held.placed.clear();

        into += "\x1b[0m\x1b[2J";

        held.standing = Standing{made.size()};
        held.now = Style{};
        held.at = ui::Spot{-1, -1};

        held.shifted = false;
    }

    // Images in pixels (sixel). The frame already knows which images are visible whole (`bared`):
    // their cells are empty and carry the image mark. The last screen row stays for characters,
    // because after an image the terminal moves the cursor below it, and below the last row there
    // is only scrolling. An image goes to the terminal when any of its cells changed in this frame
    // (new, moved, another animation frame, uncovered, the screen cleared) or went out again with
    // the rest of a row. The cells go first: rewriting them erases the pixels underneath, and the
    // image is laid on a clean area.
    std::vector<Placement> pictured;
    std::vector<Placement> standing;

    if (imaged && kind == Graphics::sixel)
        pictured = bare->bared({{0, 0}, {made.size().wide, made.size().high - 1}});

    // Kitty works differently: text does not erase an image, so what decides is not a change of
    // cells but a comparison with what stands. Whatever vanished or moved is removed before the
    // cells; an image whose pixels nobody holds any more is forgotten by the terminal. New ones are
    // placed after the cells, first sending those the terminal does not have yet. Forgetting comes
    // last, when the memory of the previous frame (`standing`, `placed`) no longer holds the
    // pixels; otherwise the image would leave the terminal one frame later.
    if (imaged && kind == Graphics::kitty) {
        standing = bare->bared({{0, 0}, made.size()});

        std::erase_if(standing, [](const Placement& one) {
            return !one.picture.pixels || one.picture.still >= one.picture.pixels->stills().size();
        });
    }

    for (const Placement& one : held.placed)
        if (std::none_of(standing.begin(), standing.end(), [&one](const Placement& now) { return same_place(one, now); }))
            unstood(one.picture.mark, false, into);

    const Colors palette = colors();

    // The frame's hyperlinks by mark. A cell under a link the terminal is not given is written as
    // plain text, and so is every cell when the terminal takes no links.
    const Addresses known = addresses_of(made, links(held.compat, held.wish));

    Pen pen{made, known, palette, into, held.now, held.at, held.linked};

    // The column of each row from which it went out again (`Pen::rowed`); the row's width means
    // nowhere.
    std::vector<int> covered(static_cast<std::size_t>(made.size().high), made.size().wide);

    // After a reset, after a frame nobody drew (its rows are not among these), and with pictures,
    // the whole screen: whether a picture is seen whole depends on all its rows, and so do the
    // cells `bared` empties, whichever rows were painted.
    if (reset || held.framed != before + 1 || imaged) rows.assign(1, {0, made.size().high});

    for (const auto& [from, upto] : rows)
        for (int y = from; y < upto; ++y) pen.rowed(y, held.standing, covered[static_cast<std::size_t>(y)]);

    std::erase_if(pictured, [&held, &made, &covered](const Placement& one) {
        for (int y = one.at.y; y < one.at.y + one.picture.size.high; ++y)
            for (int x = one.at.x; x < one.at.x + one.picture.size.wide; ++x)
                if (!(made.at({x, y}) == held.standing.at({x, y})) || x >= covered[static_cast<std::size_t>(y)])
                    return false;

        return true;
    });

    // No link stays open past the frame: whatever reaches the terminal outside a frame (a title,
    // a copy through OSC 52, another program after stepping aside, the shell after the end) must
    // not land inside one. The next frame opens it again where its cells need it.
    if (held.linked != 0) {
        linking(0, known, into);

        held.linked = 0;
    }

    pictured_out(pictured, standing, held.placed, held.sent, held.at, into);

    held.placed = std::move(standing);

    // The cursor comes from the root, not from the sheet. The frame says what to draw, not where
    // the caret stands; that belongs to the focus.
    careted(*held.onto, held.shaped, held.showing, held.at, into);

    for (const auto& [from, upto] : rows) held.standing.took(made, from, upto);

    std::erase_if(held.sent, [&into](const auto& one) {
        if (!one.second.expired()) return false;

        unstood(one.first, true, into);

        return true;
    });

    if (!into.empty()) {
        // Drawing is held for the duration of the frame if the terminal can do that; then even a
        // large change appears whole at once.
        Console& out = held.console.provider();

        if (synchronized(held.compat, held.wish)) out.write("\x1b[?2026h");

        out.write(into);

        if (synchronized(held.compat, held.wish)) out.write("\x1b[?2026l");
    }
}

namespace {

// Delivers what the decoder heard, each value to the root shown at that moment: a key in the
// middle of a read may switch the root (`Term::show`), and keys typed ahead must go to the new one.
void delivered(const std::vector<Heard>& heard, ui::Root* const& onto) {
    for (const Heard& one : heard) (void)dispatched(one, *onto);
}

}

// Asks `run` to end after the current turn, ringing the loop's bell to wake it.
void Term::stop() {
    held_->running = false;

    held_->loop->bell().ring();
}

// Turns until `stop`.
void Term::run() {
    while (turn()) {
    }
}

// One turn: waits for input, the loop's bell or a deadline, delivers what came, and draws when
// something happened. Returns false once `stop` was called. Throws std::logic_error when no root is
// shown.
bool Term::turn() {
    Held& held = *held_;

    if (held.onto == nullptr)
        throw std::logic_error("talcite::Term: no root set");

    // The terminal is handed over: input belongs to the other program, the screen to whatever it
    // writes there. Reading stdin would steal its key, and drawing would smear its picture; only
    // the loop remains, so that timers, the network and file reads can finish their work. We wait
    // on the bell, not in an empty turn, because `run()` would then spin hot.
    if (held.away > 0) {
        (void)held.loop->bell().wait(glancing);
        (void)held.loop->turn();

        return held.running;
    }

    // Keys pressed during the probe have waited since start-up. They go first, before anything
    // fresh; otherwise they would be lost despite all the work done to keep them.
    bool doing = false;

    if (!held.spare.empty()) {
        const std::string given = std::move(held.spare);
        held.spare.clear();

        delivered(held.decoder.fed(given, std::chrono::steady_clock::now()), held.onto);

        doing = true;
    }

    // The first frame is drawn before waiting, because the wait may have no limit; otherwise the
    // program would stand empty until the first key press.
    if (held.fresh) {
        held.fresh = false;

        draw();
    }

    // No limit while there is nothing to settle. The decoder says when it wants to settle what it
    // holds (a lone Esc, a paste without its end); the wait is rounded up, so the turn does not wake
    // a fraction of a millisecond too early and go round once more for nothing. A click waiting for
    // its second one does not wake the loop: the root settles it by the time of the second when
    // that comes, and when it does not come, there is nothing to settle.
    std::optional<std::chrono::milliseconds> patience;

    if (const std::optional<Clock::Moment> settling = held.decoder.due()) {
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(*settling - std::chrono::steady_clock::now());

        patience = std::max(left, std::chrono::milliseconds{0});
    }

    // The root's schedule may have an appointment, the nearest of which bounds the wait as well; the
    // tree is not asked, because whoever needs time has made an appointment with the schedule
    // (`ui::Schedule::after`, `ui::Schedule::every`). The wait is measured by the schedule's own
    // time.
    if (const std::optional<Clock::Moment> next = held.onto->schedule().next()) {
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(*next - held.onto->schedule().now());
        const std::chrono::milliseconds soon = std::max(left, std::chrono::milliseconds{0});

        if (!patience.has_value() || soon < *patience) patience = soon;
    }

    // We wait for the keyboard and the loop's bell at once. The console also returns empty after a
    // size change (how it learns of that is its own business), so when idle there is not a single
    // needless wake-up on this side.
    Console& console = held.console.provider();

    const std::string given = console.hear(patience, &held.loop->bell());

    // Resumed after a stop: meanwhile the terminal belonged to the shell, which removed our
    // dressing and wrote on the screen. We dress it again and redraw everything, as after another
    // program, cursor included.
    if (console.resumed()) {
        if (held.entered) dressed(console, sending(held.compat, held.wish));

        held.shifted = true;
        held.showing = false;
        held.shaped.reset();
        held.at = ui::Spot{-1, -1};
        doing = true;
    }

    const bool spoken = !given.empty();

    const Clock::Moment now = std::chrono::steady_clock::now();

    if (spoken) delivered(held.decoder.fed(given, now), held.onto);

    // The size is checked at every turn, not only after an empty read: a turn also happens after a
    // key and after the loop's bell, and the window may have changed just then.
    const bool sized = measured(console, held.wide, held.high);

    // The decoder's time has come: the Esc was alone, or the paste lost its end. The clock decides,
    // not an empty read: the loop's bell also ends the wait early, and a paste must not be cut
    // short because some download finished.
    if (const std::optional<Clock::Moment> settling = held.decoder.due(); settling.has_value() && now >= *settling) {
        delivered(held.decoder.lapsed(now), held.onto);

        doing = true;
    }

    if (spoken) doing = true;

    if (sized) {
        ui::SizeEvent what{held.wide, held.high};

        (void)held.onto->dispatch(what);

        doing = true;
    }

    // What the schedule has due by now is called, and a call changes what is on the screen, or asks
    // for a frame for a node that computes its picture from the time.
    if (held.onto->elapse()) doing = true;

    // Work from the loop is an event too. A callback moved over by `here()` or `Loop::post` reaches
    // the node tree for exactly that purpose, so without this the change would show only after the
    // next key, and a progress bar would not move at all.
    if (held.loop->turn() > 0) doing = true;

    // Empty turns, in which only the limit passed, cost nothing. When something happened we compose
    // a frame; if nothing changed even so, not a byte goes to the terminal.
    if (doing) draw();

    return held.running;
}

}
