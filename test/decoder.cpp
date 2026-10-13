#include "proving.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "talcite/decoder.hpp"
#include "talcite/encoding.hpp"

using namespace talcite;

// The decoder gives values, so most trials compare what it heard written out as text: "Ctrl+Up",
// "down left at 12,4", "wheel +1 vertical", "paste 'ala'". A failure then shows the whole sequence
// of what came out, not a field that differed. Time is a parameter: an `Ear` keeps its own moment
// and moves it by hand, so the trials of the Esc and of the paste's silence do not sleep.

namespace {

using namespace std::chrono_literals;

// The modifiers as a prefix: "Ctrl+Alt+Shift+".
std::string prefixed(ui::Mods mods) {
    std::string made;

    if (mods.ctrl) made += "Ctrl+";
    if (mods.alt) made += "Alt+";
    if (mods.shift) made += "Shift+";

    return made;
}

// The name of a mouse button, or "none".
std::string named(ui::Mouse button) {
    switch (button) {
        case ui::Mouse::left: return "left";
        case ui::Mouse::middle: return "middle";
        case ui::Mouse::right: return "right";
        case ui::Mouse::back: return "back";
        case ui::Mouse::forward: return "forward";
        case ui::Mouse::none: break;
    }

    return "none";
}

// One value written out.
std::string spoken(const Heard& one) {
    if (const auto* struck = std::get_if<Struck>(&one)) {
        const ui::Key key = struck->key;

        if (key.letter() != 0) return prefixed(key.mods()) + "'" + utf8::encode(key.letter()) + "'";

        return prefixed(key.mods()) + ui::to_string(ui::Key{key.code()});
    }

    if (const auto* moused = std::get_if<Moused>(&one)) {
        const std::string press = moused->press == ui::Press::down ? "down" : moused->press == ui::Press::up ? "up" : "move";

        return prefixed(moused->mods) + press + " " + named(moused->button) + " at " + std::to_string(moused->where.x) +
               "," + std::to_string(moused->where.y);
    }

    if (const auto* wheeled = std::get_if<Wheeled>(&one)) {
        return prefixed(wheeled->mods) + "wheel " + (wheeled->steps > 0 ? "+" : "") + std::to_string(wheeled->steps) +
               (wheeled->axis == ui::Axis::horizontal ? " horizontal" : " vertical") + " at " +
               std::to_string(wheeled->where.x) + "," + std::to_string(wheeled->where.y);
    }

    if (const auto* pasted = std::get_if<Pasted>(&one)) return "paste '" + pasted->text + "'" + (pasted->whole ? "" : " cut");

    if (const auto* focused = std::get_if<Focused>(&one)) return focused->in ? "focus in" : "focus out";

    return "?";
}

// All values written out, separated by " | ".
std::string told(const std::vector<Heard>& all) {
    std::string made;

    for (const Heard& one : all) {
        if (!made.empty()) made += " | ";

        made += spoken(one);
    }

    return made;
}

// A decoder with its own moment.
struct Ear {
    Decoder decoder;
    Clock::Moment now{};

    // What the bytes mean, fed at the current moment.
    std::string heard(std::string_view bytes) { return told(decoder.fed(bytes, now)); }

    // What time settled once `by` has passed.
    std::string after(std::chrono::milliseconds by) {
        now += by;

        return told(decoder.lapsed(now));
    }

    // Whether the decoder waits for time to settle something.
    [[nodiscard]] bool waiting() const { return decoder.due().has_value(); }
};

}

// --- Letters and control characters -------------------------------------------------------------

TRIAL("Decoder: letters, one value each") {
    Ear ear;

    same(ear.heard("q"), std::string{"'q'"});
    same(ear.heard("abc"), std::string{"'a' | 'b' | 'c'"});
    ought(!ear.waiting());
}

TRIAL("Decoder: a control character is a letter with Ctrl") {
    Ear ear;

    same(ear.heard("\x01"), std::string{"Ctrl+'a'"});
    same(ear.heard(std::string_view{"\x00", 1}), std::string{"Ctrl+' '"});
    same(ear.heard("\x1c\x1d"), std::string{"Ctrl+'\\' | Ctrl+']'"});
}

