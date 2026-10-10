#include "proving.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "conhost.hpp"
#include "talcite/capabilities.hpp"
#include "talcite/console.hpp"
#include "talcite/term.hpp"

// The translation for a console without VT. The portable part is checked here, on every system:
// a small terminal reads what `Term` writes and answers the probe, and keyboard and mouse records
// come out as the same bytes that conhost in VT mode would give the decoder. The Win32 glue is
// checked only on a real console.

using namespace talcite;

namespace {

// A screen of the given size in the console colors white on black, as in the default cmd.exe.
conhost::Screen screen(int wide = 20, int high = 5) { return conhost::Screen{{wide, high}, 7, 0}; }

// The characters of row `y` of the screen, without trailing spaces.
std::u32string row(const conhost::Screen& from, int y) {
    std::u32string made;

    for (int x = 0; x < from.size().wide; ++x) made += from.at(x, y).glyph;

    while (!made.empty() && made.back() == U' ') made.pop_back();

    return made;
}

// A console with the translation behind it, as it will stand in `console.cpp`, only without Win32.
// On Windows an empty TERM is enough for "this is a console"; here, under POSIX, the floor needs
// one, so we give the most modest one, without 256 and without COLORTERM, so that the colors
// follow from `vt()`.
class Translated final : public Console {
public:
    conhost::Screen shown{{40, 6}, 7, 0};

    // The name of this fake console.
    [[nodiscard]] std::string_view name() const override { return "translated console"; }

    // Only TERM is set, to xterm.
    [[nodiscard]] std::optional<std::string> variable(std::string_view name) const override {
        if (name == "TERM") return std::string{"xterm"};

        return std::nullopt;
    }

    // There is a terminal to talk to.
    [[nodiscard]] bool lively() const override { return true; }

    // The console does not understand VT sequences itself.
    [[nodiscard]] bool vt() const override { return false; }

    // The size of the translated screen.
    [[nodiscard]] std::optional<ui::Size> size() const override { return shown.size(); }

    // The double-click time of the console.
    [[nodiscard]] std::chrono::milliseconds double_click() const override { return std::chrono::milliseconds{500}; }

    void raw(bool) override {}

    // The program was never suspended.
    [[nodiscard]] bool resumed() override { return false; }

    // Feeds the written bytes to the translated screen.
    void write(std::string_view bytes) override { shown.fed(bytes); }

    // The screen's answers to the queries written so far.
    [[nodiscard]] std::string hear(std::optional<std::chrono::milliseconds>, const Bell*) override {
        return shown.answers();
    }
};

// A root that draws a red x, a wide character and "ok" in reverse video.
class Painted : public ui::Root {
protected:
    // Draws the three samples in the top left corner.
    View paint(ui::Size given, ui::Area seen) override {
        (void)seen;

        View made{given};

        Style red;
        red.foreground = Color{false, 255, 0, 0};

        Style turned;
        turned.reverse = true;

        made.put({0, 0}, U'x', red);
        made.put({2, 0}, U'你', Style{});
        made.put({0, 1}, std::string{"ok"}, turned);

        return made;
    }
};

}

// --- Grid ----------------------------------------------------------------------------------------

TRIAL("Console without VT: text lands at the cursor, and CUP counts from one") {
    conhost::Screen one = screen();

    one.fed("ab\x1b[3;5Hcd");

    same(row(one, 0), std::u32string{U"ab"});
    same(row(one, 2), std::u32string{U"    cd"});
    ought(one.cursor() == ui::Spot{6, 2});
}

TRIAL("Console without VT: UTF-8 cut off halfway comes out as a question mark") {
    conhost::Screen one = screen();

    one.fed("\xc4");
    one.fed("\x85!\xc4x");

    same(row(one, 0), std::u32string{U"ą!?x"});
}

