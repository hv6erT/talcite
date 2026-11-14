#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/emulator.hpp"
#include "tal/style.hpp"
#include "tal/text.hpp"

#include "palette.hpp"

#include "tal/testing.hpp"

// The screen of a terminal as a program sees it: text, wrapping and the history, UTF-8 in pieces,
// wide characters and marks, looks and colors, the cursor, erasing and inserting, the scroll
// region, both screens, the line-drawing set, unknown sequences, modes and answers, the title, the
// folder, links and the stages of a shell; the bounds that hold against a hostile program; and new
// sizes, which lay the text out anew.

namespace {

using tal::Caret;
using tal::Cell;
using tal::Color;
using tal::Emulator;
using tal::Fed;
using tal::Mousing;
using tal::Size;
using tal::Spot;
using tal::Stage;

// Feeds `said` as the program's bytes.
Fed fed(Emulator& one, std::string_view said) { return one.feed(tal::bytes(said)); }

// The row `at` of the screen (or of the history, below zero) as text, without the spaces at its
// end; the second half of a wide character adds nothing.
std::string row(const Emulator& one, int at) {
    std::string made;

    for (const Cell& each : one.row(at).cells) made += each.cluster;

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

// The answers the terminal gave, as text.
std::string replied(const Fed& one) {
    std::string made;

    for (const std::byte each : one.replies) made += static_cast<char>(each);

    return made;
}

// The cell `x` of the row `at`.
Cell cell(const Emulator& one, int at, int x) { return one.row(at).cells.at(static_cast<std::size_t>(x)); }

// Every row of the history and the screen as text, oldest first.
std::vector<std::string> all(const Emulator& one) {
    std::vector<std::string> made;

    for (int at = -static_cast<int>(one.history()); at < one.size().high; ++at) made.push_back(row(one, at));

    return made;
}

}

// --- Text, wrapping and the history --------------------------------------------------------------

TRIAL("Emulator: text lands at the cursor") {
    Emulator one{{20, 3}};

    (void)fed(one, "abc");

    SAME(row(one, 0), std::string{"abc"});
    OUGHT(one.cursor() == Spot{3, 0});
}

TRIAL("Emulator: the last column waits for the next character before wrapping, and the row goes on") {
    Emulator one{{5, 3}};

    (void)fed(one, "abcde");

    OUGHT(one.cursor() == Spot{4, 0});
    OUGHT(!one.row(0).wrapped);

    (void)fed(one, "\r\nx");

    SAME(row(one, 1), std::string{"x"});
    OUGHT(!one.row(0).wrapped);

    (void)fed(one, "\rabcdefg");

    SAME(row(one, 1), std::string{"abcde"});
    SAME(row(one, 2), std::string{"fg"});
    OUGHT(one.row(1).wrapped);
    OUGHT(!one.row(2).wrapped);

    (void)fed(one, "\x1b[2;3H\x1b[K");

    OUGHT(!one.row(1).wrapped);
}

TRIAL("Emulator: lines scrolled away go into the history, as they were") {
    Emulator one{{10, 3}, 2};

    (void)fed(one, "1\r\n2\r\n3\r\n4\r\n5");

    SAME(row(one, 0), std::string{"3"});
    SAME(row(one, 2), std::string{"5"});
    SAME(one.history(), std::size_t{2});
    SAME(row(one, -1), std::string{"2"});
    SAME(row(one, -2), std::string{"1"});
    SAME(row(one, -3), std::string{});
    SAME(one.row(-3).cells.size(), std::size_t{10});

    Emulator wrapped{{4, 2}};

    (void)fed(wrapped, "abcdef\r\n\r\n");

    OUGHT(wrapped.row(-2).wrapped);
    SAME(row(wrapped, -2), std::string{"abcd"});
    OUGHT(!wrapped.row(-1).wrapped);
    SAME(row(wrapped, -1), std::string{"ef"});
}

TRIAL("Emulator: the history gives back exactly what left the screen, looks, links and wide characters") {
    std::mt19937 random{540};
    const std::vector<std::string> pieces{"a", "ż", "日", "e\xCC\x81", " ", "\x1b[1m", "\x1b[31m", "\x1b[48;2;1;2;3m", "\x1b[0m", "\x1b]8;;https://x.pl/a\x07",
                                          "\x1b]8;;\x07", "\t", "\x1b[3m", "\x1b[K"};

    for (int round = 0; round < 200; ++round) {
        Emulator one{{12, 4}, 1000};
        std::string said;

        for (int piece = 0, many = static_cast<int>(random() % 60); piece < many; ++piece) said += pieces[random() % pieces.size()];

        (void)fed(one, said);

        std::vector<Emulator::Row> before;

        for (int at = 0; at < 4; ++at) before.push_back(one.row(at));

        (void)fed(one, "\x1b[0m\x1b]8;;\x07\x1b[4;1H\r\n\r\n\r\n\r\n");

        for (int at = 0; at < 4; ++at) OUGHT(one.row(at - 4) == before[static_cast<std::size_t>(at)]);
    }
}

TRIAL("Emulator: UTF-8 may break between chunks") {
    Emulator one{{10, 2}};

    const std::string word = "żółw";

    (void)fed(one, word.substr(0, 1));
    (void)fed(one, word.substr(1));

    SAME(row(one, 0), word);

    (void)fed(one, "\r\n\xff!");

    SAME(row(one, 1), std::string{"\xEF\xBF\xBD!"});
}

TRIAL("Emulator: broken UTF-8 is one replacement for each maximal subpart") {
    Emulator one{{10, 2}};

    (void)fed(one, "\xed\xa0\x80!");

    SAME(row(one, 0), std::string{"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD!"});

    (void)fed(one, "\r\n\xe2\x9d");
    (void)fed(one, "A");

    SAME(row(one, 1), std::string{"\xEF\xBF\xBD" "A"});
}

TRIAL("Emulator: wide characters and combining marks; a wide character at the last column goes on") {
    Emulator one{{10, 3}};

    (void)fed(one, "日本x");

    SAME(row(one, 0), std::string{"日本x"});
    OUGHT(one.cursor() == Spot{5, 0});
    SAME(cell(one, 0, 1).cluster, std::string{});

    (void)fed(one, "\r\ne\xCC\x81!");

    OUGHT(one.cursor() == Spot{2, 1});
    SAME(row(one, 1), std::string{"e\xCC\x81!"});

    (void)fed(one, "\r\n123456789日");

    SAME(row(one, 2), std::string{"日"});
    SAME(row(one, 1), std::string{"123456789"});
    OUGHT(one.row(1).wrapped);

    (void)fed(one, "\x1b[3;2H日");

    SAME(row(one, 2), std::string{" 日"});
}

TRIAL("Emulator: the controls of C1 do nothing and show nothing") {
    Emulator one{{20, 2}};

    (void)fed(one, "a\xC2\x9B" "31mb\xC2\x85" "c");

    SAME(row(one, 0), std::string{"a31mbc"});
    OUGHT(!cell(one, 0, 4).look.ink.has_value());
    OUGHT(one.cursor() == Spot{6, 0});
}

// --- Looks ---------------------------------------------------------------------------------------

TRIAL("Emulator: SGR sets the look of what follows") {
    Emulator one{{10, 2}};

    (void)fed(one, "\x1b[1;31mX\x1b[0mY\x1b[4:3;9;7;3;2mZ\x1b[22;23;24;27;29mW");

    OUGHT(cell(one, 0, 0).look.bold);
    OUGHT(cell(one, 0, 0).look.ink == tal::palette::indexed(1));
    OUGHT(!cell(one, 0, 1).look.bold);
    OUGHT(!cell(one, 0, 1).look.ink.has_value());

    const tal::Look z = cell(one, 0, 2).look;

    OUGHT(z.under && z.crossed && z.reverse && z.italic && z.faint);

    const tal::Look w = cell(one, 0, 3).look;

    OUGHT(!w.under && !w.reverse && !w.italic && !w.faint && !w.bold && !w.crossed);
}

TRIAL("Emulator: 256 colors and true color both ways, and colors past the palette are none") {
    Emulator one{{10, 2}};

    (void)fed(one, "\x1b[38;5;196ma\x1b[48;2;1;2;3mb\x1b[38:2::10:20:30mc\x1b[0;95md\x1b[38;5;300me\x1b[38:5:999mf\x1b[38;2;999;0;0mg");

    OUGHT(cell(one, 0, 0).look.ink == tal::palette::indexed(196));
    OUGHT(cell(one, 0, 1).look.paper == (Color{1, 2, 3}));
    OUGHT(cell(one, 0, 2).look.ink == (Color{10, 20, 30}));
    OUGHT(cell(one, 0, 3).look.ink == tal::palette::indexed(13));
    OUGHT(!cell(one, 0, 3).look.paper.has_value());
    OUGHT(!cell(one, 0, 4).look.ink.has_value());
    OUGHT(!cell(one, 0, 5).look.ink.has_value());
    OUGHT(cell(one, 0, 6).look.ink == (Color{255, 0, 0}));
}

// --- The cursor and erasing ----------------------------------------------------------------------

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