// 0x7F is Backspace and 0x08 Ctrl+Backspace, as xterm, VTE and Windows Terminal send them.
TRIAL("Decoder: Tab, Enter and Backspace have their own codes, 0x08 is Ctrl+Backspace") {
    Ear ear;

    same(ear.heard("\x09\x0d\x0a\x7f\x08"), std::string{"Tab | Enter | Enter | Backspace | Ctrl+Backspace"});
    same(ear.heard("\x1b\x08"), std::string{"Ctrl+Alt+Backspace"});
}

TRIAL("Decoder: UTF-8 becomes one character, also when a read cuts it") {
    Ear ear;

    same(ear.heard("ł🙂"), std::string{"'ł' | '🙂'"});

    const std::string said = "ż";

    same(ear.heard(said.substr(0, 1)), std::string{});
    ought(!ear.waiting());
    same(ear.heard(said.substr(1)), std::string{"'ż'"});
}

// An overlong form, a surrogate, a stray continuation and a lead cut by an ASCII byte are dropped;
// the bytes around them are not lost.
TRIAL("Decoder: broken UTF-8 is dropped without taking its neighbours") {
    Ear ear;

    same(ear.heard("a\xc0\x80" "b\xed\xa0\x80" "c\x80" "d\xc5" "e"), std::string{"'a' | 'b' | 'c' | 'd' | 'e'"});
}

// --- Keys with sequences -------------------------------------------------------------------------

TRIAL("Decoder: arrows, Home and End in the CSI and SS3 forms") {
    Ear ear;

    same(ear.heard("\x1b[A\x1b[B\x1b[C\x1b[D\x1bOA\x1b[H\x1b[F\x1b[1~\x1b[4~\x1bOH"),
         std::string{"Up | Down | Right | Left | Up | Home | End | Home | End | Home"});
}

TRIAL("Decoder: modifiers are a mask plus one") {
    Ear ear;

    same(ear.heard("\x1b[1;5C\x1b[1;2A\x1b[1;3B\x1b[1;8D\x1b[3;5~"),
         std::string{"Ctrl+Right | Shift+Up | Alt+Down | Ctrl+Alt+Shift+Left | Ctrl+Del"});
}

TRIAL("Decoder: keys with a tilde") {
    Ear ear;

    same(ear.heard("\x1b[2~\x1b[3~\x1b[5~\x1b[6~"), std::string{"Ins | Del | PgUp | PgDn"});
}

TRIAL("Decoder: F1 to F20 in every form terminals send them") {
    Ear ear;

    same(ear.heard("\x1bOP\x1bOQ\x1bOR\x1bOS\x1b[11~\x1b[14~"), std::string{"F1 | F2 | F3 | F4 | F1 | F4"});
    same(ear.heard("\x1b[15~\x1b[17~\x1b[21~\x1b[23~\x1b[24~"), std::string{"F5 | F6 | F10 | F11 | F12"});
    same(ear.heard("\x1b[25~\x1b[26~\x1b[28~\x1b[29~\x1b[31~\x1b[32~\x1b[33~\x1b[34~"),
         std::string{"F13 | F14 | F15 | F16 | F17 | F18 | F19 | F20"});
}

// The Linux console sends F1–F5 as `CSI [ A` to `CSI [ E`; before, they came out as arrows.
TRIAL("Decoder: the Linux console's F1 to F5") {
    Ear ear;

    same(ear.heard("\x1b[[A\x1b[[B\x1b[[C\x1b[[D\x1b[[E"), std::string{"F1 | F2 | F3 | F4 | F5"});
}

// Shift+Tab is `CSI Z` in every VT terminal and in the translation of the old Windows console.
TRIAL("Decoder: Shift+Tab") {
    Ear ear;

    same(ear.heard("\x1b[Z"), std::string{"Shift+Tab"});
    same(ear.heard("\x1b[1;5Z"), std::string{"Ctrl+Shift+Tab"});
}

// Old xterms put the modifier between SS3 and the letter.
TRIAL("Decoder: SS3 with a modifier") {
    Ear ear;

    same(ear.heard("\x1bO5P\x1bO2A"), std::string{"Ctrl+F1 | Shift+Up"});
}

