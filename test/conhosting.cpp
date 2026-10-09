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

// **Tłumaczenie dla konsoli bez VT.** Część przenośna sprawdza się tu, na każdym systemie: mały
// terminal czyta to, co pisze `Term`, i odpowiada sondzie, a rekordy klawiatury i myszy wychodzą
// tymi samymi bajtami, które conhost z trybem VT dałby dekoderowi. Klej Win32 sprawdza się już
// tylko na prawdziwej konsoli.

using namespace talcite;

namespace {

// Kolory konsoli: biały na czarnym, jak w domyślnym cmd.exe.
conhost::Screen screen(int wide = 20, int high = 5) { return conhost::Screen{{wide, high}, 7, 0}; }

std::u32string row(const conhost::Screen& from, int y) {
    std::u32string made;

    for (int x = 0; x < from.size().wide; ++x) made += from.at(x, y).glyph;

    while (!made.empty() && made.back() == U' ') made.pop_back();

    return made;
}

// Konsola, za którą stoi tłumaczenie — tak jak stanie w `console.cpp`, tylko bez Win32. Na Windows
// pusty TERM wystarcza za „to jest konsola"; tu, pod POSIX, podłoga potrzebuje jakiegoś, więc
// dajemy najskromniejszy — bez 256 i bez COLORTERM, żeby barwy szły z `vt()`.
class Translated final : public Console {
public:
    conhost::Screen shown{{40, 6}, 7, 0};

    [[nodiscard]] std::string_view name() const override { return "translated console"; }

    [[nodiscard]] std::optional<std::string> variable(std::string_view name) const override {
        if (name == "TERM") return std::string{"xterm"};

        return std::nullopt;
    }

    [[nodiscard]] bool lively() const override { return true; }

    [[nodiscard]] bool vt() const override { return false; }

    [[nodiscard]] std::optional<ui::Size> size() const override { return shown.size(); }

    [[nodiscard]] std::chrono::milliseconds double_click() const override { return std::chrono::milliseconds{500}; }

    void raw(bool) override {}

    [[nodiscard]] bool resumed() override { return false; }

    void write(std::string_view bytes) override { shown.fed(bytes); }