    (void)fed(one, "\x1b[41m\x1b[2J");

    OUGHT(cell(one, 1, 1).look.paper == tal::palette::indexed(1));
}

TRIAL("Emulator: inserting and deleting characters and lines, no half of a wide character left") {
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

    (void)fed(one, "\x1b[1;1H\x1b[2K12345678日\x1b[1;1H\x1b[1@");

    SAME(row(one, 0), std::string{" 12345678"});

    (void)fed(one, "\x1b[1;1H\x1b[2K日本\x1b[1;2H\x1b[1P");

    SAME(row(one, 0), std::string{" 本"});

    (void)fed(one, "\x1b[1;1H\x1b[2K日本\x1b[1;2H\x1b[1X");

    SAME(row(one, 0), std::string{"  本"});
}

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

TRIAL("Emulator: tab stops, set, cleared and walked both ways") {
    Emulator one{{20, 3}};

    (void)fed(one, "\tx");

    OUGHT(one.cursor() == (Spot{9, 0}));

    (void)fed(one, "\x1b[1;4H\x1bH\x1b[1;1H\ty\x1b[2I");

    SAME(cell(one, 0, 3).cluster, std::string{"y"});
    OUGHT(one.cursor() == (Spot{16, 0}));

    (void)fed(one, "\x1b[Z");

    OUGHT(one.cursor() == (Spot{8, 0}));

    (void)fed(one, "\x1b[2Z");

    OUGHT(one.cursor() == (Spot{0, 0}));

    (void)fed(one, "\x1b[1;4H\x1b[g\x1b[1;1H\t");

    OUGHT(one.cursor() == (Spot{8, 0}));

    (void)fed(one, "\x1b[3g\x1b[1;1H\t");

    OUGHT(one.cursor() == (Spot{19, 0}));
}