TRIAL("Decoder: the keypad in application mode") {
    Ear ear;

    same(ear.heard("\x1bOM\x1bOp\x1bOy\x1bOj\x1bOn\x1bOX"), std::string{"Enter | '0' | '9' | '*' | '.' | '='"});
}

// rxvt marks the modifiers of a tilde key by the final byte, and Shift and Ctrl of an arrow by a
// lowercase letter. Its Shift form, `CSI n $`, has no final byte at all and is settled by time.
TRIAL("Decoder: rxvt's modified keys") {
    Ear ear;

    same(ear.heard("\x1b[3^\x1b[5@\x1b[a\x1bOd"), std::string{"Ctrl+Del | Ctrl+Shift+PgUp | Shift+Up | Ctrl+Left"});

    same(ear.heard("\x1b[2$"), std::string{});
    ought(ear.waiting());
    same(ear.after(50ms), std::string{"Shift+Ins"});
}

// The kitty keyboard protocol and xterm's modifyOtherKeys give keys that the old forms cannot:
// Ctrl+Enter, Shift+Enter, Ctrl with any letter. A release is not a key.
TRIAL("Decoder: keys of the kitty protocol and of modifyOtherKeys") {
    Ear ear;

    same(ear.heard("\x1b[97;5u\x1b[13;2u\x1b[27u\x1b[9;5u\x1b[127;3u"),
         std::string{"Ctrl+'a' | Shift+Enter | Esc | Ctrl+Tab | Alt+Backspace"});
    same(ear.heard("\x1b[97;5:3u\x1b[97;5:2u"), std::string{"Ctrl+'a'"});
    same(ear.heard("\x1b[57399u\x1b[57414u\x1b[57376;5u\x1b[57441u"), std::string{"'0' | Enter | Ctrl+F13"});
    same(ear.heard("\x1b[27;5;13~\x1b[27;6;65~"), std::string{"Ctrl+Enter | Ctrl+Shift+'A'"});
}

TRIAL("Decoder: an unknown sequence does not spoil what follows it") {
    Ear ear;

    same(ear.heard("\x1b[99;99Y\x1b[E\x1b[9~q"), std::string{"'q'"});
}

// A number past what any sequence means saturates instead of overflowing.
TRIAL("Decoder: a run of digits does not overflow") {
    Ear ear;

    same(ear.heard("\x1b[" + std::string(40, '9') + "~q"), std::string{"'q'"});
    same(ear.heard("\x1b[" + std::string(40, '9') + ";5Aq"), std::string{"Ctrl+Up | 'q'"});
}

// A sequence longer than any key's is garbage: it is read to its final byte and dropped whole, so
// its digits do not turn into keys, and what follows is read as before.
TRIAL("Decoder: an overlong sequence is dropped whole") {
    Ear ear;

    same(ear.heard("\x1b[" + std::string(200, '1') + "Aqw"), std::string{"'q' | 'w'"});
}

// Inside a sequence CAN cancels it, Esc starts a new one and another control character takes
// effect at once, as in a terminal's own parser.
TRIAL("Decoder: control characters inside a sequence") {
    Ear ear;

    same(ear.heard("\x1b[1;\x18q"), std::string{"'q'"});
    same(ear.heard("\x1b[1;\x1b[B"), std::string{"Down"});
    same(ear.heard("\x1b[1;\x03" "5A"), std::string{"Ctrl+'c' | Ctrl+Up"});
}

// --- Esc and time -------------------------------------------------------------------------------

TRIAL("Decoder: a sequence cut between reads waits and comes out whole") {
    Ear ear;

    same(ear.heard("\x1b"), std::string{});
    ought(ear.waiting());
    same(ear.heard("["), std::string{});
    same(ear.heard("A"), std::string{"Up"});
    ought(!ear.waiting());
}

TRIAL("Decoder: a lone Esc becomes the key once fifty milliseconds pass") {
    Ear ear;

    same(ear.heard("\x1b"), std::string{});
    ought(ear.decoder.due() == std::optional<Clock::Moment>{ear.now + 50ms});
    same(ear.after(49ms), std::string{});
    same(ear.after(1ms), std::string{"Esc"});
    ought(!ear.waiting());
    same(ear.after(100ms), std::string{});
}

