#include "proving.hpp"

#include <string>
#include <vector>

#include "talcite/decoder.hpp"

using namespace talcite;

namespace {

// One event as the recording root saw it.
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

// A recording root. The decoder feeds a real tree, so the cases check the whole way from a byte
// to handle, not the parsing alone.
class Sheet : public ui::Root {
public:
    // Everything recorded so far, in order.
    [[nodiscard]] const std::vector<Told>& among() const { return among_; }

    // How many events were recorded.
    [[nodiscard]] std::size_t much() const { return among_.size(); }

protected:
    // Records a key with its modifiers.
    bool handle(ui::KeyEvent& what) override {
        Told one;
        one.what = Told::What::key;
        one.key = what.key();
        one.mods = what.mods();

        among_.push_back(one);

        return true;
    }

    // Records a mouse event with its cell, press, button and modifiers.
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

    // Records a wheel event with its cell, steps and modifiers.
    bool handle(ui::WheelEvent& what) override {
        Told one;
        one.what = Told::What::wheel;
        one.where = what.where();
        one.steps = what.steps();
        one.mods = what.mods();

        among_.push_back(one);

        return true;
    }

    // Records a paste with its text.
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

TRIAL("Decoder: a plain letter") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("q", sheet);

    same(sheet.much(), std::size_t{1});
    ought(sheet.among()[0].key.letter() == U'q');
    ought(!sheet.among()[0].mods.ctrl);
    ought(!decoder.pending());
}

TRIAL("Decoder: several letters in a row") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("abc", sheet);

    same(sheet.much(), std::size_t{3});
    ought(sheet.among()[0].key.letter() == U'a');
    ought(sheet.among()[2].key.letter() == U'c');
}

TRIAL("Decoder: a control character becomes a letter with ctrl") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x01", sheet);

    ought(sheet.among()[0].key.letter() == U'a');
    ought(sheet.among()[0].mods.ctrl);
}

TRIAL("Decoder: tab, enter and backspace have their own codes, not ctrl") {
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

TRIAL("Decoder: UTF-8 is put together into one character") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("ł", sheet);

    same(sheet.much(), std::size_t{1});
    ought(sheet.among()[0].key.letter() == U'ł');
}

TRIAL("Decoder: a character split between feeds is put back together") {
    Sheet sheet;
    Decoder decoder;

    const std::string said = "ł";

    decoder.fed(said.substr(0, 1), sheet);

    same(sheet.much(), std::size_t{0});

    decoder.fed(said.substr(1), sheet);

    same(sheet.much(), std::size_t{1});
    ought(sheet.among()[0].key.letter() == U'ł');
}

TRIAL("Decoder: an emoji outside the basic plane") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("🙂", sheet);

    same(sheet.much(), std::size_t{1});
    ought(sheet.among()[0].key.letter() == U'🙂');
}