TRIAL("Emulator: the saved cursor brings back its place, look and character set, one for each screen") {
    Emulator one{{10, 3}};

    (void)fed(one, "ab\x1b" "7\x1b[31m\x1b(0\x1b[3;5Hcd\x1b" "8q");

    SAME(row(one, 0), std::string{"abq"});
    OUGHT(!cell(one, 0, 2).look.ink.has_value());

    (void)fed(one, "\x1b[2;2H\x1b[s\x1b[1;1H\x1b[uz");

    SAME(cell(one, 1, 1).cluster, std::string{"z"});

    (void)fed(one, "\x1b[?1049h\x1b" "8w");

    SAME(cell(one, 0, 0).cluster, std::string{"w"});

    (void)fed(one, "\x1b[?1049l\x1b[?1048h\x1b[3;3H\x1b[?1048lv");

    SAME(cell(one, 1, 2).cluster, std::string{"v"});
}

TRIAL("Emulator: the origin mode measures from the scroll region, and the cursor keeps to it") {
    Emulator one{{10, 5}};

    (void)fed(one, "\x1b[2;3r\x1b[?6h\x1b[1;1Hx");

    SAME(row(one, 1), std::string{"x"});
    SAME(replied(fed(one, "\x1b[6n")), std::string{"\x1b[1;2R"});

    (void)fed(one, "\x1b[9;1Hy\x1b[1;5d");

    SAME(row(one, 2), std::string{"y"});
    OUGHT(one.cursor() == (Spot{1, 1}));

    (void)fed(one, "\x1b[?6l\x1b[9;1H");

    OUGHT(one.cursor() == (Spot{0, 4}));

    (void)fed(one, "\x1b[3;3H\x1b[5A");

    OUGHT(one.cursor() == (Spot{2, 1}));

    (void)fed(one, "\x1b[5B");

    OUGHT(one.cursor() == (Spot{2, 2}));

    (void)fed(one, "\x1b[5;1H\x1b[9A");

    OUGHT(one.cursor() == (Spot{0, 1}));

    (void)fed(one, "\x1b[1;1H\x1b[9e");

    OUGHT(one.cursor() == (Spot{0, 2}));

    (void)fed(one, "\x1b[4;4H\x1b[2E");

    OUGHT(one.cursor() == (Spot{0, 4}));

    (void)fed(one, "\x1b[2F\x1b[3G\x1b[2a");

    OUGHT(one.cursor() == (Spot{4, 2}));

    (void)fed(one, "\x1b[3;2r\x1b[2;3H\x1b[?6h");

    OUGHT(one.cursor() == (Spot{0, 1}));
}

TRIAL("Emulator: wrapping off, insert mode, the newline mode and repeating the last character") {
    Emulator one{{5, 4}};

    (void)fed(one, "\x1b[?7labcdefg\x1b[?7h");

    SAME(row(one, 0), std::string{"abcdg"});
    SAME(row(one, 1), std::string{});

    (void)fed(one, "\x1b[2;1Habc\x1b[2;1H\x1b[4hxy\x1b[4l");

    SAME(row(one, 1), std::string{"xyabc"});

    (void)fed(one, "\x1b[3;3H\x1b[20ha\nb\x1b[20l");

    SAME(row(one, 3), std::string{"b"});

    (void)fed(one, "\x1b[3;1H\x1b[2K\x1b[1;1H\x1b[2Kz\x1b[3b");

    SAME(row(one, 0), std::string{"zzzz"});

    Emulator fresh{{5, 2}};

    (void)fed(fresh, "\x1b[3b");

    SAME(row(fresh, 0), std::string{});
}

TRIAL("Emulator: index, next line and reverse index, scrolling up and down") {
    Emulator one{{10, 3}};

    (void)fed(one, "a\x1b" "Db\x1b" "Ec\x1b[1;1H\x1bMz");

    SAME(row(one, 0), std::string{"z"});
    SAME(row(one, 1), std::string{"a"});
    SAME(row(one, 2), std::string{" b"});
    SAME(one.history(), std::size_t{0});

    (void)fed(one, "\x1b[2S");

    SAME(row(one, 0), std::string{" b"});
    SAME(one.history(), std::size_t{2});

    (void)fed(one, "\x1b[44m\x1b[T");

    SAME(row(one, 1), std::string{" b"});
    OUGHT(cell(one, 0, 3).look.paper == tal::palette::indexed(4));
}