TRIAL("Console without VT: a Chinese character takes two cells, the second empty and marked") {
    conhost::Screen one = screen();

    one.fed("a你b");

    same(one.at(1, 0).glyph, U'你');
    ought(!one.at(1, 0).trailing);
    ought(one.at(2, 0).trailing);
    same(one.at(3, 0).glyph, U'b');
    ought(one.cursor() == ui::Spot{4, 0});
}

TRIAL("Console without VT: covering half of a wide character clears the other half") {
    conhost::Screen one = screen();

    one.fed("你好\x1b[1;2Hx\x1b[1;3Hy");

    same(row(one, 0), std::u32string{U" xy"});
    ought(!one.at(2, 0).trailing);
    ought(!one.at(3, 0).trailing);
}

TRIAL("Console without VT: a character outside the BMP lands as a question mark on two cells") {
    conhost::Screen one = screen();

    one.fed("\xf0\x9f\xac\x80|");

    same(row(one, 0), std::u32string{U"? |"});
    ought(one.at(1, 0).trailing);
}

TRIAL("Console without VT: without wrapping the last column takes the following characters") {
    conhost::Screen one = screen(4, 2);

    one.fed("\x1b[?7labcdef");

    same(row(one, 0), std::u32string{U"abcf"});
    same(row(one, 1), std::u32string{});
}

TRIAL("Console without VT: with wrapping text moves a row down, and at the bottom the grid scrolls up") {
    conhost::Screen one = screen(3, 2);

    one.fed("abcdefg");

    same(row(one, 0), std::u32string{U"def"});
    same(row(one, 1), std::u32string{U"g"});
}

TRIAL("Console without VT: EL and ED clear from the cursor, up to the cursor and everything") {
    conhost::Screen one = screen(6, 3);

    one.fed("abcdef\r\nghijkl\r\nmnopqr");
    one.fed("\x1b[1;3H\x1b[K\x1b[2;3H\x1b[1K\x1b[3;4H\x1b[J");

    same(row(one, 0), std::u32string{U"ab"});
    same(row(one, 1), std::u32string{U"   jkl"});
    same(row(one, 2), std::u32string{U"mno"});

    one.fed("\x1b[2;5H\x1b[J");

    same(row(one, 1), std::u32string{U"   j"});
    same(row(one, 2), std::u32string{});
    same(row(one, 0), std::u32string{U"ab"});

    one.fed("\x1b[2J");

    for (int y = 0; y < 3; ++y) same(row(one, y), std::u32string{});
}

TRIAL("Console without VT: changed rows come out once, in ascending order") {
    conhost::Screen one = screen(6, 4);

    (void)one.dirty();

    one.fed("\x1b[3;1Hx\x1b[1;1Hy");

    ought(one.dirty() == std::vector<int>{0, 2});
    ought(one.dirty().empty());
}

// --- Colors --------------------------------------------------------------------------------------

TRIAL("Console without VT: ANSI SGR maps to console bits, and bold brightens") {
    conhost::Screen one = screen();

    one.fed("\x1b[0;31;44ma\x1b[0;1;32mb\x1b[0;93;100mc\x1b[0md");

    same(int{one.at(0, 0).ink}, 4);
    same(int{one.at(0, 0).paper}, 1);
    same(int{one.at(1, 0).ink}, 10);
    same(int{one.at(2, 0).ink}, 14);
    same(int{one.at(2, 0).paper}, 8);
    same(int{one.at(3, 0).ink}, 7);
    same(int{one.at(3, 0).paper}, 0);
}

TRIAL("Console without VT: reverse swaps ink and paper, and the underline stays on the cell") {
    conhost::Screen one = screen();

    one.fed("\x1b[0;4;7;31ma");

    same(int{one.at(0, 0).ink}, 0);
    same(int{one.at(0, 0).paper}, 4);
    ought(one.at(0, 0).under);
}

