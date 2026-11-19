#include "tal/app.hpp"

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

#include "tal/crypto.hpp"
#include "tal/notify.hpp"
#include "tal/port.hpp"

#include "decoder.hpp"
#include "hexing.hpp"
#include "palette.hpp"
#include "telling.hpp"
#include "unicode.hpp"

namespace tal {

namespace {

// A picture seen whole at `at`, with what it shows.
struct Placement {
    Spot at;
    Picture picture;
};

// Whether two placements are the same picture at the same cell.
bool same_place(const Placement& one, const Placement& two) { return one.picture.mark == two.picture.mark && one.at == two.at; }

// A frame as it goes to the terminal: the document's surface, with the pictures it shows whole in
// pixels. A picture is seen whole when every cell of its rectangle carries its mark and no cell
// elsewhere does, and the rectangle lies within `room`; its cells then go out blank, with the paper
// they had and the picture's mark, so that they never equal an ordinary cell and comparing frames
// sees every change between the picture and characters. A picture covered in even one cell (by a
// window, a menu, a shadow, the edge of a scroll) stays in characters, and its cells lose the mark,
// for the same reason. Without pictures in pixels every cell goes out as it is, without a mark.
class Frame {
public:
    Frame(const Surface& made, Graphics kind, Area room) : made_(made), bare_(cells(made.size())) {
        if (kind == Graphics::none) return;

        struct Found {
            Area bounds;
            std::size_t many = 0;
        };

        std::vector<std::pair<std::uint32_t, Found>> found;
        const Size size = made.size();

        for (int y = 0; y < size.high; ++y)
            for (int x = 0; x < size.wide; ++x) {
                const std::uint32_t mark = made.at({x, y}).picture;

                if (mark == 0) continue;

                auto one = std::find_if(found.begin(), found.end(), [mark](const auto& known) { return known.first == mark; });

                if (one == found.end()) one = found.insert(found.end(), {mark, Found{Area{{x, y}, {1, 1}}, 0}});

                Area& bounds = one->second.bounds;
                const int right = std::max(bounds.at.x + bounds.size.wide, x + 1);
                const int left = std::min(bounds.at.x, x);

                bounds = Area{{left, bounds.at.y}, {right - left, y + 1 - bounds.at.y}};
                ++one->second.many;
            }

        pictured_ = !found.empty();

        for (const auto& [mark, one] : found) {
            const Picture* const shown = made.picture(mark);

            if (shown == nullptr || one.bounds.size != shown->size) continue;

            if (one.many != static_cast<std::size_t>(shown->size.wide) * static_cast<std::size_t>(shown->size.high)) continue;

            if (one.bounds.common(room) != one.bounds) continue;

            if (kind == Graphics::kitty && (!shown->pixels || shown->still >= shown->pixels->stills().size())) continue;

            for (int y = one.bounds.at.y; y < one.bounds.at.y + one.bounds.size.high; ++y)
                for (int x = one.bounds.at.x; x < one.bounds.at.x + one.bounds.size.wide; ++x) bare_[index({x, y})] = mark;

            placed_.push_back(Placement{one.bounds.at, *shown});
        }
    }

    // The size.
    [[nodiscard]] Size size() const { return made_.size(); }

    // The cell at `where` as it goes out: blank with its paper and the mark where a picture shows
    // whole, otherwise the surface's cell without a picture.
    [[nodiscard]] Cell at(Spot where) const {
        const Cell& one = made_.at(where);

        if (const std::uint32_t mark = bare_[index(where)]; mark != 0) {
            Cell made{" ", Look{}, mark, 0};

            made.look.paper = one.look.paper;

            return made;
        }

        if (one.picture == 0) return one;

        Cell made = one;

        made.picture = 0;

        return made;
    }

    // The address of the link `link`.
    [[nodiscard]] std::string_view address(std::uint32_t link) const { return made_.address(link); }

    // Whether a cell shows a picture, seen whole or not, where pictures go in pixels.
    [[nodiscard]] bool pictured() const { return pictured_; }

    // The pictures seen whole, in the order their first cells come.
    [[nodiscard]] const std::vector<Placement>& placed() const { return placed_; }

private:
    // Room for a mark per cell of `size`.
    static std::vector<std::uint32_t> cells(Size size) { return std::vector<std::uint32_t>(static_cast<std::size_t>(size.wide) * static_cast<std::size_t>(size.high), 0); }

    // The place of the cell `where` among them.
    [[nodiscard]] std::size_t index(Spot where) const {
        return static_cast<std::size_t>(where.y) * static_cast<std::size_t>(made_.size().wide) + static_cast<std::size_t>(where.x);
    }

    const Surface& made_;
    std::vector<std::uint32_t> bare_;
    std::vector<Placement> placed_;
    bool pictured_ = false;
};

// What the terminal shows after the last frame, cell by cell, with the address of each link it
// shows. A frame compares with it the rows the document changed and takes those rows in; the others
// stand as they were. The surface gives a link's number to another address once no cell holds it,
// so a cell is the same as the one shown only when its link's address is the same too.
class Standing {
public:
    Standing() = default;

    // A blank screen of `size`, as after clearing it.
    explicit Standing(Size size)
        : size_(size), cells_(static_cast<std::size_t>(std::max(size.wide, 0)) * static_cast<std::size_t>(std::max(size.high, 0))) {}

    // The size.
    [[nodiscard]] Size size() const { return size_; }