TRIAL("Emulator: the alignment test, both resets and the alternate screens of 47 and 1047") {
    Emulator one{{4, 2}};

    (void)fed(one, "x\r\n\r\n\x1b#8");

    SAME(row(one, 0), std::string{"EEEE"});

    (void)fed(one, "\x1b[4h\x1b[?6h\x1b[?7l\x1b[?25l\x1b[?1h\x1b=\x1b[1m\x1b(0\x1b[!p");

    OUGHT(!one.modes().insert && !one.modes().origin && one.modes().wrap && one.modes().cursor && !one.modes().cursor_keys && !one.modes().keypad);

    (void)fed(one, "q");

    SAME(cell(one, 0, 0).cluster, std::string{"q"});
    OUGHT(!cell(one, 0, 0).look.bold);

    (void)fed(one, "\x1b[?47hab\x1b[?47l\x1b[?47h");

    SAME(row(one, 0), std::string{" ab"});

    (void)fed(one, "\x1b[?47l\x1b[?1047h");

    SAME(row(one, 0), std::string{});

    (void)fed(one, "\x1b[?1000h\x1b=\x1b" "c");

    OUGHT(!one.modes().alternate && one.modes().mouse == Mousing::none && !one.modes().keypad);
    SAME(row(one, 0), std::string{});
    SAME(one.history(), std::size_t{1});

    (void)fed(one, "\x1b[?1003h");

    OUGHT(one.modes().mouse == Mousing::moves);

    (void)fed(one, "\x1b[?1000h");

    OUGHT(one.modes().mouse == Mousing::clicks);

    (void)fed(one, "\x1b=");

    OUGHT(one.modes().keypad);

    (void)fed(one, "\x1b>");

    OUGHT(!one.modes().keypad);
}

// --- Screens, character sets and unknown sequences -----------------------------------------------

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

TRIAL("Emulator: the line-drawing set") {
    Emulator one{{10, 2}};

    (void)fed(one, "\x1b(0lqk\x1b(Bq\x1b)0\x0Eq\x0Fq");

    SAME(row(one, 0), std::string{"┌─┐q─q"});

    (void)fed(one, "\x1b)0\x1b)B\x0Eq\x0F");

    SAME(row(one, 0), std::string{"┌─┐q─qq"});
}

TRIAL("Emulator: unknown sequences leave nothing behind") {
    Emulator one{{20, 2}};

    (void)fed(one, "a\x1b[?9999h\x1bP1$r0m\x1b\\b\x1b]52;c;aGk=\x07" "c\x1b[>4;1md\x1b[3 qe\x1b[21tf\x1b]10;?\x07g");

    SAME(row(one, 0), std::string{"abcdefg"});
}

TRIAL("Emulator: the modes a program sets") {
    Emulator one{{10, 2}};

    (void)fed(one, "\x1b[?1h\x1b[?2004h\x1b[?1002;1006h\x1b[?25l\x1b[?1004h\x1b[?2026h");

    OUGHT(one.modes().cursor_keys);
    OUGHT(one.modes().paste);
    OUGHT(one.modes().mouse == Mousing::drags);
    OUGHT(one.modes().sgr_mouse);
    OUGHT(!one.modes().cursor);
    OUGHT(one.modes().focus);
    OUGHT(one.modes().synchronized);

    (void)fed(one, "\x1b[?1l\x1b[?2004l\x1b[?1002l\x1b[?25h\x1b[?2026l");

    OUGHT(!one.modes().cursor_keys);
    OUGHT(!one.modes().paste);
    OUGHT(one.modes().mouse == Mousing::none);
    OUGHT(one.modes().cursor);
    OUGHT(!one.modes().synchronized);
}

// --- What the terminal tells ---------------------------------------------------------------------

TRIAL("Emulator: the cursor's form a program asks for") {
    Emulator one{{10, 3}};

    OUGHT(one.modes().form == Caret::Form::usual && one.modes().blink);

    (void)fed(one, "\x1b[4 q");

    OUGHT(one.modes().form == Caret::Form::underline && !one.modes().blink);

    (void)fed(one, "\x1b[5 q");

    OUGHT(one.modes().form == Caret::Form::bar && one.modes().blink);

    (void)fed(one, "\x1b[1 q");

    OUGHT(one.modes().form == Caret::Form::block && one.modes().blink);

    (void)fed(one, "\x1b[0 q");

    OUGHT(one.modes().form == Caret::Form::usual && one.modes().blink);

    (void)fed(one, "\x1b[2 q\x1b" "c");

    OUGHT(one.modes().form == Caret::Form::usual);
}

TRIAL("Emulator: answers to the program's questions, about modes too") {
    Emulator one{{20, 5}};

    (void)fed(one, "\x1b[3;5H");

    SAME(replied(fed(one, "\x1b[6n")), std::string{"\x1b[3;5R"});
    SAME(replied(fed(one, "\x1b[5n")), std::string{"\x1b[0n"});
    SAME(replied(fed(one, "\x1b[c")), std::string{"\x1b[?62;22c"});
    SAME(replied(fed(one, "\x1b[>c")), std::string{"\x1b[>1;10;0c"});
    SAME(fed(one, "\x07\x07").bells, 2);

    (void)fed(one, "\x1b[?2026h\x1b[4h");

    SAME(replied(fed(one, "\x1b[?2026$p")), std::string{"\x1b[?2026;1$y"});
    SAME(replied(fed(one, "\x1b[?2004$p")), std::string{"\x1b[?2004;2$y"});
    SAME(replied(fed(one, "\x1b[?9999$p")), std::string{"\x1b[?9999;0$y"});
    SAME(replied(fed(one, "\x1b[4$p")), std::string{"\x1b[4;1$y"});
    SAME(replied(fed(one, "\x1b[20$p")), std::string{"\x1b[20;2$y"});
    SAME(replied(fed(one, "\x1b[21t\x1b[14t\x1b]11;?\x07\x1b]52;c;?\x07")), std::string{});
}

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