TRIAL("Console without VT: 256 colors and RGB go to the nearest console color") {
    conhost::Screen one = screen();

    one.fed("\x1b[0;38;5;196ma\x1b[0;38;2;0;0;140mb\x1b[0;48;5;9mc\x1b[0;38;5;244md");

    same(int{one.at(0, 0).ink}, 12);
    same(int{one.at(1, 0).ink}, 1);
    same(int{one.at(2, 0).paper}, 12);
    same(int{one.at(3, 0).ink}, 8);
}

TRIAL("Console without VT: the nearest color takes the first of equally near ones") {
    const conhost::Palette among = conhost::classic();

    same(int{conhost::nearest(among, 0, 0, 0)}, 0);
    same(int{conhost::nearest(among, 255, 255, 255)}, 15);
    same(int{conhost::nearest(among, 200, 200, 200)}, 7);
    same(int{conhost::nearest(among, 64, 0, 0)}, 0);
}

// --- Modes and the alternate screen --------------------------------------------------------------

TRIAL("Console without VT: cursor and mouse modes follow DECSET and DECRST") {
    conhost::Screen one = screen();

    one.fed("\x1b[?25l\x1b[?1000h\x1b[?1002h\x1b[?1006h");

    ought(!one.shown());
    ought(one.tracking() == conhost::Tracking{true, true, false, true});

    one.fed("\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[?1003h\x1b[?25h");

    ought(one.shown());
    ought(one.tracking() == conhost::Tracking{false, false, true, false});
}

TRIAL("Console without VT: the alternate screen clears and asks for a full rewrite, returning gives back the old one without rewriting") {
    conhost::Screen one = screen(6, 3);

    one.fed("main\r\n\x1b[2;3H");
    (void)one.dirty();

    one.fed("\x1b[?1049h");

    ought(one.apart());
    same(row(one, 0), std::u32string{});
    ought(one.dirty() == std::vector<int>{0, 1, 2});

    one.fed("\x1b[3;1Hapart");
    one.fed("\x1b[?1049l");

    ought(!one.apart());
    same(row(one, 0), std::u32string{U"main"});
    ought(one.cursor() == ui::Spot{2, 1});
    ought(one.dirty().empty());
}

TRIAL("Console without VT: resizing on the alternate screen also clips the main one") {
    conhost::Screen one = screen(6, 3);

    one.fed("abcdef\x1b[?1049h");
    one.resize({3, 2});
    one.fed("\x1b[?1049l");

    same(one.size(), (ui::Size{3, 2}));
    same(row(one, 0), std::u32string{U"abc"});
    same(row(one, 1), std::u32string{});
}

TRIAL("Console without VT: a too long CSI sequence vanishes entirely and does not leak into the text") {
    conhost::Screen one = screen();

    one.fed("\x1b[" + std::string(100, '1') + "mx");

    same(row(one, 0), std::u32string{U"x"});
}

TRIAL("Console without VT: resizing keeps what fits and asks for a full rewrite") {
    conhost::Screen one = screen(6, 3);

    one.fed("abcdef\x1b[3;6H");
    (void)one.dirty();

    one.resize({4, 2});

    same(row(one, 0), std::u32string{U"abcd"});
    ought(one.cursor() == ui::Spot{3, 1});
    ought(one.dirty() == std::vector<int>{0, 1});
}

// --- Answers -------------------------------------------------------------------------------------

TRIAL("Console without VT: DECRQM knows only what the translation can do") {
    conhost::Screen one = screen();

    one.fed("\x1b[?1006$p\x1b[?1049$p\x1b[?2004$p\x1b[?2026$p\x1b[?1004$p\x1b[?25$p");

    same(one.answers(), std::string{"\x1b[?1006;2$y\x1b[?1049;2$y\x1b[?2004;0$y\x1b[?2026;0$y\x1b[?1004;0$y\x1b[?25;1$y"});
    same(one.answers(), std::string{});
}

