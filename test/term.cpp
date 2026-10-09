#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "faking.hpp"
#include "talcite/term.hpp"

using namespace talcite;

// `Term` rozmawia z terminalem wyłącznie przez `Console`, więc z atrapą widać każdy bajt: klatkę,
// ubranie terminala i jego zdjęcie, tytuł — a także to, co przychodzi: klawisze, rozmiar i tryb.

namespace {

// Korzeń, który w lewym górnym rogu ma jedną literę w czystej czerwieni.
class Red : public ui::Root {
protected:
    View paint(ui::Size given, ui::Area seen) override {
        (void)seen;

        View made{given};

        Style how;
        how.foreground = Color{false, 255, 0, 0};

        made.put({0, 0}, U'x', how);

        return made;
    }
};

// To, co poszło na terminal po badaniu: badanie mówi swoje przez tę samą konsolę, a tu chodzi
// o to, co pisze sam `Term`.
std::string drawn(const std::shared_ptr<faking::Terminal>& line, Setup given = {}) {
    Red root;

    Term term{std::move(given), Seam<Console>{line}};

    term.show(root);

    line->written.clear();

    term.draw();

    return line->written;
}

bool holds(const std::string& where, const std::string& what) {
    return where.find(what) != std::string::npos;
}

}

// Korzeń, który pisze tekst z sekwencją ucieczki — tak, jak przyszłaby w nazwie pliku.
class Hostile : public ui::Root {
protected:
    View paint(ui::Size given, ui::Area seen) override {
        (void)seen;

        View made{given};

        made.put({0, 0}, std::string{"\x1b]52;c;cHduZWQ=\x07\x1bZ\xc2\x9b""31m"}, Style{});

        return made;
    }
};

// --- Barwy ---------------------------------------------------------------------------------------

TRIAL("Term: paleta 256 wychodzi jako 38;5") {
    const std::string said = drawn(faking::terminal());

    ought(holds(said, "\x1b[0;38;5;196mx"));
}

TRIAL("Term: szesnaście kolorów wychodzi jako jasna czerwień") {
    auto line = faking::terminal();

    line->environment["TERM"] = "vt220";

    ought(holds(drawn(line), "\x1b[0;91mx"));
}

TRIAL("Term: NO_COLOR zostawia sam znak bez barwy") {
    auto line = faking::terminal();

    line->environment["NO_COLOR"] = "1";

    const std::string said = drawn(line);

    ought(holds(said, "\x1b[0mx"));
    ought(!holds(said, "38;"));
}

// Życzenie wygrywa z pomiarem: `Setup` mówi, czym rysować, choćby konsola twierdziła inaczej.
TRIAL("Term: Setup przykrywa wykryte barwy") {
    Setup given;
    given.colors = Colors::rgb;

    ought(holds(drawn(faking::terminal(), given), "\x1b[0;38;2;255;0;0mx"));
}

TRIAL("Term: badanie idzie przez tę samą konsolę") {
    auto line = faking::terminal();

    Term term{Setup{}, Seam<Console>{line}};

    ought(line->written.rfind("\x1b[>c", 0) == 0);
}

// --- Ubranie terminala ---------------------------------------------------------------------------

namespace {

Setup dressy() {
    Setup made;
    made.screen = Screen::apart;
    made.mouse = Mouse::sgr;
    made.bracketed_paste = true;

    return made;
}

}

// Ekran zapasowy idzie pierwszy przy ubieraniu i ostatni przy zdejmowaniu: inaczej sekwencje
// myszy i wklejania trafiłyby na inny bufor niż ten, na którym zostaną.
TRIAL("Term: ubranie i zdjęcie idą w odwrotnej kolejności") {
    auto line = faking::terminal();

    std::string on;

    {
        Term term{dressy(), Seam<Console>{line}};

        on = line->written;

        line->written.clear();
    }

    const std::string off = line->written;

    ought(on.find("\x1b[?1049h") < on.find("\x1b[?1000h\x1b[?1002h\x1b[?1006h"));
    ought(on.find("\x1b[?1006h") < on.find("\x1b[?2004h"));

    ought(off.find("\x1b[?2004l") < off.find("\x1b[?1006l\x1b[?1002l\x1b[?1000l"));
    ought(off.find("\x1b[?1000l") < off.find("\x1b[?1049l"));
    ought(holds(off, "\x1b[?25h"));
}