TRIAL("Emulator: links carry their address over the cells written under them; an escape ends one") {
    Emulator one{{30, 2}};

    (void)fed(one, "\x1b]8;id=1;https://x.pl/a\x1b\\ab\x1b]8;;\x1b\\c\x1b]8;;https://x.pl/a\x07" "d\x1b]8;;\x07");

    const std::uint32_t link = cell(one, 0, 0).link;

    OUGHT(link != 0);
    SAME(cell(one, 0, 1).link, link);
    SAME(cell(one, 0, 2).link, std::uint32_t{0});
    SAME(cell(one, 0, 3).link, link);
    SAME(one.address(link), std::string_view{"https://x.pl/a"});
    SAME(one.address(0), std::string_view{});
    SAME(one.address(999), std::string_view{});

    (void)fed(one, "\x1b]8;;https://x.pl/\x1b[31m\x07" "e\x1b]8;;https://x.pl/ż\x07" "f\x1b]8;;" + std::string(2049, 'a') + "\x07g\x1b]8;;" + std::string(2048, 'b') + "\x07h");

    SAME(one.address(cell(one, 0, 4).link), std::string_view{"https://x.pl/"});
    OUGHT(cell(one, 0, 4).look.ink == tal::palette::indexed(1));
    SAME(cell(one, 0, 5).link, std::uint32_t{0});
    SAME(cell(one, 0, 6).link, std::uint32_t{0});
    OUGHT(cell(one, 0, 7).link != 0);
    SAME(row(one, 0), std::string{"abcdefgh"});
}

TRIAL("Emulator: the stages of a shell's prompt") {
    Emulator one{{20, 4}};

    const Fed first = fed(one, "\x1b]133;A\x07~$ \x1b]133;B\x07");

    SAME(first.stages.size(), std::size_t{2});
    OUGHT(first.stages[0].stage == Stage::prompt);
    OUGHT(first.stages[1].stage == Stage::input);
    OUGHT(one.waiting());
    OUGHT(one.prompt() == std::optional<int>{0});

    const Fed ran = fed(one, "ls\r\n\x1b]133;C\x07" "a b\r\n\x1b]133;D;3\x07");

    SAME(ran.stages.size(), std::size_t{2});
    OUGHT(ran.stages[0].stage == Stage::output);
    OUGHT(ran.stages[1].stage == Stage::done);
    OUGHT(ran.stages[1].code == std::optional<int>{3});
    OUGHT(!one.waiting());
    OUGHT(!one.prompt().has_value());

    (void)fed(one, "\x1b]133;A\x07~$ \x1b]133;B\x07");

    OUGHT(one.prompt() == std::optional<int>{2});

    (void)fed(one, "\x1b[4;1H\r\n");

    OUGHT(one.prompt() == std::optional<int>{1});

    SAME(one.scrolled(), std::uint64_t{1});

    (void)fed(one, "\x1b[3J");

    OUGHT(one.prompt() == std::optional<int>{1});
    SAME(one.scrolled(), std::uint64_t{1});

    (void)fed(one, "\r\n");

    SAME(one.scrolled(), std::uint64_t{2});
}

TRIAL("Emulator: a mark joins the character printed last, only where the cursor stands after it") {
    Emulator one{{5, 3}};

    (void)fed(one, "abcde\xCC\x81");

    SAME(cell(one, 0, 4).cluster, std::string{"e\xCC\x81"});

    (void)fed(one, "\x1b[?7l\x1b[2;1Hfghij\xCC\x81\x1b[?7h");

    SAME(cell(one, 1, 4).cluster, std::string{"j\xCC\x81"});

    (void)fed(one, "\x1b[3;1H日\xCC\x81" "a \xCC\x81" "b\xE1\x85\xA0\x1b[3;1H\xCC\x81");

    SAME(cell(one, 2, 0).cluster, std::string{"日\xCC\x81"});
    SAME(cell(one, 2, 3).cluster, std::string{" "});
    SAME(cell(one, 2, 4).cluster, std::string{"b"});

    Emulator moved{{10, 2}};

    (void)fed(moved, "ab\x1b[1;5H\xCC\x81\r\n");

    SAME(cell(moved, 0, 1).cluster, std::string{"b"});

    Emulator other{{10, 2}};

    (void)fed(other, "\x1b[?47hxy\x1b[?47l\x1b[1;1Hab\x1b[?47h\xCC\x81");

    SAME(cell(other, 0, 1).cluster, std::string{"y"});

    (void)fed(other, "\x1b[?47lcd");

    other.resize({10, 3});

    (void)fed(other, "\xCC\x81");

    SAME(cell(other, 0, 3).cluster, std::string{"d"});
}

TRIAL("Emulator: a backspace, a cursor that moves or wrapping turned off end the wait for a wrap") {
    Emulator one{{5, 3}};

    (void)fed(one, "abcde\bx");

    SAME(row(one, 0), std::string{"abcxe"});
    SAME(row(one, 1), std::string{});

    (void)fed(one, "\x1b[2;1Hfghij\x1b[?7lk\x1b[?7h");

    SAME(row(one, 1), std::string{"fghik"});
    SAME(row(one, 2), std::string{});

    (void)fed(one, "\x1b[3;1H\x1b[?7lvwxyz1\x1b[?7h2");

    SAME(row(one, 2), std::string{"vwxy2"});

    Emulator saved{{5, 3}};

    (void)fed(saved, "abcde\x1b" "7\x1b[2;1H\x1b" "8x");

    SAME(row(saved, 0), std::string{"abcde"});
    SAME(row(saved, 1), std::string{"x"});
}

