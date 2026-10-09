#include "proving.hpp"

#include <string>
#include <vector>

#include "talcite/decoder.hpp"

using namespace talcite;

namespace {

struct Told {
    enum class What : int { key, mouse, wheel, paste };

    What what = What::key;
    ui::Key key;
    ui::Mods mods;
    ui::Spot where;
    ui::Press press = ui::Press::move;
    ui::Mouse button = ui::Mouse::none;
    int steps = 0;
    std::string said;
};

// Korzeń zapisujący. Dekoder karmi prawdziwe drzewo, więc przypadki sprawdzają całą drogę
// od bajtu do handle, a nie sam rozbiór.
class Sheet : public ui::Root {
public:
    [[nodiscard]] const std::vector<Told>& among() const { return among_; }

    [[nodiscard]] std::size_t much() const { return among_.size(); }

protected:
    bool handle(ui::KeyEvent& what) override {
        Told one;
        one.what = Told::What::key;
        one.key = what.key();
        one.mods = what.mods();

        among_.push_back(one);

        return true;
    }

    bool handle(ui::MouseEvent& what) override {
        Told one;
        one.what = Told::What::mouse;
        one.where = what.where();
        one.press = what.press();
        one.button = what.button();
        one.mods = what.mods();

        among_.push_back(one);

        return true;
    }

    bool handle(ui::WheelEvent& what) override {
        Told one;
        one.what = Told::What::wheel;
        one.where = what.where();
        one.steps = what.steps();
        one.mods = what.mods();

        among_.push_back(one);

        return true;
    }

    bool handle(ui::PasteEvent& what) override {
        Told one;
        one.what = Told::What::paste;
        one.said = what.text();

        among_.push_back(one);

        return true;
    }

private:
    std::vector<Told> among_;
};

}

TRIAL("Decoder: zwykła litera") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("q", sheet);

    same(sheet.much(), std::size_t{1});
    ought(sheet.among()[0].key.letter() == U'q');
    ought(!sheet.among()[0].mods.ctrl);
    ought(!decoder.pending());
}

TRIAL("Decoder: kilka liter po kolei") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("abc", sheet);

    same(sheet.much(), std::size_t{3});
    ought(sheet.among()[0].key.letter() == U'a');
    ought(sheet.among()[2].key.letter() == U'c');
}

TRIAL("Decoder: znak sterujący staje się literą z ctrl") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x01", sheet);

    ought(sheet.among()[0].key.letter() == U'a');
    ought(sheet.among()[0].mods.ctrl);
}

TRIAL("Decoder: tab, enter i backspace mają własne kody, nie ctrl") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x09\x0d\x7f\x08", sheet);

    same(sheet.much(), std::size_t{4});
    ought(sheet.among()[0].key.code() == ui::Code::tab);
    ought(sheet.among()[1].key.code() == ui::Code::enter);
    ought(sheet.among()[2].key.code() == ui::Code::backspace);
    ought(sheet.among()[3].key.code() == ui::Code::backspace);
    ought(!sheet.among()[0].mods.ctrl);
    ought(!sheet.among()[1].mods.ctrl);
}

TRIAL("Decoder: UTF-8 składa się w jeden znak") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("ł", sheet);

    same(sheet.much(), std::size_t{1});
    ought(sheet.among()[0].key.letter() == U'ł');
}

TRIAL("Decoder: znak pocięty między karmieniami składa się w całość") {
    Sheet sheet;
    Decoder decoder;

    const std::string said = "ł";

    decoder.fed(said.substr(0, 1), sheet);

    same(sheet.much(), std::size_t{0});

    decoder.fed(said.substr(1), sheet);

    same(sheet.much(), std::size_t{1});
    ought(sheet.among()[0].key.letter() == U'ł');
}

TRIAL("Decoder: emoji poza podstawową płaszczyzną") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("🙂", sheet);

    same(sheet.much(), std::size_t{1});
    ought(sheet.among()[0].key.letter() == U'🙂');
}