TRIAL("Term: bez życzeń mysz i ekran idą za pomiarem") {
    auto line = faking::terminal();

    line->environment["TERM"] = "dumb";

    Term term{Setup{}, Seam<Console>{line}};

    ought(!holds(line->written, "\x1b[?1000h"));
    ought(!holds(line->written, "\x1b[?47h"));
    ought(!holds(line->written, "\x1b[?1049h"));
}

// Zejście z drogi zdejmuje wszystko, powrót zakłada z powrotem, a zagnieżdżone zejście nie wysyła
// drugiego kompletu.
TRIAL("Term: Pause zdejmuje raz i raz zakłada") {
    auto line = faking::terminal();

    Term term{dressy(), Seam<Console>{line}};

    line->written.clear();

    std::string away;

    {
        const Term::Pause one{term};
        const Term::Pause two{term};

        away = line->written;

        line->written.clear();
    }

    const std::string back = line->written;

    ought(holds(away, "\x1b[?1049l"));
    ought(away.find("\x1b[?1049l") == away.rfind("\x1b[?1049l"));
    ought(holds(back, "\x1b[?1049h"));
    ought(back.find("\x1b[?1049h") == back.rfind("\x1b[?1049h"));
}

TRIAL("Term: w trakcie zejścia nic nie rysuje") {
    auto line = faking::terminal();

    Red root;

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    const Term::Pause away{term};

    line->written.clear();

    term.draw();

    same(line->written, std::string{});
}

// --- Klatka --------------------------------------------------------------------------------------

TRIAL("Term: druga taka sama klatka nie wysyła ani bajtu") {
    auto line = faking::terminal();

    Red root;

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);
    term.draw();

    line->written.clear();

    term.draw();

    same(line->written, std::string{});
}

TRIAL("Term: klatka w synchronizacji, gdy wolno") {
    Setup given;
    given.synchronized = true;

    const std::string said = drawn(faking::terminal(), given);

    ought(said.rfind("\x1b[?2026h", 0) == 0);
    ought(said.size() >= 8 && said.compare(said.size() - 8, 8, "\x1b[?2026l") == 0);
}

// --- Tytuł ---------------------------------------------------------------------------------------

TRIAL("Term: tytuł idzie jako OSC 0") {
    auto line = faking::terminal();

    Term term{Setup{}, Seam<Console>{line}};

    line->written.clear();

    term.title("abc");

    same(line->written, std::string{"\x1b]0;abc\x1b\\"});
}

TRIAL("Term: tytuł zakazany w Setup nie wychodzi") {
    auto line = faking::terminal();

    Setup given;
    given.titles = false;

    Term term{given, Seam<Console>{line}};

    line->written.clear();

    term.title("abc");

    same(line->written, std::string{});
}

// Zmiana idzie przez pętlę, więc dopiero obrót ją wykonuje — i wysyła samą różnicę.
TRIAL("Term: reconfigure wysyła samą różnicę") {
    auto line = faking::terminal();

    Term term{dressy(), Seam<Console>{line}};

    Setup given = dressy();
    given.bracketed_paste = false;

    term.reconfigure(given);

    line->written.clear();

    (void)term.loop().turn();

    same(line->written, std::string{"\x1b[?2004l"});
}

// --- Tryb surowy, wejście i rozmiar --------------------------------------------------------------
//
// Także to idzie przez konsolę: `Term` nie ma kodu systemowego, więc atrapa odpowiada za
// wszystko, co terminal robi — i próba sprawdza obrót od klawisza do klatki bez terminala.