TRIAL("Console without VT: CPR, DA1, 16t and OSC 11 answer, DA2, APC and DCS stay silent") {
    conhost::Screen one{{20, 5}, 7, 1};

    one.fed("\x1b[>c\x1b_Gi=31,a=q;AAAA\x1b\\\x1bP+q5463\x1b\\\x1b[16t\x1b]11;?\x1b\\\x1b[2;4H\x1b[6n\x1b[c");

    same(one.answers(), std::string{"\x1b]11;rgb:0000/0000/8080\x1b\\\x1b[2;4R\x1b[?62;22c"});
    same(row(one, 0), std::u32string{});

    one.pixels({8, 16});
    one.fed("\x1b[18t\x1b[16t");

    same(one.answers(), std::string{"\x1b[6;16;8t"});
}

TRIAL("Console without VT: the title comes from OSC 0 and OSC 2, ended by ST or BEL") {
    conhost::Screen one = screen();

    one.fed("\x1b]0;twa \xe2\x80\x94 filmy\x1b\\");

    ought(one.title() == std::optional<std::string>{"twa \xe2\x80\x94 filmy"});
    ought(!one.title().has_value());

    one.fed("\x1b]2;drugi\x07x");

    ought(one.title() == std::optional<std::string>{"drugi"});
    same(row(one, 0), std::u32string{U"x"});
}

TRIAL("Console without VT: the capability probe sees sixteen colors, SGR mouse, the alternate screen and basic blocks") {
    auto line = std::make_shared<Translated>();

    const Capabilities seen{Seam<Console>{line}};

    same(static_cast<int>(seen.mouse()), static_cast<int>(Mouse::sgr));
    same(static_cast<int>(seen.screen()), static_cast<int>(Screen::apart));
    same(static_cast<int>(seen.shade()), static_cast<int>(Shade::dark));
    ought(!seen.bracketed_paste());
    ought(!seen.synchronized());
    same(static_cast<int>(seen.graphics()), static_cast<int>(Graphics::none));
    same(static_cast<int>(seen.blocks()), static_cast<int>(ui::Blocks::basic));
    same(seen.wide(), 2);
    same(seen.rest(), std::string{});
}

TRIAL("Console without VT: a Term frame lands in the grid in console colors") {
    auto line = std::make_shared<Translated>();

    Painted root;

    {
        Term term{Setup{}, Seam<Console>{line}};

        term.show(root);
        term.draw();

        const conhost::Screen& shown = line->shown;

        ought(shown.apart());
        same(shown.at(0, 0).glyph, U'x');
        same(int{shown.at(0, 0).ink}, 12);
        same(shown.at(2, 0).glyph, U'你');
        ought(shown.at(3, 0).trailing);
        same(shown.at(0, 1).glyph, U'o');
        same(int{shown.at(0, 1).paper}, 7);
        same(int{shown.at(0, 1).ink}, 0);
        ought(shown.tracking().sgr);
    }

    ought(!line->shown.apart());
    ought(line->shown.tracking() == conhost::Tracking{});
}

// --- Keyboard ------------------------------------------------------------------------------------

namespace {

// The bytes one keyboard record with the virtual key `code`, the UTF-16 `unit`, the control key
// `state` and the `repeat` count turns into.
std::string key(std::uint16_t code, char16_t unit = 0, std::uint32_t state = 0, int repeat = 1) {
    char16_t pending = 0;

    return conhost::typed({code, unit, state, repeat}, pending);
}

}

TRIAL("Console without VT: named keys go as in conhost with VT") {
    same(key(0x26), std::string{"\x1b[A"});
    same(key(0x25, 0, 0x10), std::string{"\x1b[1;2D"});
    same(key(0x24, 0, 0x08), std::string{"\x1b[1;5H"});
    same(key(0x23), std::string{"\x1b[F"});
    same(key(0x2E), std::string{"\x1b[3~"});
    same(key(0x21, 0, 0x02), std::string{"\x1b[5;3~"});
    same(key(0x70), std::string{"\x1bOP"});
    same(key(0x73, 0, 0x10), std::string{"\x1b[1;2S"});
    same(key(0x74), std::string{"\x1b[15~"});
    same(key(0x79), std::string{"\x1b[21~"});
    same(key(0x7B), std::string{"\x1b[24~"});
    same(key(0x0D, u'\r'), std::string{"\r"});
    same(key(0x09, u'\t', 0x10), std::string{"\x1b[Z"});
    same(key(0x08, 0x08), std::string{"\x7f"});
    same(key(0x08, 0x7F, 0x08), std::string{"\x08"});
    same(key(0x1B, 0x1B), std::string{"\x1b"});
}