TRIAL("Emulator: what changes a row's end tells whether it goes on, also on the alternate screen") {
    Emulator one{{5, 3}};

    (void)fed(one, "abcdefg\x1b[1;1H\x1b[4hz\x1b[4l");

    OUGHT(!one.row(0).wrapped);

    (void)fed(one, "\x1b[1;1H\x1b[2Kabcdefg\x1b[1;3H\x1b[5X");

    OUGHT(!one.row(0).wrapped);

    (void)fed(one, "\x1b[1;1H\x1b[2Kabc日\x1b[1;1H\x1b[4hz\x1b[4l");

    SAME(row(one, 0), std::string{"zabc"});
    SAME(cell(one, 0, 4).cluster, std::string{" "});

    (void)fed(one, "\x1b[1;1H\x1b[2K日\x1b[1;1Hx");

    SAME(cell(one, 0, 1).cluster, std::string{" "});

    (void)fed(one, "\x1b[1;1H\x1b[2Ka日\x1b[1;2H\x1b[P");

    SAME(cell(one, 0, 1).cluster, std::string{" "});

    (void)fed(one, "\x1b[?1049h\x1b[Habcdefg");

    OUGHT(one.row(0).wrapped);

    one.resize({6, 3});

    OUGHT(!one.row(0).wrapped);
}

TRIAL("Emulator: sequences with parameters out of place, intermediates or empty parameters") {
    Emulator one{{10, 4}};

    (void)fed(one, "\x1b[1?h\x1b[?1 h");

    OUGHT(!one.modes().cursor_keys);
    SAME(replied(fed(one, "\x1b[> c\x1b[1c")), std::string{});

    (void)fed(one, "x\x1b[2 J");

    SAME(row(one, 0), std::string{"x"});

    (void)fed(one, "\x1b[;5H");

    OUGHT(one.cursor() == (Spot{4, 0}));

    std::string many = "\x1b[";

    for (int at = 0; at < 32; ++at) many += "0;";

    (void)fed(one, many + "1my\x1b[0m");

    OUGHT(!cell(one, 0, 4).look.bold);

    (void)fed(one, "\x1b[1;4294967297H");

    OUGHT(one.cursor() == (Spot{9, 0}));

    (void)fed(one, "\x1b]2;abc\x18" "def\x07");

    SAME(one.title(), std::string{});
    SAME(row(one, 0), std::string{"x   y    d"});
    SAME(row(one, 1), std::string{"ef"});
}

TRIAL("Emulator: moving left and up a line, the margins set wrong, and lines inserted outside them") {
    Emulator one{{10, 4}};

    (void)fed(one, "\x1b[1;5H\x1b[2D");

    OUGHT(one.cursor() == (Spot{2, 0}));

    (void)fed(one, "\x1b[3;5H\x1b[F");

    OUGHT(one.cursor() == (Spot{0, 1}));

    (void)fed(one, "ab\x1b" "Ec");

    OUGHT(one.cursor() == (Spot{1, 2}));

    (void)fed(one, "\x1b[3;3H\x1b[2;2r");

    OUGHT(one.cursor() == (Spot{2, 2}));

    (void)fed(one, "\x1b[?6h\x1b[2;3r");

    OUGHT(one.cursor() == (Spot{0, 1}));

    (void)fed(one, "\x1b[?6l\x1b[4;1Hq\x1b[L\x1b[M");

    SAME(row(one, 3), std::string{"q"});
    OUGHT(one.cursor() == (Spot{1, 3}));

    (void)fed(one, "\x1b[1;1Hp\x1b[L\x1b[M");

    SAME(row(one, 0), std::string{"p"});
    OUGHT(one.cursor() == (Spot{1, 0}));

    (void)fed(one, "\x1b[1;2r\x1b[4;1H\n\n");

    OUGHT(one.cursor() == (Spot{0, 3}));

    (void)fed(one, "\x1b[3;4r\x1b[1;1H\x1bM");

    OUGHT(one.cursor() == (Spot{0, 0}));

    Emulator three{{10, 3}};

    (void)fed(three, "a\r\nb\r\nc\x1b[9M");

    SAME(row(three, 2), std::string{});
    SAME(row(three, 1), std::string{"b"});
}

TRIAL("Emulator: colors and looks in their other spellings, and hidden text") {
    Emulator one{{10, 2}};

    (void)fed(one, "\x1b[38:2:10:20:30ma\x1b[38;5;4;3mb\x1b[0;4:0mc\x1b[8mde\x1b[28mf");

    OUGHT(cell(one, 0, 0).look.ink == (Color{10, 20, 30}));
    OUGHT(cell(one, 0, 1).look.ink == tal::palette::indexed(4));
    OUGHT(cell(one, 0, 1).look.italic && !cell(one, 0, 1).look.under);
    OUGHT(!cell(one, 0, 2).look.under);
    SAME(row(one, 0), std::string{"abc  f"});
}

TRIAL("Emulator: each screen saves its own cursor, and 1047 clears the alternate screen only on the way in") {
    Emulator one{{10, 4}};

    (void)fed(one, "\x1b[?47h\x1b[2;2H\x1b" "7\x1b[?47l\x1b[3;3H\x1b" "8");

    OUGHT(one.cursor() == (Spot{0, 0}));

    (void)fed(one, "\x1b[?1047hab\x1b[?1047l\x1b[?47h");

    SAME(row(one, 0), std::string{"ab"});

    (void)fed(one, "\x1b[?47l\x1b]8;;https://x.pl\x07\x1b" "cx");

    SAME(cell(one, 0, 0).link, std::uint32_t{0});
}