namespace {

// Korzeń, który zapisuje, co do niego doszło, a o obrót prosi wtedy, gdy mu to ustawić.
class Heard : public Red {
public:
    std::u32string keys;
    std::vector<ui::Size> sizes;
    std::optional<std::chrono::milliseconds> wanted;

    [[nodiscard]] std::optional<std::chrono::milliseconds> due() const override { return wanted; }

protected:
    using Red::handle;

    bool handle(ui::KeyEvent& what) override {
        keys += what.key().code() == ui::Code::escape ? U'⎋' : what.key().letter();

        return true;
    }

    bool handle(ui::SizeEvent& what) override {
        sizes.push_back({what.wide(), what.high()});

        return false;
    }
};

}

TRIAL("Term: tryb surowy od budowy do końca, badanie w nim zagnieżdżone") {
    auto line = faking::terminal();

    {
        Term term{Setup{}, Seam<Console>{line}};

        same(line->depth, 1);
        same(line->entered, 2);
    }

    same(line->depth, 0);
}

TRIAL("Term: Pause zdejmuje tryb surowy, powrót zakłada go i mierzy okno od nowa") {
    auto line = faking::terminal();

    line->window = ui::Size{100, 30};

    Term term{Setup{}, Seam<Console>{line}};

    {
        const Term::Pause one{term};
        const Term::Pause two{term};

        same(line->depth, 0);

        line->window = ui::Size{70, 20};
    }

    same(line->depth, 1);
    same(term.size().wide, 70);
    same(term.size().high, 20);
}

TRIAL("Term: w trakcie zejścia konsola nie słucha") {
    auto line = faking::terminal();

    Heard root;

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    const Term::Pause away{term};

    line->replies.push_back("a");

    const int before = line->rounds;

    (void)term.turn();

    same(line->rounds, before);
    ought(root.keys.empty());
}

TRIAL("Term: klawisz z konsoli trafia do korzenia") {
    auto line = faking::terminal();

    Heard root;

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    line->replies.push_back("ab");

    (void)term.turn();

    ought(root.keys == U"ab");
}

// Bajty, które przyszły w trakcie badania i nie były odpowiedzią, to klawisze naciśnięte przy
// starcie. Idą pierwsze, przed czymkolwiek świeżym.
TRIAL("Term: klawisze z badania idą przed świeżymi") {
    auto line = faking::terminal("q\x1b[?62;1c");

    Heard root;

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    line->replies.push_back("a");

    (void)term.turn();

    ought(root.keys == U"qa");
}

TRIAL("Term: samotny Esc czeka pięćdziesiąt milisekund i rozstrzyga się sam") {
    auto line = faking::terminal();

    Heard root;

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    line->replies.push_back("\x1b");

    (void)term.turn();

    ought(root.keys.empty());

    (void)term.turn();

    ought(line->patience.back() == std::chrono::milliseconds{50});
    ought(root.keys == U"⎋");
}

// Bez Esc w dekoderze i bez prośby drzewa nie ma czego rozstrzygać, więc konsola czeka bez końca —
// na klawisz albo na dzwonek pętli, który przynosi jej `Term`.
TRIAL("Term: bez powodu czeka się bez ogranicznika, ale z dzwonkiem") {
    auto line = faking::terminal();

    Heard root;

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    const int before = line->belled;

    (void)term.turn();

    ought(!line->patience.back().has_value());
    same(line->belled, before + 1);
}

TRIAL("Term: prośba drzewa o obrót idzie do konsoli jako cierpliwość") {
    auto line = faking::terminal();

    Heard root;

    root.wanted = std::chrono::milliseconds{30};

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    (void)term.turn();

    ought(line->patience.back() == std::chrono::milliseconds{30});
}