    // The cell at `where`; a blank one outside.
    [[nodiscard]] Cell at(Spot where) const {
        if (where.x < 0 || where.y < 0 || where.x >= size_.wide || where.y >= size_.high) return Cell{};

        return cells_[static_cast<std::size_t>(where.y) * static_cast<std::size_t>(size_.wide) + static_cast<std::size_t>(where.x)];
    }

    // Whether the screen shows at `where` what the frame `made` has there, its link's address
    // included.
    [[nodiscard]] bool shows(const Frame& made, Spot where) const {
        const Cell one = made.at(where);

        if (!(one == at(where))) return false;

        if (one.link == 0) return true;

        const auto found = shown_.find(one.link);

        return found != shown_.end() && found->second == made.address(one.link);
    }

    // Row `y` of a frame of the same size, as it now stands on the screen.
    void took(const Frame& made, int y) {
        if (y < 0 || y >= size_.high) return;

        for (int x = 0; x < size_.wide; ++x) {
            const Cell one = made.at({x, y});

            cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(size_.wide) + static_cast<std::size_t>(x)] = one;

            if (one.link != 0) shown_[one.link] = std::string{made.address(one.link)};
        }
    }

private:
    Size size_;
    std::vector<Cell> cells_;
    std::map<std::uint32_t, std::string> shown_;
};

// The squared distance between two channel values.
int apart(int one, int two) { return (one - two) * (one - two); }

// The nearest entry of the 256-color palette: the 6x6x6 cube or the grey ramp. The closer of the
// two wins, because a grey from the cube may be farther than the separate grey ladder.
int indexed(Color one) {
    const auto closest = [](int given) {
        int best = 0;

        for (int which = 1; which < 6; ++which)
            if (apart(given, palette::steps[static_cast<std::size_t>(which)]) < apart(given, palette::steps[static_cast<std::size_t>(best)]))
                best = which;

        return best;
    };

    const int red = closest(one.red);
    const int green = closest(one.green);
    const int blue = closest(one.blue);

    const int cube = apart(one.red, palette::steps[static_cast<std::size_t>(red)]) + apart(one.green, palette::steps[static_cast<std::size_t>(green)]) +
                     apart(one.blue, palette::steps[static_cast<std::size_t>(blue)]);

    int grey = (one.red + one.green + one.blue) / 3;
    int level = (grey - 8) / 10;

    level = level < 0 ? 0 : (level > 23 ? 23 : level);
    grey = 8 + 10 * level;

    const int plain = apart(one.red, grey) + apart(one.green, grey) + apart(one.blue, grey);

    if (plain < cube) return 232 + level;

    return 16 + 36 * red + 6 * green + blue;
}

// The nearest of the palette's sixteen basic colors. The values are xterm's; other terminals paint
// them a little differently, but choosing the nearest is an approximation anyway.
int shortened(Color one) {
    int best = 0;
    int least = -1;

    for (int which = 0; which < 16; ++which) {
        const Color basic = palette::basics[static_cast<std::size_t>(which)];
        const int much = apart(one.red, basic.red) + apart(one.green, basic.green) + apart(one.blue, basic.blue);

        if (least < 0 || much < least) {
            least = much;
            best = which;
        }
    }

    return best;
}

// Appends the SGR parameters for an ink or paper color at the terminal's color level: 24-bit,
// 256-color, or the sixteen basic ones. Nothing for no color or a mono terminal.
void tinted(const std::optional<Color>& one, bool ink, Colors how, std::string& into) {
    if (!one || how == Colors::mono) return;

    if (how == Colors::rgb) {
        into += ink ? ";38;2;" : ";48;2;";
        into += std::to_string(one->red) + ";" + std::to_string(one->green) + ";" + std::to_string(one->blue);

        return;
    }

    if (how == Colors::palette) {
        into += ink ? ";38;5;" : ";48;5;";
        into += std::to_string(indexed(*one));

        return;
    }

    const int which = shortened(*one);

    if (which < 8) into += ";" + std::to_string((ink ? 30 : 40) + which);
    else into += ";" + std::to_string((ink ? 90 : 100) + which - 8);
}

// Appends the SGR sequence for a look, always written from a reset. Computing the difference
// between two looks would save a few bytes but cost remembering which attribute turns off what;
// the cursor jumps are the expensive part anyway, not those few characters.
void painted(const Look& what, Colors how, std::string& into) {
    into += "\x1b[0";

    if (what.bold) into += ";1";
    if (what.faint) into += ";2";
    if (what.italic) into += ";3";
    if (what.under) into += ";4";
    if (what.reverse) into += ";7";
    if (what.crossed) into += ";9";

    tinted(what.ink, true, how, into);
    tinted(what.paper, false, how, into);

    into += "m";
}

// Appends a cursor jump to `where` (zero-based).
void placed(Spot where, std::string& into) {
    into += "\x1b[" + std::to_string(where.y + 1) + ";" + std::to_string(where.x + 1) + "H";
}

// The kitty graphics protocol: appends the sending of one picture's frame. A frame goes to the
// terminal once, under the picture's mark, and after that it is only placed and taken away. Raw
// pixels (f=32, RGBA) in base64, in pieces of 4096 characters, the pieces after the first carrying
// only m and q; q=2 silences the answers that would otherwise come as keys.
void kitty(const Picture& one, std::string& into) {
    const media::Pixels& pixels = *one.pixels;
    const std::string coded = crypto::base64(pixels.stills()[one.still].rgba);
    constexpr std::size_t piece = 4096;

    for (std::size_t at = 0;; at += piece) {
        const bool more = at + piece < coded.size();

        into += "\x1b_G";

        if (at == 0)
            into += "a=t,f=32,s=" + std::to_string(pixels.size().width) + ",v=" + std::to_string(pixels.size().height) + ",i=" + std::to_string(one.mark) + ",";

        into += more ? "q=2,m=1;" : "q=2,m=0;";
        into.append(coded, at, piece);
        into += "\x1b\\";

        if (!more) return;
    }
}

// Appends the placing of a picture sent before, over its rectangle of cells: in columns and rows
// (c, r), so that the terminal fits it into them even where we know the cell's size wrong, and C=1
// leaves the cursor where it is, which is why with kitty the last row may hold a picture.
void stood(const Placement& one, std::string& into) {
    placed(one.at, into);

    into += "\x1b_Ga=p,i=" + std::to_string(one.picture.mark) + ",c=" + std::to_string(one.picture.size.wide) + ",r=" +
            std::to_string(one.picture.size.high) + ",C=1,q=2\x1b\\";
}

// Appends the taking away of the picture `mark` from the screen (d=i, the terminal keeps its
// pixels) or its forgetting (d=I, the pixels go too).
void unstood(std::uint32_t mark, bool forget, std::string& into) {
    into += "\x1b_Ga=d,d=";
    into += forget ? 'I' : 'i';
    into += ",i=" + std::to_string(mark) + ",q=2\x1b\\";
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

// The hyperlinks of a frame the terminal is given, by number, with their addresses as OSC 8
// carries them, gathered as the cells ask for them; none at all when the terminal does not take
// links.
class Addresses {
public:
    Addresses(const Frame& made, bool linking) : made_(made), linking_(linking) {}

    // The number the cell is written under: its link when the terminal gets it, otherwise none.
    [[nodiscard]] std::uint32_t of(const Cell& one) {
        if (!linking_ || one.link == 0) return 0;

        auto found = known_.find(one.link);

        if (found == known_.end()) found = known_.emplace(one.link, addressed(made_.address(one.link))).first;

        return found->second ? one.link : 0;
    }

    // The address of the link `mark`, which `of` gave.
    [[nodiscard]] const std::string& at(std::uint32_t mark) const { return *known_.at(mark); }

private:
    const Frame& made_;
    bool linking_;
    std::map<std::uint32_t, std::optional<std::string>> known_;
};

// Appends the switch to the hyperlink `mark` (OSC 8 with the number as its id), or its end for
// zero. A link is a state of the terminal like the look: it covers every character written until
// the next switch, wherever the cursor jumps in between.
void linking(std::uint32_t mark, const Addresses& known, std::string& into) {
    if (mark == 0) {
        into += "\x1b]8;;\x1b\\";

        return;
    }

    into += "\x1b]8;id=" + std::to_string(mark) + ";" + known.at(mark) + "\x1b\\";
}

// Whether the terminal may count the cell's cluster in other cells than tal does: a cluster of
// several characters, which the terminal cuts and measures by its own rules (the conjuncts of Indic
// scripts, the vowels of Thai, the emoji sequences), a spacing mark or a prepended character, which
// a terminal following UAX #29 joins to the character beside it, unlike tal. The terminal moves its
// cursor by its own count, so what is written after such a cluster in one piece lands elsewhere
// than in its cells.
bool uncertain(std::string_view cluster) {
    std::size_t at = 0;
    const std::optional<char32_t> first = text::decoded(cluster, at);

    if (!first) return false;

    return at < cluster.size() || text::unicode::spacing(*first) || text::unicode::prepending(*first);
}

// The longest gap between changed cells that is rewritten rather than jumped over.
constexpr int nearby = 4;

// The end of the stretch of row `y` that starts at `x`: a space alone, or a word, the cells up to
// the next space. The terminal joins and shapes characters only within a word.
int worded(const Frame& made, int y, int x) {
    if (made.at({x, y}).cluster == " ") return x + 1;

    int end = x + 1;

    while (end < made.size().wide && made.at({end, y}).cluster != " ") ++end;

    return end;
}

// Whether a cell in [from, upto) of row `y` has a cluster the terminal may count otherwise.
bool unsure(const Frame& made, int y, int from, int upto) {
    for (int x = from; x < upto; ++x)
        if (uncertain(made.at({x, y}).cluster)) return true;

    return false;
}

// Whether a cell in [from, upto) of row `y` differs from what the terminal has.
bool altered(const Frame& made, const Standing& standing, int y, int from, int upto) {
    for (int x = from; x < upto; ++x)
        if (!standing.shows(made, {x, y})) return true;

    return false;
}

// The settings the terminal cannot guess from a frame: it has to be told them with a sequence and
// told to undo them with another.
struct Sending {
    Screen screen = Screen::none;
    Mouse mouse = Mouse::none;
    bool hover = false;
    bool bracketed_paste = false;
    bool focus = false;
};

// The effective screen, mouse, hover, bracketed-paste and focus settings. Tracking motion without a
// pressed button has nothing to measure in the terminal: no reply says whether ?1003 is understood.
// So the wish alone decides, and the default is off, because when on, it makes the terminal report
// every twitch of the pointer, even when nobody cares, and over ssh that is real traffic on the
// link. The window's focus (?1004) is reported whenever the terminal says it knows how.
Sending sending(const Capabilities& compat, const Setup& wish) {
    Sending made;

    made.screen = wish.screen.value_or(compat.screen());
    made.mouse = wish.mouse.value_or(compat.mouse());
    made.hover = wish.hover.value_or(false);
    made.bracketed_paste = wish.bracketed_paste.value_or(compat.bracketed_paste());
    made.focus = compat.focus_events();

    return made;
}

// The effective block-glyph density. The only field where the wish does not win outright:
// `Capabilities::blocks()` returns a ceiling, the density the terminal does not yet break up. A
// glyph wider than a cell breaks the surface, mouse hits and writing the difference, so the smaller
// of the two is taken.
Blocks blocked(const Capabilities& compat, const Setup& wish) {
    const Blocks want = wish.blocks.value_or(Blocks::basic);
    const Blocks most = compat.blocks();

    return want < most ? want : most;
}

// How this terminal draws emoji sequences, from the probe's widths: a programmer (woman, ZWJ,
// laptop) two columns wide means it joins the sequence, a red heart with the emoji selector two
// columns wide means the selector widens. Without an answer the rendering stays tal's usual one.
text::Rendering drawn(const Capabilities& compat) {
    text::Rendering made;

    if (compat.joined() > 0) made.joined = compat.joined() <= 2;

    if (compat.varied() > 0) made.presented = compat.varied() >= 2;

    return made;
}

// What holds for this terminal: the wish where it says something, otherwise the measurement, and
// the size as last measured.
Abilities effective(const Capabilities& compat, const Setup& wish, Size size) {
    Abilities made;

    made.size = size;
    made.colors = wish.colors.value_or(compat.colors());
    made.mouse = wish.mouse.value_or(compat.mouse());
    made.screen = wish.screen.value_or(compat.screen());
    made.shade = wish.shade.value_or(compat.shade());
    made.graphics = wish.graphics.value_or(compat.graphics());
    made.blocks = blocked(compat, wish);
    made.cell = wish.cell.value_or(compat.cell());
    made.unicode = wish.unicode.value_or(compat.unicode());
    made.rendering = drawn(compat);
    made.links = wish.links.value_or(compat.links());
    made.titles = wish.titles.value_or(compat.titles());
    made.synchronized = wish.synchronized.value_or(compat.synchronized());
    made.bracketed_paste = wish.bracketed_paste.value_or(compat.bracketed_paste());

    return made;
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

// The look of the terminal's cursor before anyone asked for another one.
constexpr Caret usual{};

// Whether two carets look the same, wherever they stand.
bool alike(const Caret& one, const Caret& two) { return one.form == two.form && one.blink == two.blink; }

// The sequence that asks the terminal for a cursor's shape (DECSCUSR, `CSI Ps SP q`): 0 for its
// usual one, 1 and 2 a block, 3 and 4 an underline, 5 and 6 a bar, the odd ones blinking.
std::string shaping(const Caret& shape) {
    int number = 0;

    switch (shape.form) {
        case Caret::Form::usual: number = 0; break;
        case Caret::Form::block: number = shape.blink ? 1 : 2; break;
        case Caret::Form::underline: number = shape.blink ? 3 : 4; break;
        case Caret::Form::bar: number = shape.blink ? 5 : 6; break;
    }

    return "\x1b[" + std::to_string(number) + " q";
}

// The terminal's usual cursor back, when it was asked for another one; afterwards the shape is
// not known, since whoever takes the terminal may change it.
void unshaped(Console& out, std::optional<Caret>& shaped) {
    if (!shaped || !alike(*shaped, usual)) out.write(shaping(usual));

    shaped.reset();
}

// Dressing the terminal: the screen, a hidden cursor, no autowrap, a cleared screen, the mouse,
// bracketed paste and the window's focus. Dressing and undressing are two halves of one thing, so
// they stand side by side and have one place: the constructor, the destructor and stepping aside
// for another program all use them. Written out in three places, they would drift apart at the
// first change.
void dressed(Console& out, const Sending& now) {
    screened(out, now.screen);

    out.write("\x1b[?25l\x1b[?7l\x1b[2J\x1b[H");

    mousing(out, now.mouse, now.hover);

    if (now.bracketed_paste) out.write("\x1b[?2004h");

    if (now.focus) out.write("\x1b[?1004h");
}

// Undressing the terminal, in the reverse order of `dressed`.
void undressed(Console& out, const Sending& now) {
    if (now.focus) out.write("\x1b[?1004l");

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
bool measured(const Console& from, Size& size) {
    const std::optional<Size> now = from.size();

    if (!now.has_value() || now->wide <= 0 || now->high <= 0) return false;

    if (*now == size) return false;

    size = *now;

    return true;
}

// The writing of one frame's cells (`made`) into `into`, keeping the terminal's look, cursor and
// open link (`now`, `at`, `linked`) as they go; links the terminal is not given stay plain text.
struct Pen {
    const Frame& made;
    Addresses& known;
    const Colors& palette;
    std::string& into;
    Look& now;
    Spot& at;
    std::uint32_t& linked;

    // Rewrites the cells [from, upto) of row `y` instead of jumping over them, when that is
    // possible; returns whether it did. A cursor jump costs several bytes, a character one. A short
    // gap between changed cells is cheaper to rewrite than to jump over, provided the rewritten
    // cells come out exactly as they already are: the same look and the same hyperlink as the
    // current ones, no double-width character and nothing that is only its continuation.
    bool bridged(int y, int from, int upto) {
        if (upto - from > nearby) return false;

        for (int x = from; x < upto; ++x) {
            const Cell& one = made.at({x, y});

            if (one.cluster.empty() || (x + 1 < made.size().wide && made.at({x + 1, y}).cluster.empty())) return false;

            if (!(one.look == now) || known.of(one) != linked) return false;
        }

        for (int x = from; x < upto; ++x) into += made.at({x, y}).cluster;

        return true;
    }

    // Brings the cursor to (x, y): rewriting a short stretch of cells when that is cheaper than a
    // jump, otherwise a jump.
    void reached(int x, int y) {
        if (at == Spot{x, y}) return;

        if (at.y != y || at.x < 0 || at.x > x || !bridged(y, at.x, x)) placed({x, y}, into);

        at = Spot{x, y};
    }

    // Switches the terminal to the look, unless it has it already.
    void dressed(const Look& look) {
        if (look == now) return;

        painted(look, palette, into);

        now = look;
    }

    // Writes the cell at (x, y), where the cursor stands, after switching the look and the link
    // where they differ from the terminal's.
    void lettered(int x, int y) {
        const Cell& one = made.at({x, y});

        dressed(one.look);

        if (const std::uint32_t link = known.of(one); link != linked) {
            linking(link, known, into);

            linked = link;
        }

        into += one.cluster;

        const bool broad = x + 1 < made.size().wide && made.at({x + 1, y}).cluster.empty();

        at.x += broad ? 2 : 1;

        // Autowrap is off, so at the last column the cursor stands in an undefined place; the
        // next character gets its own jump.
        if (at.x >= made.size().wide) at = Spot{-1, -1};
    }

    // Row `y` goes out by stretches: spaces and words. A word with a cluster the terminal may count
    // otherwise (`uncertain`) goes out whole whenever any of its cells changed, because the
    // terminal joins its characters by its own rules: its cells are erased first (ECH), so that
    // nothing of the previous picture stays where the terminal draws it narrower, and after it the
    // cursor jumps to the next cell instead of trusting the terminal's count. A terminal that drew
    // it wider covered the cells after it, so once such a word went out, the rest of the row goes
    // out again too, and `covered` says from which column. Elsewhere only the cells that differ
    // from what stands go.
    void rowed(int y, const Standing& standing, int& covered) {
        bool covering = false;

        for (int x = 0; x < made.size().wide;) {
            const int end = worded(made, y, x);

            if (unsure(made, y, x, end)) {
                if (covering || altered(made, standing, y, x, end)) {
                    reached(x, y);

                    // ECH erases in the background of the current look, so the word's own goes
                    // first.
                    dressed(made.at({x, y}).look);

                    into += "\x1b[" + std::to_string(end - x) + "X";

                    for (int on = x; on < end; ++on)
                        if (!made.at({on, y}).cluster.empty()) lettered(on, y);

                    if (!covering) covered = x;

                    covering = true;
                }

                // Where the terminal's cursor stands after the word is not known, and the word
                // must not be rewritten as a mere gap either.
                at = Spot{-1, -1};
                x = end;

                continue;
            }

            for (; x < end; ++x) {
                const Cell& one = made.at({x, y});

                // The continuation of a wide character went out together with its first half.
                if (one.cluster.empty()) continue;

                if (!covering && standing.shows(made, {x, y})) continue;

                reached(x, y);
                lettered(x, y);
            }
        }
    }
};

}

// How long a turn waits for the loop's work while the terminal belongs to another program.
constexpr std::chrono::milliseconds glancing{100};

// The loop of an application: the program's work runs on it between hearings, and work posted to
// it, a timer set or a stop from any thread ends the hearing under way, so the application turns
// at once.
class Listening final : public Loop {
public:
    Listening(Port<Clock> time, Port<Console> console) : Loop(std::move(time)), console_(std::move(console)) {}

protected:
    // Ends the console's hearing, so that the application runs what was posted.
    void stirred() override { console_->interrupt(); }

private:
    Port<Console> console_;
};

// The application's state, behind a pointer so the public header stays free of it.
struct App::Held {
    Held(Port<Console> with, Port<Clock> time) : console(with), compat(with), loop(std::move(time), std::move(with)) {}

    // Everything that reaches the terminal goes through this port: frames, the title, the mouse and
    // the screen, and also input, raw mode, the size and the double-click time.
    Port<Console> console;

    // It measures the terminal and decides nothing; it asks through the same console we draw on.
    const Capabilities compat;
    Decoder decoder;
    Listening loop;

    // The application's wish; the effective value is formed as wish.value_or(measurement).
    Setup wish;
    Abilities able;

    // A fresh configuration invalidates the screen image just as a change of the window size does.
    bool shifted = false;

    Document* onto = nullptr;

    std::string spare;

    bool running = true;
    int code = 0;
    bool entered = false;
    bool fresh = true;

    Standing standing;

    // The document's count of frames at the last one drawn (`Document::framed`). A frame in
    // between, which someone else asked for, took its damage away; the next one is then compared
    // whole.
    std::uint64_t framed = 0;

    Look now;
    std::uint32_t linked = 0;
    Spot at{-1, -1};
    bool showing = false;

    // The cursor's shape the terminal was last asked for; at the start its usual one. Nothing when
    // it is not known: another program had the terminal meanwhile.
    std::optional<Caret> shaped = usual;

    Size size{80, 25};

    // The kitty pictures standing after the last frame, and the frames whose pixels the terminal
    // holds under their marks, as long as someone here still holds them too.
    std::vector<Placement> placed;
    std::vector<std::pair<std::uint32_t, std::weak_ptr<const media::Pixels>>> sent;

    // The notifier the application puts among the defaults for its lifetime, which shows notices
    // through this terminal when it knows a way, through the system otherwise.
    std::shared_ptr<telling::Terminal> notifier;

    // The notifier set among the defaults before the application, or none, which stands again at
    // its end.
    std::shared_ptr<Notifier> before;

    // How many times we have stepped out of the terminal's way. A counter, not a flag, because
    // nested stepping aside is allowed and means one thing: the first one leaves, the last one
    // returns.
    int away = 0;

    // Tells the document shown what the terminal is.
    void displayed() const {
        if (onto != nullptr) onto->display(Display{able.size, able.colors, able.unicode, able.rendering, able.graphics, able.blocks, able.cell});
    }

    // Takes away and forgets every kitty picture, on leaving and when handing the terminal over:
    // the other program gets a clean screen, and our marks could meet its own.
    void forgotten() {
        if (placed.empty() && sent.empty()) return;

        console->write("\x1b_Ga=d,d=A,q=2\x1b\\");

        placed.clear();
        sent.clear();
    }

    // Writes the frame in a single write, held whole by the terminal if it can (mode 2026).
    void delivered(const std::string& into) {
        if (into.empty()) return;

        if (able.synchronized) console->write("\x1b[?2026h");

        console->write(into);

        if (able.synchronized) console->write("\x1b[?2026l");
    }
};

// Takes over the terminal: raw mode, the probe of its capabilities, then the dressing (alternate
// screen, hidden cursor, mouse) according to the wish. The application's loop is made here and,
// when it is the first loop, it is the program's loop that tasks run on. Notices go through the
// terminal while the application lives (`telling::Terminal` among the defaults).
App::App(Setup wish, Port<Console> console, Port<Clock> time) : held_(std::make_unique<Held>(std::move(console), std::move(time))) {
    Held& held = *held_;

    held.wish = std::move(wish);

    Console& out = *held.console;

    out.raw(true);

    // The probe has to happen on a prepared terminal, but before entering the alternate screen;
    // otherwise the probe characters would flicker on the application's clean screen.
    (void)held.compat.colors();

    held.spare = held.compat.rest();

    dressed(out, sending(held.compat, held.wish));

    held.entered = true;

    // The size only after entering the alternate screen: on Windows it is a different buffer, and
    // we measure the one we draw on.
    (void)measured(out, held.size);

    held.able = effective(held.compat, held.wish, held.size);

    // Last, so that a constructor that throws leaves no notifier on a loop that is gone.
    held.before = defaults().held<Notifier>();
    held.notifier = std::make_shared<telling::Terminal>(held.console, telling::way(held.compat.named()), defaults().get<Notifier>(), held.loop);
    defaults().set<Notifier>(held.notifier);
}

// Gives the terminal back, in reverse order, however we got here. A terminal left in raw mode
// without a cursor is unusable, and the user has no way to undo it. When the terminal is handed
// to someone else right now there is nothing to undo: `Pause` did it on entry. Notices waiting are
// written first, the notifier set before the application stands again (none when none was set,
// unless another was set meanwhile), and the document shown lets go of the loop.
App::~App() {
    Held& held = *held_;

    held.notifier->closed();

    if (defaults().held<Notifier>() == held.notifier) {
        if (held.before != nullptr) defaults().set<Notifier>(held.before);
        else defaults().clear<Notifier>();
    }

    if (held.onto != nullptr) held.onto->homed(nullptr);

    if (held.away > 0) return;

    held.forgotten();

    if (held.entered) {
        unshaped(*held.console, held.shaped);

        undressed(*held.console, sending(held.compat, held.wish));
    }

    held.console->raw(false);
}

// The loop the application turns.
Loop& App::loop() { return held_->loop; }

// Applies a new wish at the next turn, on the loop, so that it always comes in between frames and
// on the application's thread, wherever it was asked.
void App::reconfigure(Setup wish) {
    held_->loop.post([held = held_.get(), wish = std::move(wish)]() mutable {
        const Sending was = sending(held->compat, held->wish);

        held->wish = std::move(wish);

        shifting(*held->console, was, sending(held->compat, held->wish));

        // Another screen hides the cursor; the next frame shows and places it again.
        held->showing = false;
        held->at = Spot{-1, -1};
        held->able = effective(held->compat, held->wish, held->size);
        held->shifted = true;

        held->displayed();
    });
}

// What holds for this terminal now: the wish, else the measurement.
const Abilities& App::abilities() const { return held_->able; }

// Shows `one`: it is told the terminal's size, colors, Unicode, rendering and the console's
// double-click time, and drawn whole at the next turn. Documents can be switched while running, as
// screens switch. The document checks its drops on the application's loop; one shown before lets
// go of it.
void App::show(Document& one) {
    Held& held = *held_;

    if (held.onto != nullptr && held.onto != &one) held.onto->homed(nullptr);

    held.onto = &one;
    one.homed(&held.loop);

    one.double_click(held.console->double_click());

    held.displayed();

    held.shifted = true;
}

// The document shown, or nothing before `show`.
Document* App::shown() const { return held_->onto; }

// Sets the window title (OSC 0), when titles are enabled. A title often comes from a file name or
// another program, so its control characters (C0, DEL and C1 as UTF-8) are left out: an escape or a
// BEL would end the title early and command the terminal with what follows.
void App::title(std::string_view said) {
    if (!held_->able.titles) return;

    std::string made = "\x1b]0;";

    for (std::size_t at = 0; at < said.size(); ++at) {
        const auto one = static_cast<unsigned char>(said[at]);

        if (one < 0x20 || one == 0x7F) continue;

        if (one == 0xC2 && at + 1 < said.size() && static_cast<unsigned char>(said[at + 1]) >= 0x80 &&
            static_cast<unsigned char>(said[at + 1]) <= 0x9F) {
            ++at;

            continue;
        }

        made += said[at];
    }

    made += "\x1b\\";

    held_->console->write(made);
}

// Draws a frame of the document shown, which `turn` has checked: the document makes it, and only
// the difference from the screen goes to the terminal, in a single write.
//
// Pictures go after the cells. A sixel lies on the cells under it and goes when one of its cells
// changed (it is new, moved, another frame of an animation, uncovered, the screen cleared) or went
// out again with the rest of its row: writing a cell erases the pixels under it, so the cells go
// first and the picture over them. Its rectangle keeps off the last row, since after a sixel the
// terminal moves the cursor below it, and below the last row there is only scrolling. A kitty
// picture is no part of the text, so what decides is what stands: one gone or moved is taken away
// before the cells, one new placed after them, its frame sent first when the terminal does not
// hold it yet, and the terminal forgets a frame once nobody here holds its pixels.
void App::draw() {
    Held& held = *held_;

    const std::uint64_t before = held.framed;
    const Surface& surface = held.onto->frame();
    const Size size = surface.size();
    const Graphics kind = held.able.graphics == Graphics::sixel && (held.able.cell.wide <= 0 || held.able.cell.high <= 0) ? Graphics::none : held.able.graphics;
    const Frame made{surface, kind, Area{{0, 0}, {size.wide, kind == Graphics::sixel ? size.high - 1 : size.high}}};
    std::string into;

    held.framed = held.onto->framed();

    // A size change or a fresh configuration invalidates the previous frame: we compare with a
    // blank screen, so everything comes out anew. Clearing the screen leaves kitty pictures, which
    // are no text, so they go too; their pixels stay, and the frame places them again.
    const bool reset = held.shifted || !(held.standing.size() == made.size());

    if (reset) {
        if (!held.placed.empty()) into += "\x1b_Ga=d,d=a,q=2\x1b\\";

        held.placed.clear();

        into += "\x1b[0m\x1b[2J";

        held.standing = Standing{made.size()};
        held.now = Look{};
        held.at = Spot{-1, -1};
        held.shifted = false;
    }

    std::vector<Placement> sixels;
    std::vector<Placement> kitties;

    for (const Placement& one : made.placed()) (kind == Graphics::sixel ? sixels : kitties).push_back(one);

    // Whether a picture shows whole depends on all its rows, and so do the cells made blank for it,
    // whichever rows were painted; with pictures every row is compared.
    std::vector<int> rows;

    if (reset || held.framed != before + 1 || made.pictured()) {
        for (int y = 0; y < made.size().high; ++y) rows.push_back(y);
    } else {
        for (const Area& one : surface.damaged())
            for (int y = one.at.y; y < one.at.y + one.size.high; ++y) rows.push_back(y);

        std::sort(rows.begin(), rows.end());
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    }

    for (const Placement& one : held.placed)
        if (std::none_of(kitties.begin(), kitties.end(), [&one](const Placement& kept) { return same_place(one, kept); })) unstood(one.picture.mark, false, into);

    Addresses known{made, held.able.links};
    Pen pen{made, known, held.able.colors, into, held.now, held.at, held.linked};
    std::vector<int> covered(static_cast<std::size_t>(std::max(size.high, 0)), size.wide);

    for (const int y : rows) pen.rowed(y, held.standing, covered[static_cast<std::size_t>(y)]);

    // No link stays open past the frame: whatever reaches the terminal outside a frame must not
    // land inside one.
    if (held.linked != 0) {
        linking(0, known, into);

        held.linked = 0;
    }

    std::erase_if(sixels, [&](const Placement& one) {
        for (int y = one.at.y; y < one.at.y + one.picture.size.high; ++y)
            for (int x = one.at.x; x < one.at.x + one.picture.size.wide; ++x)
                if (!held.standing.shows(made, {x, y}) || x >= covered[static_cast<std::size_t>(y)]) return false;

        return true;
    });

    for (const Placement& one : sixels) {
        if (!one.picture.sixel) continue;

        placed(one.at, into);

        into += *one.picture.sixel;
        held.at = Spot{-1, -1};
    }

    for (const Placement& one : kitties) {
        if (std::any_of(held.placed.begin(), held.placed.end(), [&one](const Placement& was) { return same_place(was, one); })) continue;

        if (std::none_of(held.sent.begin(), held.sent.end(), [&one](const auto& was) { return was.first == one.picture.mark; })) {
            kitty(one.picture, into);

            held.sent.emplace_back(one.picture.mark, one.picture.pixels);
        }

        stood(one, into);

        held.at = Spot{-1, -1};
    }

    held.placed = std::move(kitties);

    // The cursor comes from the document: where the node with the focus wants it, in its shape,
    // shown; hidden when there is none. The shape goes before the cursor shows.
    if (const std::optional<Caret> caret = held.onto->caret()) {
        placed(caret->at, into);

        held.at = caret->at;

        if (!held.shaped || !alike(*held.shaped, *caret)) {
            into += shaping(*caret);

            held.shaped = *caret;
        }

        if (!held.showing) {
            into += "\x1b[?25h";

            held.showing = true;
        }
    } else if (held.showing) {
        into += "\x1b[?25l";

        held.showing = false;
    }

    for (const int y : rows) held.standing.took(made, y);

    // The terminal forgets a frame whose pixels nobody here holds any more: last, once the memory
    // of this frame (`placed`) holds what stands.
    std::erase_if(held.sent, [&into](const auto& one) {
        if (!one.second.expired()) return false;

        unstood(one.first, true, into);

        return true;
    });

    held.delivered(into);
}

// Asks `run` to end after the current turn, with `code`; from any thread.
void App::stop(int code) {
    held_->loop.post([held = held_.get(), code] {
        held->running = false;
        held->code = code;
    });
}

// Turns until `stop`; returns the code it was given.
int App::run() {
    while (turn()) {
    }

    return held_->code;
}

// One turn: the loop's work, then waiting for input, the loop's work, a timer or the decoder's
// deadline, whichever comes first; delivers what came, rings the document's timers and draws when
// the document changed. Returns false once `stop` was called. Throws std::logic_error when no
// document is shown.
bool App::turn() {
    Held& held = *held_;

    if (held.onto == nullptr) throw std::logic_error("tal::App: no document shown");

    Console& console = *held.console;
    Clock& clock = held.loop.clock();

    // The terminal is handed over: input belongs to the other program, the screen to whatever it
    // writes there. Only the loop remains, so that timers and work can finish; it waits for them
    // by the real clock, which its condition measures, so `run` does not spin.
    if (held.away > 0) {
        (void)held.loop.turn(std::chrono::steady_clock::now() + glancing);

        return held.running;
    }

    (void)held.loop.turn();

    // Keys pressed during the probe have waited since start-up; they go first.
    if (!held.spare.empty()) {
        const std::string given = std::move(held.spare);

        held.spare.clear();

        for (const Heard& one : held.decoder.fed(given, clock.now())) dispatched(one, *held.onto);
    }

    // The first frame is drawn before waiting, because the wait may have no limit.
    if (held.fresh) {
        held.fresh = false;

        draw();
    }

    if (!held.running) return false;

    // The wait ends at the nearest of the decoder's deadline (a lone Esc, a paste without its
    // end), the document's timer and the loop's timer, or without a limit; work posted meanwhile
    // ends it early (`Listening`). All three are measured by the loop's clock, which the document
    // shares unless it was given another one.
    std::optional<std::chrono::steady_clock::time_point> until = held.decoder.due();

    for (const auto& one : {held.onto->alarm(), held.loop.next()})
        if (one && (!until || *one < *until)) until = one;

    // A frame can leave the document due, when a handler of `Arranged` laid something out again for
    // the next frame (the focus going into a window just opened); that frame comes at once.
    if (held.onto->due()) until = clock.now();

    std::optional<std::chrono::milliseconds> patience;

    if (until) patience = std::max(std::chrono::ceil<std::chrono::milliseconds>(*until - clock.now()), std::chrono::milliseconds{0});

    const std::string given = console.hear(patience);

    // Resumed after a stop: meanwhile the terminal belonged to the shell, which removed our
    // dressing and wrote on the screen. We dress it again and redraw everything, cursor included.
    if (console.resumed()) {
        if (held.entered) dressed(console, sending(held.compat, held.wish));

        held.shifted = true;
        held.showing = false;
        held.shaped.reset();
        held.at = Spot{-1, -1};
    }

    const auto now = clock.now();

    if (!given.empty())
        for (const Heard& one : held.decoder.fed(given, now)) dispatched(one, *held.onto);

    // The size is checked at every turn: the window may have changed just now.
    if (measured(console, held.size)) {
        held.able.size = held.size;

        held.displayed();
    }

    // The decoder's time has come: the Esc was alone, or the paste lost its end.
    if (const auto settling = held.decoder.due(); settling && now >= *settling)
        for (const Heard& one : held.decoder.lapsed(now)) dispatched(one, *held.onto);

    held.onto->ring();

    (void)held.loop.turn();

    if (held.onto->due() || held.shifted) draw();

    return held.running;
}

// Steps out of the terminal's way: we undo exactly what the constructor set up, in reverse order.
// The program that runs meanwhile finds the terminal as we found it, and no notice of ours. Nested
// pauses are one: the first leaves, the last returns.
App::Pause::Pause(App& one) : app_(one) {
    Held& held = *app_.held_;

    if (held.away++ > 0) return;

    held.notifier->away(true);
    held.forgotten();

    if (held.entered) {
        unshaped(*held.console, held.shaped);

        undressed(*held.console, sending(held.compat, held.wish));
    }

    held.console->raw(false);
}

// Takes the terminal back: raw mode and the dressing again, and a full repaint at the next turn,
// with the cursor shown and placed anew. The size may have changed while we were away. Notices told
// meanwhile are written last.
App::Pause::~Pause() {
    Held& held = *app_.held_;

    if (--held.away > 0) return;

    Console& console = *held.console;

    console.raw(true);

    if (held.entered) dressed(console, sending(held.compat, held.wish));

    held.shifted = true;
    held.fresh = true;
    held.showing = false;
    held.at = Spot{-1, -1};

    if (measured(console, held.size)) {
        held.able.size = held.size;

        held.displayed();
    }

    held.notifier->away(false);
}

}
