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
// od bajtu do react, a nie sam rozbiór.
class Sheet : public ui::Root {
public:
    [[nodiscard]] const std::vector<Told>& among() const { return among_; }

    [[nodiscard]] std::size_t much() const { return among_.size(); }

protected:
    bool react(ui::KeyEvent& what) override {
        Told one;
        one.what = Told::What::key;
        one.key = what.key();
        one.mods = what.mods();

        among_.push_back(one);

        return true;
    }

    bool react(ui::MouseEvent& what) override {
        Told one;
        one.what = Told::What::mouse;
        one.where = what.where();
        one.press = what.press();
        one.button = what.button();
        one.mods = what.mods();

        among_.push_back(one);

        return true;
    }

    bool react(ui::WheelEvent& what) override {
        Told one;
        one.what = Told::What::wheel;
        one.where = what.where();
        one.steps = what.steps();
        one.mods = what.mods();

        among_.push_back(one);

        return true;
    }

    bool react(ui::PasteEvent& what) override {
        Told one;
        one.what = Told::What::paste;
        one.said = what.said();

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