// The time counts from the last bytes, so the rest of a sequence that comes a moment later still
// finds it waiting.
TRIAL("Decoder: the wait counts from the last bytes") {
    Ear ear;

    same(ear.heard("\x1b"), std::string{});
    ear.now += 40ms;
    same(ear.heard("["), std::string{});
    same(ear.after(40ms), std::string{});
    same(ear.heard("A"), std::string{"Up"});
}

TRIAL("Decoder: Esc with a letter is Alt") {
    Ear ear;

    same(ear.heard("\x1b" "f\x1bż\x1b\x0d"), std::string{"Alt+'f' | Alt+'ż' | Alt+Enter"});
}

TRIAL("Decoder: Esc after Esc is two presses") {
    Ear ear;

    same(ear.heard("\x1b\x1b"), std::string{"Esc"});
    ought(ear.waiting());
    same(ear.after(50ms), std::string{"Esc"});
}

// `Esc [` and `Esc O` with nothing after them are what a terminal sends for Alt+[ and Alt+O, not Esc
// and a letter; an unfinished sequence is Alt with its second byte and the rest typed after it.
TRIAL("Decoder: an unfinished escape is Alt with its second byte") {
    Ear ear;

    same(ear.heard("\x1b["), std::string{});
    same(ear.after(50ms), std::string{"Alt+'['"});

    same(ear.heard("\x1bO"), std::string{});
    same(ear.after(50ms), std::string{"Alt+'O'"});

    same(ear.heard("\x1b[1;5"), std::string{});
    same(ear.after(50ms), std::string{"Alt+'[' | '1' | ';' | '5'"});
}

// Alt+O typed quickly before a character that cannot end a single shift.
TRIAL("Decoder: Alt+O before a space is not a single shift") {
    Ear ear;

    same(ear.heard("\x1bO x"), std::string{"Alt+'O' | ' ' | 'x'"});
}

// Esc with a letter that opens a control string waits for the timer: a finished string vanishes,
// an unfinished one is Alt with that letter and plain characters after it.
TRIAL("Decoder: the timer decides Alt with a letter that opens a string") {
    Ear ear;

    same(ear.heard("\x1bP"), std::string{});
    same(ear.after(50ms), std::string{"Alt+'P'"});

    same(ear.heard("\x1b]ab"), std::string{});
    same(ear.after(50ms), std::string{"Alt+']' | 'a' | 'b'"});
}

// --- Terminal answers and focus -----------------------------------------------------------------

// Answers that come late (after the capability probe, over SSH) are not keys. Otherwise the
// background color would become Alt+] and a string of letters, a position report Ctrl+F3, and an
// XTGETTCAP answer Alt+P with the rest typed into a field.
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
        "\x1bP1+r524742\x07"
        "q";

    Ear whole;

    same(whole.heard(late), std::string{"'q'"});
    ought(!whole.waiting());

    Ear slowly;
    std::string said;

    for (const char one : late) said += slowly.heard(std::string(1, one));

    same(said, std::string{"'q'"});
}

// F3 with a modifier has the shape of a position report, but with a one in front.
TRIAL("Decoder: Ctrl+F3 stays a key") {
    Ear ear;

    same(ear.heard("\x1b[1;5R\x1b[R"), std::string{"Ctrl+F3 | F3"});
}

// A string longer than any answer is garbage: dropped whole, so its text does not come out as
// keys.
TRIAL("Decoder: an overlong control string is dropped whole") {
    Ear ear;

    same(ear.heard("\x1b]52;c;" + std::string(100000, 'A') + "\x07q"), std::string{"'q'"});
}

TRIAL("Decoder: the terminal's focus") {
    Ear ear;

    same(ear.heard("\x1b[I\x1b[O"), std::string{"focus in | focus out"});
}

// --- The mouse ----------------------------------------------------------------------------------

TRIAL("Decoder: SGR mouse: press, release, buttons and modifiers") {
    Ear ear;

    same(ear.heard("\x1b[<0;13;5M\x1b[<0;13;5m"), std::string{"down left at 12,4 | up left at 12,4"});
    same(ear.heard("\x1b[<1;1;1M\x1b[<2;1;1M\x1b[<16;1;1M\x1b[<12;1;1M"),
         std::string{"down middle at 0,0 | down right at 0,0 | Ctrl+down left at 0,0 | Alt+Shift+down left at 0,0"});
}