TRIAL("Emulator: the history and the prompt count rows with no history kept, and none on the alternate screen") {
    Emulator none{{10, 3}, 0};

    (void)fed(none, "\r\n\r\n\x1b]133;A\x07$ \r\n");

    OUGHT(none.prompt() == std::optional<int>{1});

    Emulator one{{10, 3}, 1};

    (void)fed(one, "\r\n\r\n\x1b]133;A\x07$ \r\n\r\n");

    OUGHT(one.prompt() == std::optional<int>{0});

    (void)fed(one, "\x1b[?1049h");

    SAME(one.history(), std::size_t{0});
    OUGHT(!one.prompt().has_value());

    (void)fed(one, "\x1b[3;1H\x1b]133;A\x07\r\n\r\n\r\n\x1b[?1049l");

    SAME(one.history(), std::size_t{1});
    OUGHT(one.prompt() == std::optional<int>{0});

    const Fed done = fed(one, "\x1b]133;D;99999999999\x07");

    OUGHT(done.stages.at(0).code == std::optional<int>{65535});

    Emulator flowing{{10, 3}, 0};

    (void)fed(flowing, "abcdefghijklmnopq\r\n\x1b]133;A\x07$ ");

    flowing.resize({5, 3});

    OUGHT(flowing.prompt() == std::optional<int>{2});
}

TRIAL("Emulator: the folder moves only when it changes") {
    Emulator one{{10, 2}};

    OUGHT(fed(one, "\x1b]7;file://h/a\x07").moved);
    OUGHT(!fed(one, "\x1b]7;file://h/a\x07").moved);
}

TRIAL("Emulator: a new size keeps the tab stops, the cursor of the alternate screen and an empty line it stands on") {
    Emulator one{{10, 6}};

    one.resize({20, 6});

    (void)fed(one, "\t\t");

    OUGHT(one.cursor() == (Spot{16, 0}));

    (void)fed(one, "\x1b[1;1Hx\x1b[6;1H");

    one.resize({20, 3});

    SAME(row(one, 2), std::string{});
    SAME(one.history(), std::size_t{3});
    OUGHT(one.cursor() == (Spot{0, 2}));

    one.resize({10, 3});

    SAME(one.history(), std::size_t{3});
    OUGHT(one.cursor() == (Spot{0, 2}));

    (void)fed(one, "\x1b[?1049h\x1b[3;9H");

    one.resize({5, 2});

    OUGHT(one.cursor() == (Spot{4, 1}));

    Emulator flowing{{10, 3}};

    (void)fed(flowing, "abcdefghij\x1b[3;1H");

    flowing.resize({5, 3});

    SAME(flowing.history(), std::size_t{1});
    SAME(row(flowing, 0), std::string{"fghij"});
    OUGHT(flowing.cursor() == (Spot{0, 2}));
}

// --- Bounds against a hostile program ------------------------------------------------------------

TRIAL("Emulator: an avalanche of parameters and parts, huge numbers and too many intermediates") {
    Emulator one{{20, 3}};
    std::string avalanche = "\x1b[";

    for (int at = 0; at < 100000; ++at) avalanche += "1;";

    (void)fed(one, avalanche + "mx\x1b[0m");

    OUGHT(cell(one, 0, 0).look.bold);

    std::string parts = "\x1b[4";

    for (int at = 0; at < 100000; ++at) parts += ":1";

    (void)fed(one, parts + "my\x1b[0m");

    OUGHT(cell(one, 0, 1).look.under);

    (void)fed(one, "\x1b[99999999999999999999;99999999999999999999H");

    OUGHT(one.cursor() == (Spot{19, 2}));

    (void)fed(one, "\x1b[4h\x1b[!!!p");

    OUGHT(one.modes().insert);

    (void)fed(one, "\x1b[!p\x1b#88z");

    OUGHT(!one.modes().insert);

    (void)fed(one, "\x1b[1;1H\x1b[2J\x1b(((0q");

    SAME(row(one, 0), std::string{"q"});
}

TRIAL("Emulator: a string past its bound is dropped whole, and a cluster keeps its bound") {
    Emulator one{{20, 2}};

    OUGHT(fed(one, "\x1b]2;" + std::string(4000, 't') + "\x07").titled);
    SAME(one.title().size(), std::size_t{4000});
    OUGHT(!fed(one, "\x1b]2;" + std::string(5000, 'u') + "\x07" "x").titled);
    SAME(one.title().size(), std::size_t{4000});
    SAME(row(one, 0), std::string{"x"});

    std::string accents = "e";

    for (int at = 0; at < 1000000; ++at) accents += "\xCC\x81";

    (void)fed(one, accents + "!");

    OUGHT(cell(one, 0, 1).cluster.size() <= 32);
    OUGHT(cell(one, 0, 1).cluster.starts_with("e\xCC\x81"));
    SAME(cell(one, 0, 2).cluster, std::string{"!"});
}