TRIAL("Console without VT: a character with Alt gets Esc, with AltGr it stays the layout's letter") {
    same(key('A', u'a', 0x02), std::string{"\x1b" "a"});
    same(key('A', u'ą', 0x01 | 0x08), std::string{"ą"});
    same(key('A', u'ą', 0x02 | 0x08), std::string{"ą"});
    same(key('C', 0x03, 0x08), std::string{"\x03"});
    same(key('C', 0, 0x02 | 0x08), std::string{"\x1b\x03"});
    same(key(0x20, 0, 0x08), std::string(1, '\0'));
    same(key(0x10), std::string{});
}

TRIAL("Console without VT: a surrogate pair is put together from two records, and the repeat count multiplies the character") {
    char16_t pending = 0;

    same(conhost::typed({0, 0xD83D, 0, 1}, pending), std::string{});
    same(conhost::typed({0, 0xDE00, 0, 1}, pending), std::string{"\xf0\x9f\x98\x80"});
    same(conhost::typed({0, 0xDE00, 0, 1}, pending), std::string{});

    same(key('X', u'x', 0, 3), std::string{"xxx"});
}

// --- Mouse ---------------------------------------------------------------------------------------

TRIAL("Console without VT: press and release come from the memory of buttons") {
    conhost::Pointing one;
    const conhost::Tracking wanted{true, true, false, true};

    same(one.pointed({{4, 2}, 0x1, 0, 0}, wanted), std::string{"\x1b[<0;5;3M"});
    same(one.pointed({{5, 2}, 0x1, 0x01, 0}, wanted), std::string{"\x1b[<32;6;3M"});
    same(one.pointed({{5, 2}, 0x3, 0, 0x10}, wanted), std::string{"\x1b[<6;6;3M"});
    same(one.pointed({{5, 2}, 0x0, 0, 0}, wanted), std::string{"\x1b[<0;6;3m\x1b[<2;6;3m"});
    same(one.pointed({{6, 2}, 0x0, 0x01, 0}, wanted), std::string{});
}

TRIAL("Console without VT: the wheel, motion without a button and a button pressed before tracking") {
    conhost::Pointing one;

    // Without 1006 nothing: the decoder does not know the old X10 encoding.
    same(one.pointed({{0, 0}, 0x00780000u, 0x04, 0}, {true, false, false, false}), std::string{});

    same(one.pointed({{0, 0}, 0x00780000u, 0x04, 0}, {true, false, false, true}), std::string{"\x1b[<64;1;1M"});
    same(one.pointed({{0, 0}, 0xFF880000u, 0x04, 0x08}, {true, false, false, true}), std::string{"\x1b[<81;1;1M"});
    same(one.pointed({{2, 3}, 0, 0x01, 0}, {true, true, true, true}), std::string{"\x1b[<35;3;4M"});
    same(one.pointed({{2, 3}, 0x1, 0, 0}, {}), std::string{});
    same(one.pointed({{2, 3}, 0x1, 0x01, 0}, {true, false, false, true}), std::string{});
    same(one.pointed({{2, 3}, 0x1, 0x01, 0}, {true, true, false, true}), std::string{"\x1b[<32;3;4M"});
    same(one.pointed({{2, 3}, 0x0, 0, 0}, {true, false, false, true}), std::string{"\x1b[<0;3;4m"});
}