// Mask 35 is motion without a pressed button, exactly what the terminal sends after ?1003. By the
// missing button a widget tells hovering from a drag, which has the same flag but carries a
// button.
TRIAL("Decoder: SGR mouse: hovering and dragging") {
    Ear ear;

    same(ear.heard("\x1b[<35;7;9M\x1b[<32;7;9M"), std::string{"move none at 6,8 | move left at 6,8"});
}

// Up and down are 64 and 65, left and right 66 and 67; a step towards the start is positive.
TRIAL("Decoder: the wheel in both axes") {
    Ear ear;

    same(ear.heard("\x1b[<64;3;3M\x1b[<65;3;3M"), std::string{"wheel +1 vertical at 2,2 | wheel -1 vertical at 2,2"});
    same(ear.heard("\x1b[<66;3;3M\x1b[<67;3;3M\x1b[<68;3;3M"),
         std::string{"wheel +1 horizontal at 2,2 | wheel -1 horizontal at 2,2 | Shift+wheel +1 vertical at 2,2"});
}

// Buttons 8 and 9 are back and forward; 10 and 11 have no agreed meaning and are left out.
TRIAL("Decoder: the back and forward buttons") {
    Ear ear;

    same(ear.heard("\x1b[<128;4;2M\x1b[<128;4;2m\x1b[<129;4;2M\x1b[<130;4;2M\x1b[<131;4;2M"),
         std::string{"down back at 3,1 | up back at 3,1 | down forward at 3,1"});
}

TRIAL("Decoder: a report outside the window is dropped") {
    Ear ear;

    same(ear.heard("\x1b[<0;0;5M\x1b[<0;5;0M\x1b[<0;5Mq"), std::string{"'q'"});
}

// X10 (DECSET 1000 without 1006) is `CSI M` and three bytes plus 32. Its release does not say
// which button went up, so the decoder remembers which one went down. Before, the three bytes were
// typed into the field under the pointer.
TRIAL("Decoder: X10 mouse") {
    Ear ear;

    same(ear.heard("\x1b[M ,%\x1b[M#,%"), std::string{"down left at 11,4 | up left at 11,4"});
    same(ear.heard("\x1b[M\"!!\x1b[M#!!"), std::string{"down right at 0,0 | up right at 0,0"});
    same(ear.heard("\x1b[MC!!\x1b[M`!!\x1b[Mb!!"),
         std::string{"move none at 0,0 | wheel +1 vertical at 0,0 | wheel +1 horizontal at 0,0"});
    same(ear.heard("\x1b[M"), std::string{});
    same(ear.heard(" "), std::string{});
    same(ear.heard("!!q"), std::string{"down left at 0,0 | 'q'"});
}

// A cell past 223 cannot be written in X10; xterm sends a zero byte, which is no cell.
TRIAL("Decoder: an X10 report it could not encode is dropped") {
    Ear ear;

    same(ear.heard(std::string{"\x1b[M ", 4} + std::string(1, '\0') + "!q"), std::string{"'q'"});
}

// urxvt (DECSET 1015) writes the X10 mask and the cell in decimal.
TRIAL("Decoder: urxvt mouse") {
    Ear ear;

    same(ear.heard("\x1b[32;10;5M\x1b[35;10;5M\x1b[96;10;5M"),
         std::string{"down left at 9,4 | up left at 9,4 | wheel +1 vertical at 9,4"});
}

// --- Pastes -------------------------------------------------------------------------------------

TRIAL("Decoder: a paste is one value, with its newlines and escapes as text") {
    Ear ear;

    same(ear.heard("\x1b[200~ala ma kota\x1b[201~"), std::string{"paste 'ala ma kota'"});
    same(ear.heard("\x1b[200~dwa\nwiersze \x1b[A\x1b[201~q"), std::string{"paste 'dwa\nwiersze \x1b[A' | 'q'"});
}

TRIAL("Decoder: a paste split between reads, also in its end marker") {
    Ear ear;

    same(ear.heard("\x1b[200~ala "), std::string{});
    same(ear.heard("ma kota"), std::string{});
    same(ear.heard("\x1b[20"), std::string{});
    same(ear.heard("1~"), std::string{"paste 'ala ma kota'"});
}