TRIAL("Emulator: the addresses of links keep their bound, and those no cell holds make room") {
    Emulator full{{1024, 1}, 0};

    for (int at = 0; at < 600; ++at) {
        std::string address = "https://x.pl/" + std::to_string(at) + "/";

        address.resize(2000, 'a');

        (void)fed(full, "\x1b]8;;" + address + "\x07" "x\x1b]8;;\x07");
    }

    OUGHT(full.address(cell(full, 0, 0).link).starts_with("https://x.pl/0/"));
    OUGHT(cell(full, 0, 400).link != 0);
    SAME(cell(full, 0, 599).link, std::uint32_t{0});
    SAME(cell(full, 0, 599).cluster, std::string{"x"});

    Emulator moving{{4, 1}, 0};

    for (int at = 0; at < 2000; ++at) {
        std::string address = "https://x.pl/" + std::to_string(at) + "/";

        address.resize(2000, 'a');

        (void)fed(moving, "\r\n\x1b]8;;" + address + "\x07" "x\x1b]8;;\x07");
    }

    OUGHT(moving.address(cell(moving, 0, 0).link).starts_with("https://x.pl/1999/"));
}

// --- Sizes ---------------------------------------------------------------------------------------

TRIAL("Emulator: a lower screen keeps the cursor in sight, a higher one brings the history back") {
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

    one.resize({0, 2000});

    OUGHT(one.size() == (Size{1, 1024}));
}

TRIAL("Emulator: a lower screen drops the empty lines under the cursor first") {
    Emulator one{{10, 6}};

    (void)fed(one, "1\r\n2$ ");

    one.resize({10, 3});

    SAME(row(one, 0), std::string{"1"});
    SAME(row(one, 1), std::string{"2$"});
    SAME(one.history(), std::size_t{0});
}

TRIAL("Emulator: a new width lays the text out anew, and the old width gives it back") {
    Emulator one{{10, 4}};

    (void)fed(one, "abcdefghijklmno\r\n$ ");

    one.resize({6, 4});

    OUGHT(all(one) == (std::vector<std::string>{"abcdef", "ghijkl", "mno", "$"}));
    OUGHT(one.row(0).wrapped && one.row(1).wrapped && !one.row(2).wrapped);
    OUGHT(one.cursor() == (Spot{2, 3}));

    one.resize({10, 4});

    OUGHT(all(one) == (std::vector<std::string>{"abcdefghij", "klmno", "$", ""}));
    OUGHT(one.cursor() == (Spot{2, 2}));

    one.resize({5, 2});

    OUGHT(all(one) == (std::vector<std::string>{"abcde", "fghij", "klmno", "$"}));
    SAME(one.history(), std::size_t{2});
    OUGHT(one.row(-1).wrapped);
    OUGHT(one.cursor() == (Spot{2, 1}));

    one.resize({10, 4});

    OUGHT(all(one) == (std::vector<std::string>{"abcdefghij", "klmno", "$", ""}));
    OUGHT(one.cursor() == (Spot{2, 2}));
    SAME(one.history(), std::size_t{0});
}

TRIAL("Emulator: a new width keeps wide characters whole and the cursor by its character") {
    Emulator one{{5, 3}};

    (void)fed(one, "ab日本");

    OUGHT(all(one) == (std::vector<std::string>{"ab日", "本", ""}));
    OUGHT(one.cursor() == (Spot{2, 1}));

    one.resize({6, 3});

    OUGHT(all(one) == (std::vector<std::string>{"ab日本", "", ""}));
    OUGHT(one.cursor() == (Spot{5, 0}));

    (void)fed(one, "c");

    OUGHT(all(one) == (std::vector<std::string>{"ab日本", "c", ""}));

    one.resize({5, 3});

    OUGHT(all(one) == (std::vector<std::string>{"ab日", "本c", ""}));
    OUGHT(one.cursor() == (Spot{3, 1}));

    one.resize({1, 3});

    SAME(row(one, 2), std::string{"c"});
    OUGHT(one.cursor() == (Spot{0, 2}));
    SAME(one.history(), std::size_t{2});
    SAME(row(one, -1), std::string{"b"});

    Emulator waits{{5, 3}};

    (void)fed(waits, "abcde");

    waits.resize({4, 3});

    OUGHT(all(waits) == (std::vector<std::string>{"abcd", "e", ""}));
    OUGHT(waits.cursor() == (Spot{1, 1}));

    waits.resize({5, 3});

    (void)fed(waits, "x");

    OUGHT(all(waits) == (std::vector<std::string>{"abcde", "x", ""}));
}

TRIAL("Emulator: a new width under the alternate screen lays out the primary one and its cursor") {
    Emulator one{{10, 3}};

    (void)fed(one, "\x1b]133;A\x07" "abcdefgh$ \x1b[?1049h\x1b[Hfull");

    one.resize({4, 4});

    SAME(row(one, 0), std::string{"full"});

    (void)fed(one, "\x1b[?1049l");

    OUGHT(all(one) == (std::vector<std::string>{"abcd", "efgh", "$", ""}));
    OUGHT(one.cursor() == (Spot{2, 2}));
    OUGHT(one.prompt() == std::optional<int>{0});
}

TRIAL("Emulator: the row of the prompt moves with its text to a new width") {
    Emulator one{{10, 4}};

    (void)fed(one, "abcdefghijklmn\r\n\x1b]133;A\x07$ \x1b]133;B\x07");

    OUGHT(one.prompt() == std::optional<int>{2});

    one.resize({5, 5});

    OUGHT(one.prompt() == std::optional<int>{3});
    OUGHT(one.cursor() == (Spot{2, 3}));
}