    [[nodiscard]] std::string hear(std::optional<std::chrono::milliseconds>, const Bell*) override {
        return shown.answers();
    }
};

class Painted : public ui::Root {
protected:
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

// --- Siatka --------------------------------------------------------------------------------------

TRIAL("Konsola bez VT: tekst staje od kursora, a CUP liczy od jedynki") {
    conhost::Screen one = screen();

    one.fed("ab\x1b[3;5Hcd");

    same(row(one, 0), std::u32string{U"ab"});
    same(row(one, 2), std::u32string{U"    cd"});
    ought(one.cursor() == ui::Spot{6, 2});
}

TRIAL("Konsola bez VT: UTF-8 urwany w połowie wychodzi jako znak zapytania") {
    conhost::Screen one = screen();

    one.fed("\xc4");
    one.fed("\x85!\xc4x");

    same(row(one, 0), std::u32string{U"ą!?x"});
}

TRIAL("Konsola bez VT: znak chiński zajmuje dwie kratki, druga pusta i oznaczona") {
    conhost::Screen one = screen();

    one.fed("a你b");

    same(one.at(1, 0).glyph, U'你');
    ought(!one.at(1, 0).trailing);
    ought(one.at(2, 0).trailing);
    same(one.at(3, 0).glyph, U'b');
    ought(one.cursor() == ui::Spot{4, 0});
}

TRIAL("Konsola bez VT: przykrycie połowy znaku szerokiego czyści drugą połowę") {
    conhost::Screen one = screen();

    one.fed("你好\x1b[1;2Hx\x1b[1;3Hy");

    same(row(one, 0), std::u32string{U" xy"});
    ought(!one.at(2, 0).trailing);
    ought(!one.at(3, 0).trailing);
}

TRIAL("Konsola bez VT: znak spoza BMP staje jako znak zapytania na dwóch kratkach") {
    conhost::Screen one = screen();

    one.fed("\xf0\x9f\xac\x80|");

    same(row(one, 0), std::u32string{U"? |"});
    ought(one.at(1, 0).trailing);
}

TRIAL("Konsola bez VT: bez zawijania ostatnia kolumna przyjmuje kolejne znaki") {
    conhost::Screen one = screen(4, 2);

    one.fed("\x1b[?7labcdef");

    same(row(one, 0), std::u32string{U"abcf"});
    same(row(one, 1), std::u32string{});
}

TRIAL("Konsola bez VT: z zawijaniem tekst schodzi wiersz niżej, a na dole siatka jedzie w górę") {
    conhost::Screen one = screen(3, 2);

    one.fed("abcdefg");

    same(row(one, 0), std::u32string{U"def"});
    same(row(one, 1), std::u32string{U"g"});
}

TRIAL("Konsola bez VT: EL i ED czyszczą od kursora, do kursora i całość") {
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

TRIAL("Konsola bez VT: zmienione wiersze wychodzą raz, rosnąco") {
    conhost::Screen one = screen(6, 4);

    (void)one.dirty();

    one.fed("\x1b[3;1Hx\x1b[1;1Hy");

    ought(one.dirty() == std::vector<int>{0, 2});
    ought(one.dirty().empty());
}

// --- Kolory --------------------------------------------------------------------------------------

TRIAL("Konsola bez VT: SGR ANSI przechodzi na bity konsoli, a wytłuszczenie rozjaśnia") {
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

TRIAL("Konsola bez VT: odwrócenie zamienia barwę z tłem, a podkreślenie zostaje na komórce") {
    conhost::Screen one = screen();

    one.fed("\x1b[0;4;7;31ma");

    same(int{one.at(0, 0).ink}, 0);
    same(int{one.at(0, 0).paper}, 4);
    ought(one.at(0, 0).under);
}

TRIAL("Konsola bez VT: 256 kolorów i RGB idą do najbliższego koloru konsoli") {
    conhost::Screen one = screen();

    one.fed("\x1b[0;38;5;196ma\x1b[0;38;2;0;0;140mb\x1b[0;48;5;9mc\x1b[0;38;5;244md");

    same(int{one.at(0, 0).ink}, 12);
    same(int{one.at(1, 0).ink}, 1);
    same(int{one.at(2, 0).paper}, 12);
    same(int{one.at(3, 0).ink}, 8);
}

TRIAL("Konsola bez VT: najbliższy kolor bierze pierwszy z równie bliskich") {
    const conhost::Palette among = conhost::classic();

    same(int{conhost::nearest(among, 0, 0, 0)}, 0);
    same(int{conhost::nearest(among, 255, 255, 255)}, 15);
    same(int{conhost::nearest(among, 200, 200, 200)}, 7);
    same(int{conhost::nearest(among, 64, 0, 0)}, 0);
}

// --- Tryby i ekran zapasowy ----------------------------------------------------------------------

TRIAL("Konsola bez VT: tryby kursora i myszy idą za DECSET i DECRST") {
    conhost::Screen one = screen();

    one.fed("\x1b[?25l\x1b[?1000h\x1b[?1002h\x1b[?1006h");

    ought(!one.shown());
    ought(one.tracking() == conhost::Tracking{true, true, false, true});

    one.fed("\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[?1003h\x1b[?25h");

    ought(one.shown());
    ought(one.tracking() == conhost::Tracking{false, false, true, false});
}

TRIAL("Konsola bez VT: ekran zapasowy czyści i każe przepisać wszystko, powrót oddaje stary bez przepisywania") {
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

TRIAL("Konsola bez VT: zmiana rozmiaru na ekranie zapasowym przycina też główny") {
    conhost::Screen one = screen(6, 3);

    one.fed("abcdef\x1b[?1049h");
    one.resize({3, 2});
    one.fed("\x1b[?1049l");

    same(one.size(), (ui::Size{3, 2}));
    same(row(one, 0), std::u32string{U"abc"});
    same(row(one, 1), std::u32string{});
}

TRIAL("Konsola bez VT: za długa sekwencja CSI znika w całości, nie wychodzi w tekst") {
    conhost::Screen one = screen();

    one.fed("\x1b[" + std::string(100, '1') + "mx");

    same(row(one, 0), std::u32string{U"x"});
}

TRIAL("Konsola bez VT: zmiana rozmiaru zostawia, co się mieści, i każe przepisać wszystko") {
    conhost::Screen one = screen(6, 3);

    one.fed("abcdef\x1b[3;6H");
    (void)one.dirty();

    one.resize({4, 2});

    same(row(one, 0), std::u32string{U"abcd"});
    ought(one.cursor() == ui::Spot{3, 1});
    ought(one.dirty() == std::vector<int>{0, 1});
}

// --- Odpowiedzi ----------------------------------------------------------------------------------

TRIAL("Konsola bez VT: DECRQM zna tylko to, co tłumaczenie umie") {
    conhost::Screen one = screen();

    one.fed("\x1b[?1006$p\x1b[?1049$p\x1b[?2004$p\x1b[?2026$p\x1b[?1004$p\x1b[?25$p");

    same(one.answers(), std::string{"\x1b[?1006;2$y\x1b[?1049;2$y\x1b[?2004;0$y\x1b[?2026;0$y\x1b[?1004;0$y\x1b[?25;1$y"});
    same(one.answers(), std::string{});
}

TRIAL("Konsola bez VT: CPR, DA1, 16t i OSC 11 odpowiadają, DA2, APC i DCS milkną") {
    conhost::Screen one{{20, 5}, 7, 1};

    one.fed("\x1b[>c\x1b_Gi=31,a=q;AAAA\x1b\\\x1bP+q5463\x1b\\\x1b[16t\x1b]11;?\x1b\\\x1b[2;4H\x1b[6n\x1b[c");

    same(one.answers(), std::string{"\x1b]11;rgb:0000/0000/8080\x1b\\\x1b[2;4R\x1b[?62;22c"});
    same(row(one, 0), std::u32string{});

    one.pixels({8, 16});
    one.fed("\x1b[18t\x1b[16t");

    same(one.answers(), std::string{"\x1b[6;16;8t"});
}

TRIAL("Konsola bez VT: tytuł przychodzi z OSC 0 i OSC 2, zakończony ST albo BEL") {
    conhost::Screen one = screen();

    one.fed("\x1b]0;twa \xe2\x80\x94 filmy\x1b\\");

    ought(one.title() == std::optional<std::string>{"twa \xe2\x80\x94 filmy"});
    ought(!one.title().has_value());

    one.fed("\x1b]2;drugi\x07x");

    ought(one.title() == std::optional<std::string>{"drugi"});
    same(row(one, 0), std::u32string{U"x"});
}

TRIAL("Konsola bez VT: sonda możliwości widzi szesnaście kolorów, mysz SGR, ekran zapasowy i bloki zwykłe") {
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

TRIAL("Konsola bez VT: klatka Term staje w siatce w kolorach konsoli") {
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

// --- Klawiatura ----------------------------------------------------------------------------------

namespace {

std::string key(std::uint16_t code, char16_t unit = 0, std::uint32_t state = 0, int repeat = 1) {
    char16_t pending = 0;

    return conhost::typed({code, unit, state, repeat}, pending);
}

}

TRIAL("Konsola bez VT: klawisze nazwane idą jak w conhoście z VT") {
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

TRIAL("Konsola bez VT: znak z Altem dostaje Esc, z AltGr zostaje literą układu") {
    same(key('A', u'a', 0x02), std::string{"\x1b" "a"});
    same(key('A', u'ą', 0x01 | 0x08), std::string{"ą"});
    same(key('A', u'ą', 0x02 | 0x08), std::string{"ą"});
    same(key('C', 0x03, 0x08), std::string{"\x03"});
    same(key('C', 0, 0x02 | 0x08), std::string{"\x1b\x03"});
    same(key(0x20, 0, 0x08), std::string(1, '\0'));
    same(key(0x10), std::string{});
}

TRIAL("Konsola bez VT: para zastępcza składa się z dwóch rekordów, a powtórzenie mnoży znak") {
    char16_t pending = 0;

    same(conhost::typed({0, 0xD83D, 0, 1}, pending), std::string{});
    same(conhost::typed({0, 0xDE00, 0, 1}, pending), std::string{"\xf0\x9f\x98\x80"});
    same(conhost::typed({0, 0xDE00, 0, 1}, pending), std::string{});

    same(key('X', u'x', 0, 3), std::string{"xxx"});
}

// --- Mysz ----------------------------------------------------------------------------------------

TRIAL("Konsola bez VT: wciśnięcie i puszczenie wychodzą z pamięci przycisków") {
    conhost::Pointing one;
    const conhost::Tracking wanted{true, true, false, true};

    same(one.pointed({{4, 2}, 0x1, 0, 0}, wanted), std::string{"\x1b[<0;5;3M"});
    same(one.pointed({{5, 2}, 0x1, 0x01, 0}, wanted), std::string{"\x1b[<32;6;3M"});
    same(one.pointed({{5, 2}, 0x3, 0, 0x10}, wanted), std::string{"\x1b[<6;6;3M"});
    same(one.pointed({{5, 2}, 0x0, 0, 0}, wanted), std::string{"\x1b[<0;6;3m\x1b[<2;6;3m"});
    same(one.pointed({{6, 2}, 0x0, 0x01, 0}, wanted), std::string{});
}

TRIAL("Konsola bez VT: kółko, ruch bez przycisku i przycisk wciśnięty przed śledzeniem") {
    conhost::Pointing one;

    // Bez 1006 nic: dekoder starego zapisu X10 nie zna.
    same(one.pointed({{0, 0}, 0x00780000u, 0x04, 0}, {true, false, false, false}), std::string{});

    same(one.pointed({{0, 0}, 0x00780000u, 0x04, 0}, {true, false, false, true}), std::string{"\x1b[<64;1;1M"});
    same(one.pointed({{0, 0}, 0xFF880000u, 0x04, 0x08}, {true, false, false, true}), std::string{"\x1b[<81;1;1M"});
    same(one.pointed({{2, 3}, 0, 0x01, 0}, {true, true, true, true}), std::string{"\x1b[<35;3;4M"});
    same(one.pointed({{2, 3}, 0x1, 0, 0}, {}), std::string{});
    same(one.pointed({{2, 3}, 0x1, 0x01, 0}, {true, false, false, true}), std::string{});
    same(one.pointed({{2, 3}, 0x1, 0x01, 0}, {true, true, false, true}), std::string{"\x1b[<32;3;4M"});
    same(one.pointed({{2, 3}, 0x0, 0, 0}, {true, false, false, true}), std::string{"\x1b[<0;3;4m"});
}