TRIAL("Decoder: arrows") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[A\x1b[B\x1b[C\x1b[D", sheet);

    same(sheet.much(), std::size_t{4});
    ought(sheet.among()[0].key.code() == ui::Code::up);
    ought(sheet.among()[1].key.code() == ui::Code::down);
    ought(sheet.among()[2].key.code() == ui::Code::right);
    ought(sheet.among()[3].key.code() == ui::Code::left);
}

TRIAL("Decoder: an arrow with a modifier") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[1;5C", sheet);

    ought(sheet.among()[0].key.code() == ui::Code::right);
    ought(sheet.among()[0].mods.ctrl);
    ought(!sheet.among()[0].mods.alt);
    ought(!sheet.among()[0].mods.shift);
}

TRIAL("Decoder: the modifier mask splits into three") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[1;8A", sheet);

    ought(sheet.among()[0].mods.shift);
    ought(sheet.among()[0].mods.alt);
    ought(sheet.among()[0].mods.ctrl);
}

TRIAL("Decoder: keys with a tilde") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[2~\x1b[3~\x1b[5~\x1b[6~", sheet);

    same(sheet.much(), std::size_t{4});
    ought(sheet.among()[0].key.code() == ui::Code::insert);
    ought(sheet.among()[1].key.code() == ui::Code::remove);
    ought(sheet.among()[2].key.code() == ui::Code::pageup);
    ought(sheet.among()[3].key.code() == ui::Code::pagedown);
}

TRIAL("Decoder: F1 to F4 in both encodings") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1bOP\x1bOQ\x1b[11~\x1b[14~", sheet);

    same(sheet.much(), std::size_t{4});
    ought(sheet.among()[0].key.code() == ui::Code::f1);
    ought(sheet.among()[1].key.code() == ui::Code::f2);
    ought(sheet.among()[2].key.code() == ui::Code::f1);
    ought(sheet.among()[3].key.code() == ui::Code::f4);
}

TRIAL("Decoder: F5 to F12") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[15~\x1b[21~\x1b[23~\x1b[24~", sheet);

    ought(sheet.among()[0].key.code() == ui::Code::f5);
    ought(sheet.among()[1].key.code() == ui::Code::f10);
    ought(sheet.among()[2].key.code() == ui::Code::f11);
    ought(sheet.among()[3].key.code() == ui::Code::f12);
}

TRIAL("Decoder: home and end in both encodings") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[H\x1b[F\x1b[1~\x1b[4~", sheet);

    ought(sheet.among()[0].key.code() == ui::Code::home);
    ought(sheet.among()[1].key.code() == ui::Code::end);
    ought(sheet.among()[2].key.code() == ui::Code::home);
    ought(sheet.among()[3].key.code() == ui::Code::end);
}

TRIAL("Decoder: a sequence split between feeds") {
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

TRIAL("Decoder: a lone Esc waits until someone decides") {
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

TRIAL("Decoder: flush without a pending Esc does nothing") {
    Sheet sheet;
    Decoder decoder;

    decoder.flush(sheet);
    decoder.fed("q", sheet);
    decoder.flush(sheet);

    same(sheet.much(), std::size_t{1});
}

TRIAL("Decoder: flush on an unfinished sequence gives Esc and the rest") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[", sheet);

    ought(decoder.pending());

    decoder.flush(sheet);

    same(sheet.much(), std::size_t{2});
    ought(sheet.among()[0].key.code() == ui::Code::escape);
    ought(sheet.among()[1].key.letter() == U'[');
}

TRIAL("Decoder: Esc with a letter is Alt") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b" "f", sheet);

    same(sheet.much(), std::size_t{1});
    ought(sheet.among()[0].key.letter() == U'f');
    ought(sheet.among()[0].mods.alt);
}

TRIAL("Decoder: Esc after Esc is two presses") {
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

TRIAL("Decoder: mouse pressed and released") {
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

TRIAL("Decoder: mouse buttons") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[<0;1;1M\x1b[<1;1;1M\x1b[<2;1;1M", sheet);

    same(static_cast<int>(sheet.among()[0].button), static_cast<int>(ui::Mouse::left));
    same(static_cast<int>(sheet.among()[1].button), static_cast<int>(ui::Mouse::middle));
    same(static_cast<int>(sheet.among()[2].button), static_cast<int>(ui::Mouse::right));
}

// Mask 35 is motion without a pressed button, exactly what the terminal sends after ?1003. The
// missing button matters here as much as the motion flag itself: by it a widget recognizes hovering
// and tells it apart from a drag, which has the same flag but carries a button.
TRIAL("Decoder: mouse motion carries the motion flag") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[<35;7;9M", sheet);

    same(static_cast<int>(sheet.among()[0].press), static_cast<int>(ui::Press::move));
    same(static_cast<int>(sheet.among()[0].button), static_cast<int>(ui::Mouse::none));
    same(sheet.among()[0].where.x, 6);
    same(sheet.among()[0].where.y, 8);
}

TRIAL("Decoder: a drag carries motion together with the button") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[<32;7;9M", sheet);

    same(static_cast<int>(sheet.among()[0].press), static_cast<int>(ui::Press::move));
    same(static_cast<int>(sheet.among()[0].button), static_cast<int>(ui::Mouse::left));
}

TRIAL("Decoder: the wheel both ways") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[<64;3;3M\x1b[<65;3;3M", sheet);

    same(static_cast<int>(sheet.among()[0].what), static_cast<int>(Told::What::wheel));
    same(sheet.among()[0].steps, 1);
    same(sheet.among()[1].steps, -1);
}

TRIAL("Decoder: mouse modifiers") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[<16;1;1M", sheet);

    ought(sheet.among()[0].mods.ctrl);
    ought(!sheet.among()[0].mods.shift);
}

TRIAL("Decoder: a paste becomes one event") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[200~ala ma kota\x1b[201~", sheet);

    same(sheet.much(), std::size_t{1});
    same(static_cast<int>(sheet.among()[0].what), static_cast<int>(Told::What::paste));
    same(sheet.among()[0].said, std::string{"ala ma kota"});
}

TRIAL("Decoder: a paste split between feeds") {
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

TRIAL("Decoder: a paste with a newline does not become Enter") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[200~dwa\nwiersze\x1b[201~", sheet);

    same(sheet.much(), std::size_t{1});
    same(sheet.among()[0].said, std::string{"dwa\nwiersze"});
}

TRIAL("Decoder: keys after a paste go back to normal") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[200~x\x1b[201~q", sheet);

    same(sheet.much(), std::size_t{2});
    same(static_cast<int>(sheet.among()[0].what), static_cast<int>(Told::What::paste));
    ought(sheet.among()[1].key.letter() == U'q');
}

TRIAL("Decoder: an unknown sequence does not spoil what follows it") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[99;99Z" "q", sheet);

    same(sheet.much(), std::size_t{1});
    ought(sheet.among()[0].key.letter() == U'q');
}

TRIAL("Decoder: broken input does not grow in the buffer without end") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[" + std::string(200, '1'), sheet);

    decoder.fed("q", sheet);

    ought(sheet.much() >= 1);
}

TRIAL("Decoder: two roots get their own, one decoder is enough") {
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

TRIAL("Decoder: a character split between roots goes to the second one") {
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

// Byte by byte gives the same as whole. `Term` feeds the decoder byte by byte and takes the current
// root for each one, because a key in the middle of a chunk that was read can switch the screen
// (`Term::show`), and keys typed ahead must already go to the new one. This works only if the feed
// boundary means nothing to the decoder: letters, UTF-8, keys from sequences, Alt, the mouse, the
// wheel and a bracketed paste come out the same, in the same order.
TRIAL("Decoder: feeding byte by byte gives the same events as feeding whole") {
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

// Switching the root in the middle of a chunk: the handler of the first key changes the target,
// and the rest, fed byte by byte as `Term` does it, already goes to the second root.
TRIAL("Decoder: byte by byte the rest of a chunk goes to the root switched in its middle") {
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


// Terminal answers that arrive late (after the capability probe, over SSH) are not keys.
// Otherwise the background color would become Alt+] and a string of letters, a cursor position
// report Ctrl+F3, and an XTGETTCAP answer Alt+P with the rest typed into a field.
TRIAL("Decoder: late terminal answers are not keys") {
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

    // Byte by byte, as `Term` feeds it: the same.
    Sheet piece;
    Decoder slowly;

    for (const char one : late) slowly.fed(std::string(1, one), piece);

    same(piece.much(), std::size_t{1});
    ought(piece.among()[0].key.letter() == U'q');
}

// F3 with a modifier has the shape of a position report, but with a one in front, so it stays a
// key.
TRIAL("Decoder: Ctrl+F3 stays a key") {
    Sheet sheet;
    Decoder decoder;

    decoder.fed("\x1b[1;5R", sheet);

    same(sheet.much(), std::size_t{1});
    ought(sheet.among()[0].key.code() == ui::Code::f3);
    ought(sheet.among()[0].mods.ctrl);
}

// Esc with a letter that starts a control string waits for the timer: a finished string vanishes,
// an unfinished one is Alt with that letter and plain characters after it.
TRIAL("Decoder: the timer decides Alt with a letter that starts a string") {
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