TRIAL("Term: rozmiar od startu z konsoli, a bez niego 80×25") {
    auto sized = faking::terminal();
    auto blind = faking::terminal();

    sized->window = ui::Size{100, 30};

    const Term one{Setup{}, Seam<Console>{sized}};
    const Term two{Setup{}, Seam<Console>{blind}};

    same(one.size().wide, 100);
    same(one.size().high, 30);
    same(two.size().wide, 80);
    same(two.size().high, 25);
}

TRIAL("Term: zmiana rozmiaru przychodzi jako SizeEvent i odmalowuje wszystko") {
    auto line = faking::terminal();

    line->window = ui::Size{40, 10};

    Heard root;

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    (void)term.turn();

    line->window = ui::Size{50, 12};
    line->written.clear();

    (void)term.turn();

    same(root.sizes.size(), std::size_t{1});
    same(root.sizes.front().wide, 50);
    same(root.sizes.front().high, 12);
    ought(holds(line->written, "\x1b[2J"));

    line->written.clear();

    (void)term.turn();

    same(root.sizes.size(), std::size_t{1});
}

// Wznowiony po zatrzymaniu: powłoka miała w tym czasie terminal, zdjęła z niego nasze ubranie
// i pisała po ekranie. Ubranie wraca i klatka maluje się cała, raz.
TRIAL("Term: wznowienie po zatrzymaniu ubiera terminal i maluje wszystko") {
    auto line = faking::terminal();

    Red root;

    Term term{dressy(), Seam<Console>{line}};

    term.show(root);

    (void)term.turn();

    line->written.clear();

    (void)term.turn();

    same(line->written, std::string{});

    line->comeback = true;

    (void)term.turn();

    ought(holds(line->written, "\x1b[?1049h"));
    ought(holds(line->written, "\x1b[2J"));
    ought(holds(line->written, "x"));

    line->written.clear();

    (void)term.turn();

    same(line->written, std::string{});
}

TRIAL("Term: czas dwukliku daje konsola") {
    auto line = faking::terminal();

    line->twice = std::chrono::milliseconds{777};

    Heard root;

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    ought(root.double_click() == std::chrono::milliseconds{777});
}

namespace {

// Korzeń, który na `x` przestawia terminal na inny — tak twa przełącza ekrany.
class Switching : public Heard {
public:
    Switching(Term& term, ui::Root& next) : term_(term), next_(next) {}

protected:
    using Heard::handle;

    bool handle(ui::KeyEvent& what) override {
        (void)Heard::handle(what);

        if (what.key().letter() == U'x') term_.show(next_);

        return true;
    }

private:
    Term& term_;
    ui::Root& next_;
};

}

// Klawisz przełączający w środku odczytanego kawałka: reszta, wpisana z wyprzedzeniem, należy już
// do nowego korzenia. Dawniej sprawdzała to tylko sonda na pseudoterminalu.
TRIAL("Term: klawisze po przełączeniu korzenia w środku kawałka idą do nowego") {
    auto line = faking::terminal();

    Term term{Setup{}, Seam<Console>{line}};

    Heard second;
    Switching first{term, second};

    term.show(first);

    line->replies.push_back("axyz");

    (void)term.turn();

    ought(first.keys == U"ax");
    ought(second.keys == U"yz");
}

// Sekwencja ucieczki w tekście nie dochodzi do terminala: ani zapis do schowka (OSC 52), ani
// prośba o przedstawienie się (DECID, na którą terminal odpowiada na wejściu), ani jednoznakowy
// CSI z C1. Z terminala wychodzą tylko nasze sekwencje.
TRIAL("Term: tekst z sekwencją ucieczki nie rozkazuje terminalowi") {
    auto line = faking::terminal();
    Hostile root;
    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);
    line->written.clear();
    term.draw();

    const std::string& said = line->written;

    ought(!holds(said, "\x1b]52"));
    ought(!holds(said, "\x1bZ"));
    ought(!holds(said, "\xc2\x9b"));
    ought(!holds(said, "\x07"));
    ought(holds(said, "\xe2\x90\x9b]52;c;"));
}