TRIAL("Decoder: strzałki") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[A\x1b[B\x1b[C\x1b[D", sheet);

    same(sheet.much(), std::size_t{4});
    ought(sheet.among()[0].key.code() == ui::Code::up);
    ought(sheet.among()[1].key.code() == ui::Code::down);
    ought(sheet.among()[2].key.code() == ui::Code::right);
    ought(sheet.among()[3].key.code() == ui::Code::left);
}

TRIAL("Decoder: strzałka z modyfikatorem") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[1;5C", sheet);

    ought(sheet.among()[0].key.code() == ui::Code::right);
    ought(sheet.among()[0].mods.ctrl);
    ought(!sheet.among()[0].mods.alt);
    ought(!sheet.among()[0].mods.shift);
}

TRIAL("Decoder: maska modyfikatorów rozkłada się na trzy") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[1;8A", sheet);

    ought(sheet.among()[0].mods.shift);
    ought(sheet.among()[0].mods.alt);
    ought(sheet.among()[0].mods.ctrl);
}

TRIAL("Decoder: klawisze z tyldą") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[2~\x1b[3~\x1b[5~\x1b[6~", sheet);

    same(sheet.much(), std::size_t{4});
    ought(sheet.among()[0].key.code() == ui::Code::insert);
    ought(sheet.among()[1].key.code() == ui::Code::remove);
    ought(sheet.among()[2].key.code() == ui::Code::pageup);
    ought(sheet.among()[3].key.code() == ui::Code::pagedown);
}

TRIAL("Decoder: F1 do F4 w obu zapisach") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1bOP\x1bOQ\x1b[11~\x1b[14~", sheet);

    same(sheet.much(), std::size_t{4});
    ought(sheet.among()[0].key.code() == ui::Code::f1);
    ought(sheet.among()[1].key.code() == ui::Code::f2);
    ought(sheet.among()[2].key.code() == ui::Code::f1);
    ought(sheet.among()[3].key.code() == ui::Code::f4);
}

TRIAL("Decoder: F5 do F12") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[15~\x1b[21~\x1b[23~\x1b[24~", sheet);

    ought(sheet.among()[0].key.code() == ui::Code::f5);
    ought(sheet.among()[1].key.code() == ui::Code::f10);
    ought(sheet.among()[2].key.code() == ui::Code::f11);
    ought(sheet.among()[3].key.code() == ui::Code::f12);
}

TRIAL("Decoder: home i end w obu zapisach") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[H\x1b[F\x1b[1~\x1b[4~", sheet);

    ought(sheet.among()[0].key.code() == ui::Code::home);
    ought(sheet.among()[1].key.code() == ui::Code::end);
    ought(sheet.among()[2].key.code() == ui::Code::home);
    ought(sheet.among()[3].key.code() == ui::Code::end);
}

TRIAL("Decoder: sekwencja pocięta między karmieniami") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b", sheet);
    same(sheet.much(), std::size_t{0});
    ought(decoder.pending());

    decoder.fed("[", sheet);
    same(sheet.much(), std::size_t{0});

    decoder.fed("A", sheet);
    same(sheet.much(), std::size_t{1});
    ought(sheet.among()[0].key.code() == ui::Code::up);
    ought(!decoder.pending());
}

TRIAL("Decoder: samotny Esc czeka, aż ktoś rozstrzygnie") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b", sheet);

    same(sheet.much(), std::size_t{0});
    ought(decoder.pending());

    decoder.flush(sheet);

    same(sheet.much(), std::size_t{1});
    ought(sheet.among()[0].key.code() == ui::Code::escape);
    ought(!decoder.pending());
}

TRIAL("Decoder: flush bez wiszącego Esc niczego nie robi") {
    Sheet sheet;
    Decoder decoder;

    decoder.flush(sheet);
    decoder.fed("q", sheet);
    decoder.flush(sheet);

    same(sheet.much(), std::size_t{1});
}

TRIAL("Decoder: flush na niedomkniętej sekwencji oddaje Esc i resztę") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[", sheet);

    ought(decoder.pending());

    decoder.flush(sheet);

    same(sheet.much(), std::size_t{2});
    ought(sheet.among()[0].key.code() == ui::Code::escape);
    ought(sheet.among()[1].key.letter() == U'[');
}

TRIAL("Decoder: Esc z literą to Alt") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b" "f", sheet);

    same(sheet.much(), std::size_t{1});
    ought(sheet.among()[0].key.letter() == U'f');
    ought(sheet.among()[0].mods.alt);
}

TRIAL("Decoder: Esc po Escu to dwa naciśnięcia") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b\x1b", sheet);

    same(sheet.much(), std::size_t{1});
    ought(sheet.among()[0].key.code() == ui::Code::escape);
    ought(decoder.pending());

    decoder.flush(sheet);

    same(sheet.much(), std::size_t{2});
    ought(sheet.among()[1].key.code() == ui::Code::escape);
}

TRIAL("Decoder: mysz wciśnięta i puszczona") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[<0;13;5M\x1b[<0;13;5m", sheet);

    same(sheet.much(), std::size_t{2});

    same(static_cast<int>(sheet.among()[0].what), static_cast<int>(Told::What::mouse));
    same(sheet.among()[0].where.x, 12);
    same(sheet.among()[0].where.y, 4);
    same(static_cast<int>(sheet.among()[0].press), static_cast<int>(ui::Press::down));
    same(static_cast<int>(sheet.among()[0].button), static_cast<int>(ui::Mouse::left));

    same(static_cast<int>(sheet.among()[1].press), static_cast<int>(ui::Press::up));
}

TRIAL("Decoder: przyciski myszy") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[<0;1;1M\x1b[<1;1;1M\x1b[<2;1;1M", sheet);

    same(static_cast<int>(sheet.among()[0].button), static_cast<int>(ui::Mouse::left));
    same(static_cast<int>(sheet.among()[1].button), static_cast<int>(ui::Mouse::middle));
    same(static_cast<int>(sheet.among()[2].button), static_cast<int>(ui::Mouse::right));
}

// Maska 35 to ruch **bez wciśniętego przycisku** — dokładnie to, co przysyła terminal po `?1003`.
// Brak przycisku jest tu równie ważny jak sam znacznik ruchu: po nim widżet poznaje najechanie
// i odróżnia je od ciągnięcia, które ma ten sam znacznik, ale przycisk już niesie.
TRIAL("Decoder: ruch myszy niesie znacznik ruchu") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[<35;7;9M", sheet);

    same(static_cast<int>(sheet.among()[0].press), static_cast<int>(ui::Press::move));
    same(static_cast<int>(sheet.among()[0].button), static_cast<int>(ui::Mouse::none));
    same(sheet.among()[0].where.x, 6);
    same(sheet.among()[0].where.y, 8);
}

TRIAL("Decoder: ciągnięcie niesie ruch razem z przyciskiem") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[<32;7;9M", sheet);

    same(static_cast<int>(sheet.among()[0].press), static_cast<int>(ui::Press::move));
    same(static_cast<int>(sheet.among()[0].button), static_cast<int>(ui::Mouse::left));
}

TRIAL("Decoder: kółko w obie strony") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[<64;3;3M\x1b[<65;3;3M", sheet);

    same(static_cast<int>(sheet.among()[0].what), static_cast<int>(Told::What::wheel));
    same(sheet.among()[0].steps, 1);
    same(sheet.among()[1].steps, -1);
}

TRIAL("Decoder: modyfikatory myszy") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[<16;1;1M", sheet);

    ought(sheet.among()[0].mods.ctrl);
    ought(!sheet.among()[0].mods.shift);
}

TRIAL("Decoder: wklejenie staje się jednym zdarzeniem") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[200~ala ma kota\x1b[201~", sheet);

    same(sheet.much(), std::size_t{1});
    same(static_cast<int>(sheet.among()[0].what), static_cast<int>(Told::What::paste));
    same(sheet.among()[0].said, std::string{"ala ma kota"});
}

TRIAL("Decoder: wklejenie pocięte między karmieniami") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[200~ala ", sheet);
    same(sheet.much(), std::size_t{0});

    decoder.fed("ma kota", sheet);
    same(sheet.much(), std::size_t{0});

    decoder.fed("\x1b[20", sheet);
    same(sheet.much(), std::size_t{0});

    decoder.fed("1~", sheet);
    same(sheet.much(), std::size_t{1});
    same(sheet.among()[0].said, std::string{"ala ma kota"});
}

TRIAL("Decoder: wklejenie z nową linią nie staje się Enterem") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[200~dwa\nwiersze\x1b[201~", sheet);

    same(sheet.much(), std::size_t{1});
    same(sheet.among()[0].said, std::string{"dwa\nwiersze"});
}

TRIAL("Decoder: klawisze po wklejeniu wracają do normy") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[200~x\x1b[201~q", sheet);

    same(sheet.much(), std::size_t{2});
    same(static_cast<int>(sheet.among()[0].what), static_cast<int>(Told::What::paste));
    ought(sheet.among()[1].key.letter() == U'q');
}

TRIAL("Decoder: nieznana sekwencja nie psuje tego, co po niej") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[99;99Z" "q", sheet);

    same(sheet.much(), std::size_t{1});
    ought(sheet.among()[0].key.letter() == U'q');
}

TRIAL("Decoder: zepsute wejście nie rośnie w buforze bez końca") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[" + std::string(200, '1'), sheet);

    decoder.fed("q", sheet);

    ought(sheet.much() >= 1);
}

TRIAL("Decoder: dwa korzenie dostają swoje, jeden dekoder wystarcza") {
    Sheet first;
    Sheet second;

    Decoder decoder;

    decoder.fed("a", first);
    decoder.fed("b", second);

    same(first.much(), std::size_t{1});
    same(second.much(), std::size_t{1});
    ought(first.among()[0].key == U'a');
    ought(second.among()[0].key == U'b');
}

TRIAL("Decoder: znak pocięty między korzeniami trafia do tego drugiego") {
    Sheet first;
    Sheet second;

    Decoder decoder;

    const std::string said = "ł";

    decoder.fed(said.substr(0, 1), first);
    decoder.fed(said.substr(1), second);

    same(first.much(), std::size_t{0});
    same(second.much(), std::size_t{1});
    ought(second.among()[0].key == U'ł');
}

// **Po bajcie to samo, co całością.** `Term` karmi dekoder bajt po bajcie i przy każdym bierze
// bieżący korzeń — bo klawisz w środku odczytanego kawałka może przełączyć ekran (`Term::show`),
// a klawisze wpisane z wyprzedzeniem mają trafić już do nowego. To działa tylko wtedy, gdy dla
// dekodera granica karmienia nic nie znaczy: litery, UTF-8, klawisze z sekwencji, Alt, mysz,
// kółko i wklejenie w nawiasach wychodzą te same, w tej samej kolejności.
TRIAL("Decoder: karmienie po bajcie daje te same zdarzenia, co całością") {
    const std::string said = std::string{"aż"} + "\x1b[A" + "\x1b[1;5C" + "\x1bx" + "\x1b[15~" +
                             "\x1b[<0;10;5M" + "\x1b[<0;10;5m" + "\x1b[<64;3;4M" +
                             "\x1b[200~wklej\nto\x1b[201~" + "🙂" + "\t";

    Sheet whole;
    Sheet bytes;

    Decoder one;
    Decoder other;

    one.fed(said, whole);

    for (const char each : said) other.fed(std::string_view{&each, 1}, bytes);

    ought(!one.pending());
    ought(!other.pending());
    same(whole.much(), bytes.much());
    same(whole.much(), std::size_t{12});

    for (std::size_t at = 0; at < whole.much() && at < bytes.much(); ++at) {
        const Told& left = whole.among()[at];
        const Told& right = bytes.among()[at];

        same(static_cast<int>(left.what), static_cast<int>(right.what));
        ought(left.key == right.key);
        same(left.mods.ctrl, right.mods.ctrl);
        same(left.mods.alt, right.mods.alt);
        same(left.mods.shift, right.mods.shift);
        same(left.where.x, right.where.x);
        same(left.where.y, right.where.y);
        same(static_cast<int>(left.press), static_cast<int>(right.press));
        same(static_cast<int>(left.button), static_cast<int>(right.button));
        same(left.steps, right.steps);
        same(left.said, right.said);
    }
}

// Przełączenie korzenia w środku kawałka: uchwyt pierwszego klawisza przestawia cel, a reszta
// — karmiona po bajcie, jak robi to `Term` — idzie już do drugiego korzenia.
TRIAL("Decoder: po bajcie reszta kawałka idzie do korzenia przełączonego w jego środku") {
    Sheet first;
    Sheet second;

    ui::Root* onto = &first;

    Decoder decoder;

    const std::string said = std::string{"x"} + "yż" + "\x1b[A";

    for (std::size_t at = 0; at < said.size(); ++at) {
        decoder.fed(std::string_view{said}.substr(at, 1), *onto);

        if (first.much() == 1) onto = &second;
    }

    same(first.much(), std::size_t{1});
    ought(first.among()[0].key == U'x');
    same(second.much(), std::size_t{3});
    ought(second.among()[0].key == U'y');
    ought(second.among()[1].key == U'ż');
    ought(second.among()[2].key == ui::Key{ui::Code::up});
}


// Odpowiedzi terminala, które przyszły po czasie — po sondzie możliwości, przez SSH — nie są
// klawiszami. Dawniej kolor tła stawał się Alt+] i ciągiem liter, raport położenia kursora —
// Ctrl+F3, a odpowiedź XTGETTCAP — Alt+P z resztą wpisaną w pole.
TRIAL("Decoder: spóźnione odpowiedzi terminala nie są klawiszami") {
    const std::string late =
        "\x1b]11;rgb:0000/0000/0000\x1b\\"
        "\x1bP1+r5463=787465726d\x1b\\"
        "\x1b_Gi=31;OK\x1b\\"
        "\x1b[?62;1;2c"
        "\x1b[>0;280;0c"
        "\x1b[?1006;2$y"
        "\x1b[?12;5R"
        "\x1b[?1;5R"
        "\x1b[12;5R"
        "\x1b[?1u"
        "\x1b]11;rgb:ffff/ffff/ffff\x07"
        "q";

    Sheet whole;
    Decoder once;

    once.fed(late, whole);

    same(whole.much(), std::size_t{1});
    ought(whole.among()[0].key.letter() == U'q');
    ought(!once.pending());

    // Po bajcie, tak jak karmi `Term` — to samo.
    Sheet piece;
    Decoder slowly;

    for (const char one : late) slowly.fed(std::string(1, one), piece);

    same(piece.much(), std::size_t{1});
    ought(piece.among()[0].key.letter() == U'q');
}

// F3 z modyfikatorem ma kształt raportu położenia, ale z jedynką na przedzie — zostaje klawiszem.
TRIAL("Decoder: Ctrl+F3 zostaje klawiszem") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[1;5R", sheet);

    same(sheet.much(), std::size_t{1});
    ought(sheet.among()[0].key.code() == ui::Code::f3);
    ought(sheet.among()[0].mods.ctrl);
}

// Esc z literą, którą zaczyna się napis sterujący, czeka na zegar: domknięty — znika, a niedomknięty
// to Alt z tą literą i zwykłe znaki za nim.
TRIAL("Decoder: Alt z literą zaczynającą napis rozstrzyga zegar") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1bP", sheet);

    ought(decoder.pending());
    same(sheet.much(), std::size_t{0});

    decoder.flush(sheet);

    same(sheet.much(), std::size_t{1});
    ought(sheet.among()[0].key.letter() == U'P');
    ought(sheet.among()[0].mods.alt);

    Sheet more;
    Decoder other;

    other.fed("\x1b]ab", more);

    ought(other.pending());

    other.flush(more);

    same(more.much(), std::size_t{3});
    ought(more.among()[0].key.letter() == U']');
    ought(more.among()[0].mods.alt);
    ought(more.among()[1].key.letter() == U'a');
    ought(!more.among()[1].mods.alt);
    ought(more.among()[2].key.letter() == U'b');
}
