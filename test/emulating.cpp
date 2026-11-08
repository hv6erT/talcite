#include "tal/testing.hpp"

#include <cstddef>
#include <string>
#include <string_view>

#include "talcite/bytes.hpp"
#include "talcite/emulator.hpp"
#include "talcite/encoding.hpp"

using namespace talcite;
using namespace talcite::ui;

namespace {

// Feeds the text as the program's bytes.
Fed fed(Emulator& one, std::string_view said) { return one.feed(bytes(said)); }

// A line of the screen (or of the history, below zero) as text, without the spaces at its end; the
// second half of a wide character adds nothing.
std::string row(const Emulator& one, int at) {
    std::string made;

    for (const Cell& each : one.line(at))
        if (each.letter != 0) made += spelled(each.letter);

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

// The answers the terminal gave, as text.
std::string replied(const Fed& one) { return text(one.replies); }

}

// --- Text, wrapping and scrolling --------------------------------------------------------------

// Text lands where the cursor stands, and the cursor moves on after it.
TRIAL("Emulator: text lands at the cursor") {
    Emulator one{{20, 3}};

    (void)fed(one, "abc");

    SAME(row(one, 0), std::string{"abc"});
    OUGHT(one.cursor() == Spot{3, 0});
}

// A line exactly as wide as the screen leaves the cursor waiting at the last column, so a newline
// after it gives no empty line; the next character wraps.
TRIAL("Emulator: the last column waits for the next character before wrapping") {
    Emulator one{{5, 3}};

    (void)fed(one, "abcde");

    OUGHT(one.cursor() == Spot{4, 0});

    (void)fed(one, "\r\nx");

    SAME(row(one, 1), std::string{"x"});

    (void)fed(one, "\rabcdefg");

    SAME(row(one, 1), std::string{"abcde"});
    SAME(row(one, 2), std::string{"fg"});
}

// Lines that leave the top of the screen go into the history, newest at -1, and the history keeps
// only as many as it was given.
TRIAL("Emulator: lines scrolled away go into the history") {
    Emulator one{{10, 3}, 2};

    (void)fed(one, "1\r\n2\r\n3\r\n4\r\n5");

    SAME(row(one, 0), std::string{"3"});
    SAME(row(one, 2), std::string{"5"});
    SAME(one.history(), std::size_t{2});
    SAME(row(one, -1), std::string{"2"});
    SAME(row(one, -2), std::string{"1"});
    SAME(row(one, -3), std::string{});
}

// A broken UTF-8 character at the end of one chunk goes on in the next; an invalid byte becomes the
// replacement character.
TRIAL("Emulator: UTF-8 may break between chunks") {
    Emulator one{{10, 2}};

    const std::string word = "żółw";

    (void)fed(one, word.substr(0, 1));
    (void)fed(one, word.substr(1));

    SAME(row(one, 0), word);

    (void)fed(one, "\r\n\xff!");

    SAME(row(one, 1), std::string{"\xEF\xBF\xBD!"});
}

// The screen of a program reads UTF-8 by the one rule (`utf8::span`): a surrogate half written as
// three bytes is three broken bytes, not one character, and a character broken off by another byte
// is one replacement before that byte.
TRIAL("Emulator: broken UTF-8 is one replacement for each maximal subpart") {
    Emulator one{{10, 2}};

    (void)fed(one, "\xed\xa0\x80!");

    SAME(row(one, 0), std::string{"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD!"});

    (void)fed(one, "\r\n\xe2\x9d");
    (void)fed(one, "A");

    SAME(row(one, 1), std::string{"\xEF\xBF\xBD" "A"});
}

// A wide character takes two cells, and a combining mark joins the character before it.
TRIAL("Emulator: wide characters and combining marks") {
    Emulator one{{10, 2}};

    (void)fed(one, "日本x");

    SAME(row(one, 0), std::string{"日本x"});
    OUGHT(one.cursor() == Spot{5, 0});
    SAME(one.line(0)[1].letter, char32_t{0});

    (void)fed(one, "\r\ne\xCC\x81!");

    OUGHT(one.cursor() == Spot{2, 1});
    SAME(row(one, 1), std::string{"e\xCC\x81!"});
}

// --- Style ---------------------------------------------------------------------------------------

// The style of what follows: bold and red, then everything back by 0; the default colors stay
// transparent for whoever draws.
TRIAL("Emulator: SGR sets the style of what follows") {
    Emulator one{{10, 2}};

    (void)fed(one, "\x1b[1;31mX\x1b[0mY");

    const Cell x = one.line(0)[0];
    const Cell y = one.line(0)[1];

    OUGHT(x.style.bold);
    OUGHT(x.style.foreground == Color::indexed(1).value());
    OUGHT(!y.style.bold);
    OUGHT(y.style.foreground.transparent);
}

// The palette of 256 and true color, written with semicolons and with colons, and the bright
// colors.
TRIAL("Emulator: 256 colors and true color both ways") {
    Emulator one{{10, 2}};

    (void)fed(one, "\x1b[38;5;196ma\x1b[48;2;1;2;3mb\x1b[38:2::10:20:30mc\x1b[0;95md");

    OUGHT(one.line(0)[0].style.foreground == Color::indexed(196).value());
    OUGHT(one.line(0)[1].style.background == (Color{false, 1, 2, 3}));
    OUGHT(one.line(0)[2].style.foreground == (Color{false, 10, 20, 30}));
    OUGHT(one.line(0)[3].style.foreground == Color::indexed(13).value());
    OUGHT(one.line(0)[3].style.background.transparent);
}

// --- Cursor and erasing --------------------------------------------------------------------------

// The cursor goes where it is sent, and erasing clears from it.
TRIAL("Emulator: moving the cursor and erasing") {
    Emulator one{{10, 3}};

    (void)fed(one, "aaaaa\r\nbbbbb\r\nccccc");
    (void)fed(one, "\x1b[2;3H\x1b[K");

    SAME(row(one, 1), std::string{"bb"});

    (void)fed(one, "\x1b[1;2H\x1b[1K");

    SAME(row(one, 0), std::string{"  aaa"});

    (void)fed(one, "\x1b[J");

    SAME(row(one, 2), std::string{});
    OUGHT(one.cursor() == Spot{1, 0});

    (void)fed(one, "\x1b[2J");

    SAME(row(one, 0), std::string{});
}

// Inserting and deleting characters and lines move what stands around the cursor.
TRIAL("Emulator: inserting and deleting characters and lines") {
    Emulator one{{10, 4}};

    (void)fed(one, "abcdef\x1b[1;3H\x1b[2@XY");

    SAME(row(one, 0), std::string{"abXYcdef"});

    (void)fed(one, "\x1b[1;3H\x1b[3P");

    SAME(row(one, 0), std::string{"abdef"});

    (void)fed(one, "\x1b[2;1H2\r\n3\r\n4\x1b[2;1H\x1b[L");

    SAME(row(one, 1), std::string{});
    SAME(row(one, 2), std::string{"2"});
    SAME(row(one, 3), std::string{"3"});

    (void)fed(one, "\x1b[M\x1b[M");

    SAME(row(one, 1), std::string{"3"});
}

// A scroll region keeps the lines outside it in place, and those scrolled out of a region that does
// not start at the top go to no history.
TRIAL("Emulator: a scroll region scrolls only itself") {
    Emulator one{{10, 4}};

    (void)fed(one, "top\x1b[2;3r\x1b[2;1Ha\r\nb\r\nc");
    (void)fed(one, "\x1b[4;1Hbottom");

    SAME(row(one, 0), std::string{"top"});
    SAME(row(one, 1), std::string{"b"});
    SAME(row(one, 2), std::string{"c"});
    SAME(row(one, 3), std::string{"bottom"});
    SAME(one.history(), std::size_t{0});
}

// --- Screens, character sets and unknown sequences -----------------------------------------------

// A full-screen program takes the alternate screen and gives it back: the primary one and its
// cursor come back as they were, and the feeding says the screen switched.
TRIAL("Emulator: the alternate screen comes and goes") {
    Emulator one{{10, 3}};

    (void)fed(one, "shell$ ");

    const Fed in = fed(one, "\x1b[?1049h\x1b[Hfull");

    OUGHT(in.switched);
    OUGHT(one.modes().alternate);
    SAME(row(one, 0), std::string{"full"});
    SAME(one.history(), std::size_t{0});

    const Fed out = fed(one, "\x1b[?1049l");

    OUGHT(out.switched);
    OUGHT(!one.modes().alternate);
    SAME(row(one, 0), std::string{"shell$"});
    OUGHT(one.cursor() == Spot{7, 0});
}

// The DEC line-drawing set turns letters into frames, and ASCII comes back.
TRIAL("Emulator: the line-drawing set") {
    Emulator one{{10, 2}};

    (void)fed(one, "\x1b(0lqk\x1b(Bq");

    SAME(row(one, 0), std::string{"┌─┐q"});
}

// An unknown sequence and a string nobody uses vanish whole: none of their letters lands on the
// screen.
TRIAL("Emulator: unknown sequences leave nothing behind") {
    Emulator one{{20, 2}};

    (void)fed(one, "a\x1b[?9999h\x1bP1$r0m\x1b\\b\x1b]52;c;aGk=\x07" "c\x1b[>4;1md\x1b[3 qe");

    SAME(row(one, 0), std::string{"abcde"});
}

// The modes programs set: the keys of the cursor, the bracketed paste, the mouse, the cursor shown.
TRIAL("Emulator: the modes a program sets") {
    Emulator one{{10, 2}};

    (void)fed(one, "\x1b[?1h\x1b[?2004h\x1b[?1002;1006h\x1b[?25l");

    OUGHT(one.modes().cursor_keys);
    OUGHT(one.modes().paste);
    OUGHT(one.modes().mouse == Mousing::drags);
    OUGHT(one.modes().sgr_mouse);
    OUGHT(!one.modes().cursor);

    (void)fed(one, "\x1b[?1l\x1b[?2004l\x1b[?1002l\x1b[?25h");

    OUGHT(!one.modes().cursor_keys);
    OUGHT(!one.modes().paste);
    OUGHT(one.modes().mouse == Mousing::none);
    OUGHT(one.modes().cursor);
}

// --- What the terminal tells -----------------------------------------------------------------

// The cursor's shape a program asks for (DECSCUSR): a block, an underline or a bar, blinking at the
// odd numbers; 0 and a reset give the terminal's usual one back.
TRIAL("Emulator: the cursor's shape a program asks for") {
    Emulator one{{10, 3}};

    OUGHT(one.modes().caret == CaretShape{});

    (void)fed(one, "\x1b[4 q");

    OUGHT(one.modes().caret == (CaretShape{Caret::underline, false}));

    (void)fed(one, "\x1b[5 q");

    OUGHT(one.modes().caret == (CaretShape{Caret::bar, true}));

    (void)fed(one, "\x1b[1 q");

    OUGHT(one.modes().caret == (CaretShape{Caret::block, true}));

    (void)fed(one, "\x1b[0 q");

    OUGHT(one.modes().caret == CaretShape{});

    (void)fed(one, "\x1b[2 q\x1b" "c");

    OUGHT(one.modes().caret == CaretShape{});
}

// The terminal answers questions: where the cursor stands, whether it is well, what it is.
TRIAL("Emulator: answers to the program's questions") {
    Emulator one{{20, 5}};

    (void)fed(one, "\x1b[3;5H");

    SAME(replied(fed(one, "\x1b[6n")), std::string{"\x1b[3;5R"});
    SAME(replied(fed(one, "\x1b[5n")), std::string{"\x1b[0n"});
    SAME(replied(fed(one, "\x1b[c")), std::string{"\x1b[?62;22c"});
    SAME(replied(fed(one, "\x1b[>c")), std::string{"\x1b[>1;10;0c"});
    SAME(fed(one, "\x07\x07").bells, 2);
}

// The title, and the folder the shell stands in: as an address with its escapes (OSC 7), also with
// a Windows drive, and as a path (OSC 9;9).
TRIAL("Emulator: the title and the folder") {
    Emulator one{{20, 2}};

    OUGHT(fed(one, "\x1b]0;mc - ~/Filmy\x07").titled);
    SAME(one.title(), std::string{"mc - ~/Filmy"});

    OUGHT(fed(one, "\x1b]7;file://laptop/home/hubert/Moje%20filmy\x1b\\").moved);
    SAME(one.folder(), std::string{"/home/hubert/Moje filmy"});

    (void)fed(one, "\x1b]7;file://laptop/C:/Users/huber\x07");

    SAME(one.folder(), std::string{"C:/Users/huber"});

    OUGHT(fed(one, "\x1b]9;9;\"C:\\Filmy\"\x07").moved);
    SAME(one.folder(), std::string{"C:\\Filmy"});
    OUGHT(!fed(one, "\x1b]9;9;C:\\Filmy\x07").moved);
}

// The marks of a shell's prompt: where the prompt begins, where the command does, its output and
// its end with the code. While the shell waits, the screen knows on which row the prompt begins.
TRIAL("Emulator: the marks of a shell's prompt") {
    Emulator one{{20, 4}};

    const Fed first = fed(one, "\x1b]133;A\x07~$ \x1b]133;B\x07");

    OUGHT(first.marks.size() == 2);
    OUGHT(first.marks[0].kind == Marked::prompt);
    OUGHT(first.marks[1].kind == Marked::input);
    OUGHT(one.waiting());
    OUGHT(one.prompt() == std::optional<int>{0});

    const Fed ran = fed(one, "ls\r\n\x1b]133;C\x07" "a b\r\n\x1b]133;D;3\x07");

    OUGHT(ran.marks.size() == 2);
    OUGHT(ran.marks[0].kind == Marked::output);
    OUGHT(ran.marks[1].kind == Marked::done);
    OUGHT(ran.marks[1].code == std::optional<int>{3});
    OUGHT(!one.waiting());
    OUGHT(!one.prompt().has_value());

    (void)fed(one, "\x1b]133;A\x07~$ \x1b]133;B\x07");

    OUGHT(one.prompt() == std::optional<int>{2});

    // Scrolled away with the screen, the prompt row moves up with it.
    (void)fed(one, "\x1b[4;1H\r\n");

    OUGHT(one.prompt() == std::optional<int>{1});
}

// --- Size
// ------------------------------------------------------------------------------------------

// A lower screen sends its top lines into the history and keeps the cursor in sight; a higher one
// takes them back, so a terminal of three rows opened to the whole screen shows what scrolled away.
TRIAL("Emulator: resizing keeps the cursor in sight and brings the history back") {
    Emulator one{{10, 6}};

    (void)fed(one, "1\r\n2\r\n3\r\n4\r\n5$ ");

    one.resize({10, 3});

    SAME(row(one, 0), std::string{"3"});
    SAME(row(one, 2), std::string{"5$"});
    OUGHT(one.cursor() == Spot{3, 2});
    SAME(one.history(), std::size_t{2});

    one.resize({10, 6});

    SAME(row(one, 0), std::string{"1"});
    SAME(row(one, 4), std::string{"5$"});
    OUGHT(one.cursor() == Spot{3, 4});
    SAME(one.history(), std::size_t{0});

    one.resize({4, 6});

    SAME(row(one, 4), std::string{"5$"});
    OUGHT(one.size() == (Size{4, 6}));
}

// A lower screen drops the empty lines under the cursor before anything goes into the history.
TRIAL("Emulator: resizing drops empty lines under the cursor first") {
    Emulator one{{10, 6}};

    (void)fed(one, "1\r\n2$ ");

    one.resize({10, 3});

    SAME(row(one, 0), std::string{"1"});
    SAME(row(one, 1), std::string{"2$"});
    SAME(one.history(), std::size_t{0});
}