// The end marker cut between two reads further apart than the Esc's wait: before, the Esc's timer
// took the marker's start for the Esc key, and the paste never ended.
TRIAL("Decoder: a paste does not end by the Esc's timer") {
    Ear ear;

    same(ear.heard("\x1b[200~ala\x1b[20"), std::string{});
    ought(ear.decoder.due() == std::optional<Clock::Moment>{ear.now + 2s});
    same(ear.after(60ms), std::string{});
    same(ear.after(1s), std::string{});
    same(ear.heard("1~q"), std::string{"paste 'ala' | 'q'"});
}

// A paste whose end never comes (a terminal that died, a broken SSH connection) is closed with
// what came after two seconds of silence, and says so; the keys after it are keys again.
TRIAL("Decoder: a paste without its end closes after two seconds of silence") {
    Ear ear;

    same(ear.heard("\x1b[200~ala\x1b[2"), std::string{});
    same(ear.after(1999ms), std::string{});
    same(ear.after(1ms), std::string{"paste 'ala\x1b[2' cut"});
    same(ear.heard("q"), std::string{"'q'"});
}

// --- The whole way -------------------------------------------------------------------------------

// Byte by byte gives the same as whole: the feed boundary means nothing to the decoder.
TRIAL("Decoder: feeding byte by byte gives the same values as feeding whole") {
    const std::string said = std::string{"aż"} + "\x1b[A" + "\x1b[1;5C" + "\x1bx" + "\x1b[15~" + "\x1b[<0;10;5M" +
                             "\x1b[<0;10;5m" + "\x1b[<64;3;4M" + "\x1b[M ,%" + "\x1b[200~wklej\nto\x1b[201~" + "🙂" +
                             "\t" + "\x1b[97;5u" + "\x1b[[B" + "\x1b]11;rgb:0/0/0\x07" + "\x1bOM";

    Ear whole;
    Ear bytes;

    const std::string all = whole.heard(said);

    std::vector<Heard> pieces;

    for (const char each : said) {
        std::vector<Heard> some = bytes.decoder.fed(std::string_view{&each, 1}, bytes.now);

        pieces.insert(pieces.end(), some.begin(), some.end());
    }

    same(told(pieces), all);
    ought(!whole.waiting());
    ought(!bytes.waiting());
    same(all, std::string{"'a' | 'ż' | Up | Ctrl+Right | Alt+'x' | F5 | down left at 9,4 | up left at 9,4 | wheel +1 "
                          "vertical at 2,3 | down left at 11,4 | paste 'wklej\nto' | '🙂' | Tab | Ctrl+'a' | F2 | Enter"});
}

namespace {

// A root that writes down every event it gets, the way `spoken` writes values, so a trial sees
// that `dispatched` gives each value its event with everything in it.
class Sheet : public ui::Root {
public:
    std::vector<std::string> among;

protected:
    // Records a key.
    bool handle(ui::KeyEvent& what) override {
        among.push_back(spoken(Struck{what.key()}));

        return true;
    }

    // Records a mouse event.
    bool handle(ui::MouseEvent& what) override {
        among.push_back(spoken(Moused{what.where(), what.press(), what.button(), what.mods()}));

        return true;
    }

    // Records a wheel event with its axis.
    bool handle(ui::WheelEvent& what) override {
        among.push_back(spoken(Wheeled{what.where(), what.steps(), what.axis(), what.mods()}));

        return true;
    }

    // Records a paste.
    bool handle(ui::PasteEvent& what) override {
        among.push_back(spoken(Pasted{what.text()}));

        return true;
    }
};

}

TRIAL("Decoder: every value reaches the root as its event") {
    Ear ear;
    Sheet sheet;

    const std::vector<Heard> all = ear.decoder.fed("q\x1b[1;5A\x1b[<2;4;3M\x1b[<67;1;1M\x1b[200~x\x1b[201~\x1b[I", ear.now);

    for (const Heard& one : all) (void)dispatched(one, sheet);

    ought(sheet.among == std::vector<std::string>{"'q'", "Ctrl+Up", "down right at 3,2", "wheel -1 horizontal at 0,0",
                                                 "paste 'x'"});
    ought(!dispatched(Focused{true}, sheet));
}
