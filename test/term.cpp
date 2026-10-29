#include "proving.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "faking.hpp"
#include "talcite/clock.hpp"
#include "talcite/encoding.hpp"
#include "talcite/term.hpp"

using namespace talcite;

// `Term` talks to the terminal only through `Console`, so with a fake every byte is visible: the
// frame, dressing the terminal and undressing it, the title, and also what comes in: keys, size and
// mode.

namespace {

// A root that has a single letter in pure red in its upper left corner.
class Red : public ui::Root {
public:
    using ui::Root::Root;

protected:
    // Draws the red `x` at (0, 0) on an otherwise empty page.
    View paint(ui::Size given, ui::Area seen) override {
        (void)seen;

        View made{given};

        Style how;
        how.foreground = Color{false, 255, 0, 0};

        made.put({0, 0}, U'x', how);

        return made;
    }
};

// The red root with its caret in the third column of the first row, as an editor would have it.
class Pointed : public Red {
public:
    // The place of the caret.
    [[nodiscard]] std::optional<ui::Spot> caret() const override { return ui::Spot{2, 0}; }
};

// The pointed root whose cursor asks for a shape.
class Shaped : public Pointed {
public:
    using Pointed::caret_shape;

    // The shape asked for.
    [[nodiscard]] ui::CaretShape caret_shape() const override { return shape; }

    ui::CaretShape shape{ui::Caret::underline, true};
};

// What went to the terminal after the probe: the probe says its piece through the same console, and
// what matters here is what `Term` itself writes.
std::string drawn(const std::shared_ptr<faking::Terminal>& line, Setup given = {}) {
    Red root;

    Term term{std::move(given), Seam<Console>{line}};

    term.show(root);

    line->written.clear();

    term.draw();

    return line->written;
}

// Whether `what` occurs anywhere in `where`.
bool holds(const std::string& where, const std::string& what) {
    return where.find(what) != std::string::npos;
}

}

// A root that writes text with an escape sequence in it, as it could arrive in a file name.
class Hostile : public ui::Root {
protected:
    // Puts the text with the escape sequences at (0, 0).
    View paint(ui::Size given, ui::Area seen) override {
        (void)seen;

        View made{given};

        made.put({0, 0}, std::string{"\x1b]52;c;cHduZWQ=\x07\x1bZ\xc2\x9b""31m"}, Style{});

        return made;
    }
};

// --- Colors --------------------------------------------------------------------------------------

TRIAL("Term: the 256-color palette comes out as 38;5") {
    const std::string said = drawn(faking::terminal());

    ought(holds(said, "\x1b[0;38;5;196mx"));
}

TRIAL("Term: sixteen colors come out as bright red") {
    auto line = faking::terminal();

    line->environment["TERM"] = "vt220";

    ought(holds(drawn(line), "\x1b[0;91mx"));
}

TRIAL("Term: NO_COLOR leaves the character alone without color") {
    auto line = faking::terminal();

    line->environment["NO_COLOR"] = "1";

    const std::string said = drawn(line);

    ought(holds(said, "\x1b[0mx"));
    ought(!holds(said, "38;"));
}

// A wish beats a measurement: `Setup` says what to draw with, whatever the console claims.
TRIAL("Term: Setup overrides the detected colors") {
    Setup given;
    given.colors = Colors::rgb;

    ought(holds(drawn(faking::terminal(), given), "\x1b[0;38;2;255;0;0mx"));
}

TRIAL("Term: the probe goes through the same console") {
    auto line = faking::terminal();

    Term term{Setup{}, Seam<Console>{line}};

    ought(line->written.rfind("\x1b[>c", 0) == 0);
}

// --- Dressing the terminal -----------------------------------------------------------------------

namespace {

// A setup that asks for the alternate screen, SGR mouse reports and bracketed paste.
Setup dressy() {
    Setup made;
    made.screen = Screen::apart;
    made.mouse = Mouse::sgr;
    made.bracketed_paste = true;

    return made;
}

}

// The alternate screen goes first when dressing and last when undressing; otherwise the mouse and
// paste sequences would land on a different buffer from the one where they stay.
TRIAL("Term: dressing and undressing go in opposite order") {
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

// A terminal without SGR gets drags and the urxvt encoding too, and loses them in reverse order.
TRIAL("Term: the mouse without SGR asks for drags and the decimal encoding") {
    auto line = faking::terminal();

    std::string on;

    {
        Setup wish;
        wish.mouse = Mouse::basic;

        Term term{wish, Seam<Console>{line}};

        on = line->written;

        line->written.clear();
    }

    ought(holds(on, "\x1b[?1000h\x1b[?1002h\x1b[?1015h"));
    ought(!holds(on, "\x1b[?1006h"));
    ought(holds(line->written, "\x1b[?1015l\x1b[?1002l\x1b[?1000l"));
}

TRIAL("Term: without wishes the mouse and the screen follow the measurement") {
    auto line = faking::terminal();

    line->environment["TERM"] = "dumb";

    Term term{Setup{}, Seam<Console>{line}};

    ought(!holds(line->written, "\x1b[?1000h"));
    ought(!holds(line->written, "\x1b[?47h"));
    ought(!holds(line->written, "\x1b[?1049h"));
}

// Stepping aside takes everything off, coming back puts it on again, and a nested step aside does
// not send a second set.
TRIAL("Term: Pause takes off once and puts on once") {
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

// The dressing hides the cursor. Coming back from another program, or resuming after Ctrl+Z, dresses
// the terminal again, so the next frame has to show the cursor again; an editor would otherwise go
// on without one.
TRIAL("Term: the cursor shows again after stepping aside") {
    auto line = faking::terminal();

    Pointed root;

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);
    term.draw();

    ought(holds(line->written, "\x1b[?25h"));

    {
        const Term::Pause away{term};
    }

    line->written.clear();

    term.draw();

    ought(holds(line->written, "\x1b[?25h"));
}

// The cursor's shape goes to the terminal once, when it changes, before the cursor shows; a root
// that asks for none sends nothing about it. At the end the terminal's usual cursor comes back, and
// after stepping aside the shape goes again, since another program may have changed it.
TRIAL("Term: the cursor's shape goes once, comes again after stepping aside, and goes back at the end") {
    {
        auto line = faking::terminal();

        Pointed root;

        Term term{Setup{}, Seam<Console>{line}};

        term.show(root);
        term.draw();

        ought(!holds(line->written, " q"));
    }

    auto line = faking::terminal();

    Shaped root;

    {
        Term term{Setup{}, Seam<Console>{line}};

        term.show(root);
        term.draw();

        ought(holds(line->written, "\x1b[3 q"));
        ought(line->written.find("\x1b[3 q") < line->written.rfind("\x1b[?25h"));

        line->written.clear();

        root.shape = ui::CaretShape{ui::Caret::bar, false};

        term.draw();

        ought(holds(line->written, "\x1b[6 q"));

        line->written.clear();

        term.draw();

        ought(!holds(line->written, " q"));

        {
            const Term::Pause away{term};
        }

        ought(holds(line->written, "\x1b[0 q"));

        line->written.clear();

        term.draw();

        ought(holds(line->written, "\x1b[6 q"));

        line->written.clear();
    }

    ought(holds(line->written, "\x1b[0 q"));
}

TRIAL("Term: draws nothing while stepped aside") {
    auto line = faking::terminal();

    Red root;

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    const Term::Pause away{term};

    line->written.clear();

    term.draw();

    same(line->written, std::string{});
}

// --- Frame ---------------------------------------------------------------------------------------

TRIAL("Term: a second identical frame sends not a single byte") {
    auto line = faking::terminal();

    Red root;

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);
    term.draw();

    line->written.clear();

    term.draw();

    same(line->written, std::string{});
}

TRIAL("Term: the frame is synchronized when allowed") {
    Setup given;
    given.synchronized = true;

    const std::string said = drawn(faking::terminal(), given);

    ought(said.rfind("\x1b[?2026h", 0) == 0);
    ought(said.size() >= 8 && said.compare(said.size() - 8, 8, "\x1b[?2026l") == 0);
}

// --- Title ---------------------------------------------------------------------------------------

TRIAL("Term: the title goes out as OSC 0") {
    auto line = faking::terminal();

    Term term{Setup{}, Seam<Console>{line}};

    line->written.clear();

    term.title("abc");

    same(line->written, std::string{"\x1b]0;abc\x1b\\"});
}

TRIAL("Term: a title forbidden in Setup does not go out") {
    auto line = faking::terminal();

    Setup given;
    given.titles = false;

    Term term{given, Seam<Console>{line}};

    line->written.clear();

    term.title("abc");

    same(line->written, std::string{});
}

// The change goes through the loop, so only a turn carries it out, and it sends just the difference.
TRIAL("Term: reconfigure sends just the difference") {
    auto line = faking::terminal();

    Term term{dressy(), Seam<Console>{line}};

    Setup given = dressy();
    given.bracketed_paste = false;

    term.reconfigure(given);

    line->written.clear();

    (void)term.loop().turn();

    same(line->written, std::string{"\x1b[?2004l"});
}

// --- Raw mode, input and size --------------------------------------------------------------------
//
// This also goes through the console: `Term` has no system code, so the fake answers for everything
// the terminal does, and the trial checks a turn from key to frame without a terminal.

namespace {

// A root that records what reached it.
class Heard : public Red {
public:
    using Red::Red;

    std::u32string keys;
    std::vector<ui::Size> sizes;

protected:
    using Red::handle;

    // Records the key, with Esc written as ⎋, and consumes it.
    bool handle(ui::KeyEvent& what) override {
        keys += what.key().code() == ui::Code::escape ? U'⎋' : what.key().letter();

        return true;
    }

    // Records the new size and lets the event go on.
    bool handle(ui::SizeEvent& what) override {
        sizes.push_back({what.wide(), what.high()});

        return false;
    }
};

}

TRIAL("Term: raw mode from construction to the end, the probe nested inside it") {
    auto line = faking::terminal();

    {
        Term term{Setup{}, Seam<Console>{line}};

        same(line->depth, 1);
        same(line->entered, 2);
    }

    same(line->depth, 0);
}

TRIAL("Term: Pause takes off raw mode, coming back puts it on and measures the window again") {
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

TRIAL("Term: the console does not listen while stepped aside") {
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

TRIAL("Term: a key from the console reaches the root") {
    auto line = faking::terminal();

    Heard root;

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    line->replies.push_back("ab");

    (void)term.turn();

    ought(root.keys == U"ab");
}

// Bytes that arrived during the probe and were not an answer are keys pressed at startup. They go
// first, before anything fresh.
TRIAL("Term: keys from the probe go before fresh ones") {
    auto line = faking::terminal("q\x1b[?62;1c");

    Heard root;

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    line->replies.push_back("a");

    (void)term.turn();

    ought(root.keys == U"qa");
}

TRIAL("Term: a lone Esc waits fifty milliseconds and resolves by itself") {
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

// With no Esc in the decoder and no request from the tree there is nothing to resolve, so the
// console waits without end, for a key or for the loop's bell that `Term` brings it.
TRIAL("Term: with no reason it waits without a limit, but with the bell") {
    auto line = faking::terminal();

    Heard root;

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    const int before = line->belled;

    (void)term.turn();

    ought(!line->patience.back().has_value());
    same(line->belled, before + 1);
}

// The nearest appointment with the root's schedule bounds the wait for the console; the tree itself
// is not asked. The root stands on a manual clock, so the wait is exact however long the turn takes.
TRIAL("Term: an appointment with the root's schedule goes to the console as patience") {
    auto line = faking::terminal();

    ManualClock time;
    Heard root{time};

    const ui::Timer later = root.schedule().after(std::chrono::milliseconds{30}, [] {});

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    (void)term.turn();

    ought(line->patience.back().has_value());
    same(*line->patience.back(), std::chrono::milliseconds{30});
}

// What the schedule has due is called in the turn after the wait, once.
TRIAL("Term: an appointment due is called in the turn") {
    auto line = faking::terminal();

    Heard root;

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    (void)term.turn();

    int called = 0;

    const ui::Timer now = root.schedule().after(std::chrono::milliseconds{0}, [&called] { ++called; });

    (void)term.turn();
    (void)term.turn();

    same(called, 1);
    ought(!now.active());
}

TRIAL("Term: the size from the console at startup, and 80×25 without it") {
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

TRIAL("Term: a size change arrives as SizeEvent and repaints everything") {
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

// Resumed after being stopped: the shell had the terminal meanwhile, took our dressing off it and
// wrote over the screen. The dressing comes back and the frame is painted whole, once.
TRIAL("Term: resuming after a stop dresses the terminal and paints everything") {
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

TRIAL("Term: the double-click time comes from the console") {
    auto line = faking::terminal();

    line->twice = std::chrono::milliseconds{777};

    Heard root;

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    ought(root.double_click() == std::chrono::milliseconds{777});
}

namespace {

// A root that switches the terminal to another root on `x`, the way twa switches screens.
class Switching : public Heard {
public:
    Switching(Term& term, ui::Root& next) : term_(term), next_(next) {}

protected:
    using Heard::handle;

    // Records the key like `Heard`, and on `x` shows the other root.
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

// A switching key in the middle of a chunk that was read: the rest, typed ahead, already belongs to
// the new root.
TRIAL("Term: keys after switching the root in the middle of a chunk go to the new one") {
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

// An escape sequence in text does not reach the terminal: not a write to the clipboard (OSC 52),
// not a request to identify itself (DECID, which the terminal answers on input), not a
// single-character CSI from C1. Only our own sequences leave for the terminal.
TRIAL("Term: text with an escape sequence does not command the terminal") {
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

// --- Hyperlinks ----------------------------------------------------------------------------------

namespace {

// A root with the word "go" at (1, 0) under a hyperlink, and "x" after it, outside the link.
class Linking : public ui::Root {
public:
    explicit Linking(std::string address) : target(hyperlink(std::move(address))) {}

    Hyperlink target;

protected:
    // Puts "go" with its link and the plain "x" on an otherwise empty page.
    View paint(ui::Size given, ui::Area seen) override {
        (void)seen;

        View made{given};

        made.put({1, 0}, "go x", Style{});
        made.link({{1, 0}, {2, 1}}, target);

        return made;
    }
};

// A fake terminal that takes escape sequences: links go only to such a one, and on Windows the
// console says so itself (`Console::vt`), while the fake says no by default.
std::shared_ptr<faking::Terminal> sequenced() {
    auto made = faking::terminal();

    made->sequences = true;

    return made;
}

// What `Term` writes for one frame of the root, after the probe.
std::string linked(const std::shared_ptr<faking::Terminal>& line, Linking& root, Setup given = {}) {
    Term term{std::move(given), Seam<Console>{line}};

    term.show(root);

    line->written.clear();

    term.draw();

    return line->written;
}

}

// The link opens right before its first cell, with its mark as the id, and ends right after its last
// one, before the plain text; an address goes out in printable ASCII, every other byte escaped.
TRIAL("Term: a hyperlink goes out as OSC 8 around its cells, its address escaped") {
    Linking root{"https://example.org/a b/\xc3\xbc"};

    const std::string said = linked(sequenced(), root);

    const std::string opening = "\x1b]8;id=" + std::to_string(root.target.mark) + ";https://example.org/a%20b/%C3%BC\x1b\\";
    const std::string closing = "\x1b]8;;\x1b\\";

    const std::size_t opened = said.find(opening);
    const std::size_t word = said.find("go");
    const std::size_t closed = said.find(closing);
    const std::size_t after = said.find('x', word);

    ought(opened != std::string::npos);
    ought(opened < word);
    ought(word < closed);
    ought(closed < after);
    ought(said.find("\x1b]8", closed + closing.size()) == std::string::npos);
}

// A terminal that is not given links gets the cells as plain text: forbidden in `Setup`, on the
// Linux console, and for an address longer than terminals keep.
TRIAL("Term: without hyperlinks the cells go out as plain text") {
    Setup forbidding;

    forbidding.links = false;

    Linking root{"https://example.org/"};

    const std::string forbidden = linked(sequenced(), root, forbidding);

    ought(!holds(forbidden, "\x1b]8"));
    ought(holds(forbidden, "go"));

    auto kernel = sequenced();

    kernel->environment["TERM"] = "linux";

#ifndef _WIN32
    ought(!holds(linked(kernel, root), "\x1b]8"));
#endif

    Linking far{"https://example.org/" + std::string(3000, 'a')};

    const std::string long_one = linked(sequenced(), far);

    ought(!holds(long_one, "\x1b]8"));
    ought(holds(long_one, "go"));
}

// A link is part of the cell for comparing frames: the same frame again sends nothing, and a new
// address, which comes with a new mark, rewrites the link's cells under it.
TRIAL("Term: the same link sends nothing again, a new address rewrites its cells") {
    auto line = sequenced();

    Linking root{"https://example.org/one"};

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);
    term.draw();

    line->written.clear();

    term.draw();

    same(line->written, std::string{});

    root.target = hyperlink("https://example.org/two");

    term.draw();

    ought(holds(line->written, "\x1b]8;id=" + std::to_string(root.target.mark) + ";https://example.org/two\x1b\\go"));

    const std::string tail = std::string{"go"} + "\x1b]8;;\x1b\\";

    ought(line->written.size() >= tail.size() && line->written.compare(line->written.size() - tail.size(), tail.size(), tail) == 0);
}

// --- Words the terminal counts otherwise ---------------------------------------------------------

namespace {

// A root that writes its text at (0, 0) in its style.
class Worded : public ui::Root {
public:
    std::string text;
    Style style;

protected:
    // Puts the text at the start of an otherwise empty page.
    View paint(ui::Size given, ui::Area seen) override {
        (void)seen;

        View made{given};

        made.put({0, 0}, text, style);

        return made;
    }
};

// How a modelled terminal moves its cursor after a character: by `advance` cells, where zero joins
// the character to the cell before.
using Counting = int (*)(char32_t before, char32_t one);

// A terminal following UAX #29 with conjuncts: a spacing mark, a virama and a consonant after a
// virama join the cell before, so it draws "इक्का" in two cells where talcite counts four.
int narrower(char32_t before, char32_t one) {
    if ((one >= 0x93E && one <= 0x940) || one == 0x94D) return 0;

    return before == 0x94D ? 0 : 1;
}

// A terminal that gives the Devanagari spacing vowels two cells each.
int wider(char32_t before, char32_t one) {
    (void)before;

    if (one >= 0x93E && one <= 0x940) return 2;

    return one == 0x94D ? 0 : 1;
}

// The first row of a modelled terminal: it reads cursor jumps (CUP), erasing (ECH, ED 2) and text,
// passes over every other sequence, and moves its cursor by its own count.
struct Modelled {
    Counting counting;
    std::vector<std::string> cells = std::vector<std::string>(24, " ");
    int x = 0;
    int y = 0;
    char32_t before = 0;

    // Reads everything the terminal was sent.
    void fed(const std::string& bytes) {
        std::size_t at = 0;

        while (at < bytes.size()) {
            if (bytes[at] != '\x1b') {
                const char32_t one = utf8::decode(bytes, at);

                lettered(one);

                continue;
            }

            ++at;

            if (at < bytes.size() && bytes[at] == '[') {
                std::size_t end = ++at;

                while (end < bytes.size() && !(bytes[end] >= 0x40 && bytes[end] <= 0x7E)) ++end;

                commanded(bytes.substr(at, end - at), bytes[end]);

                at = end + 1;
            } else {
                // A string (OSC, APC, DCS) ends with ST or BEL; any other escape is two bytes.
                const std::size_t st = bytes.find("\x1b\\", at);
                const std::size_t bel = bytes.find('\a', at);
                const bool stringed = at < bytes.size() && (bytes[at] == ']' || bytes[at] == '_' || bytes[at] == 'P');

                at = !stringed ? at + 1 : bel < st ? bel + 1 : st + 2;
            }
        }
    }

    // The number of digits at `at`, zero when there is none.
    static int number(const std::string& parameters, std::size_t at) {
        int made = 0;

        for (; at < parameters.size() && parameters[at] >= '0' && parameters[at] <= '9'; ++at)
            made = made * 10 + (parameters[at] - '0');

        return made;
    }

    // Carries out a CSI sequence with its parameters and final byte.
    void commanded(const std::string& parameters, char last) {
        const int first = number(parameters, 0);

        if (last == 'H') {
            const std::size_t semicolon = parameters.find(';');

            y = std::max(first, 1) - 1;
            x = semicolon == std::string::npos ? 0 : std::max(number(parameters, semicolon + 1), 1) - 1;
            before = 0;
        } else if (last == 'X' && y == 0) {
            for (int at = x; at < x + std::max(first, 1) && at < 24; ++at) cells[static_cast<std::size_t>(at)] = " ";
        } else if (last == 'J' && first == 2) {
            cells.assign(24, " ");
        }
    }

    // Draws one character at the cursor, or joins it to the cell before.
    void lettered(char32_t one) {
        const int advance = counting(before, one);

        before = one;

        if (y != 0) return;

        if (advance == 0) {
            if (x > 0) cells[static_cast<std::size_t>(x - 1)] += utf8::encode(one);

            return;
        }

        if (x < 24) cells[static_cast<std::size_t>(x)] = utf8::encode(one);

        for (int more = 1; more < advance && x + more < 24; ++more) cells[static_cast<std::size_t>(x + more)] = "";

        x += advance;
    }

    // The cells from `from` on, joined.
    [[nodiscard]] std::string from(int from) const {
        std::string made;

        for (int at = from; at < 24; ++at) made += cells[static_cast<std::size_t>(at)];

        return made;
    }
};

// Whether the bytes hold an ECH.
bool erasing(const std::string& said) {
    for (std::size_t at = said.find("\x1b["); at != std::string::npos; at = said.find("\x1b[", at + 1)) {
        std::size_t end = at + 2;

        while (end < said.size() && said[end] >= '0' && said[end] <= '9') ++end;

        if (end < said.size() && said[end] == 'X') return true;
    }

    return false;
}

}

// A word with a cluster goes out whole: erased first, then the jump to the cell after it, because
// the terminal's cursor stands where its own count put it.
TRIAL("Term: a word the terminal may count otherwise is erased, written whole and followed by a jump") {
    auto line = faking::terminal();

    line->window = ui::Size{24, 2};

    Worded root;

    root.text = "\xe0\xa4\x87\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\x95\xe0\xa4\xbe (2026)";

    const std::string word = root.text.substr(0, root.text.find(' '));

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    line->written.clear();

    term.draw();

    ought(holds(line->written, "\x1b[1;1H\x1b[4X" + word + "\x1b[1;5H (2026)"));

    // An unchanged word stays; a change after it goes by itself.
    root.text = word + " (2027)";

    line->written.clear();

    term.draw();

    ought(!holds(line->written, word));
    ought(!erasing(line->written));
    ought(holds(line->written, "7"));

    // A changed word takes the rest of the row with it.
    root.text = word.substr(0, word.size() - 3) + "\xe0\xa5\x80 (2027)";

    line->written.clear();

    term.draw();

    ought(holds(line->written, "\x1b[4X" + root.text.substr(0, root.text.find(' ')) + "\x1b[1;5H (2027)"));
}

// A cluster is cut and measured by the terminal's own rules; talcite gives a spacing mark and a
// prepended character a cell of their own, a terminal following UAX #29 joins them to their
// neighbour. Their words go out whole, a double-width character in them as one letter.
TRIAL("Term: a cluster, a spacing mark and a prepended character make their word one the terminal may count otherwise") {
    const std::pair<std::string, int> words[] = {{"\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\x95", 2},
                                                 {"\xe0\xa4\x95\xe0\xa4\xbe", 2},
                                                 {"\xd8\x80\xd9\xa1", 2},
                                                 {"\xe6\x97\xa5\xe0\xa4\x95\xe0\xa4\xbe", 4}};

    for (const auto& [word, wide] : words) {
        auto line = faking::terminal();

        line->window = ui::Size{24, 2};

        Worded root;

        root.text = "ab " + word + " cd";

        Term term{Setup{}, Seam<Console>{line}};

        term.show(root);

        line->written.clear();

        term.draw();

        ought(holds(line->written, "\x1b[1;1Hab \x1b[" + std::to_string(wide) + "X" + word + "\x1b[1;" +
                                       std::to_string(4 + wide) + "H cd"));

        // Changes on both sides of an unchanged word close to it: the word is not rewritten as a
        // gap between them.
        root.text = "ax " + word + " yd";

        line->written.clear();

        term.draw();

        ought(!holds(line->written, word));
        ought(holds(line->written, "\x1b[1;" + std::to_string(5 + wide) + "Hy"));
    }
}

// ECH erases in the current background, so the word's style is set before it.
TRIAL("Term: the erasing before a word goes in the word's own style") {
    auto line = faking::terminal();

    line->window = ui::Size{24, 2};

    Worded root;

    root.text = "\xe0\xa4\x95\xe0\xa4\xbe";
    root.style.background = Color{false, 0, 0, 200};

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    line->written.clear();

    term.draw();

    ought(holds(line->written, "m\x1b[2X" + root.text));
}

TRIAL("Term: plain words go out in one piece, without erasing and without extra jumps") {
    auto line = faking::terminal();

    line->window = ui::Size{24, 2};

    Worded root;

    root.text = "Heat (1995) abc";

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    line->written.clear();

    term.draw();

    ought(holds(line->written, "\x1b[1;1HHeat (1995) abc"));
    ought(!erasing(line->written));
}

// Whatever the terminal's count, the columns after the word stay where talcite put them, and
// nothing of the previous picture is left under a word drawn narrower.
TRIAL("Term: after a word the terminal counts otherwise, the columns stay in place") {
    const std::string word = "\xe0\xa4\x87\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\x95\xe0\xa4\xbe";
    const std::string other = "\xe0\xa4\x87\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\x95\xe0\xa5\x80";

    for (const Counting counting : {&narrower, &wider}) {
        auto line = faking::terminal();

        line->window = ui::Size{24, 2};

        Worded root;

        root.text = "Heat (1995) abc";

        Term term{Setup{}, Seam<Console>{line}};

        term.show(root);

        Modelled shown{counting};

        // Draws the text and lets the model read what went out.
        const auto drawing = [&](const std::string& text) {
            root.text = text;

            line->written.clear();

            term.draw();

            shown.fed(line->written);
        };

        drawing("Heat (1995) abc");
        drawing(word + " (2026) abc");

        same(shown.from(4), std::string{" (2026) abc         "});

        if (counting == &narrower) same(shown.cells[2] + shown.cells[3], std::string{"  "});

        drawing(word + " (2026) abd");

        same(shown.from(4), std::string{" (2026) abd         "});

        drawing(other + " (2026) abd");

        same(shown.from(4), std::string{" (2026) abd         "});

        // A word that went out again takes the next one with it, though that one is unchanged: a
        // terminal counting wider drew over it.
        const std::string twice = "\xe0\xa4\x95\xe0\xa4\xbe\xe0\xa4\x95\xe0\xa4\xbe";
        const std::string changed = "\xe0\xa4\x95\xe0\xa5\x80\xe0\xa4\x95\xe0\xa4\xbe";

        drawing(twice + " " + word + " abc");
        drawing(changed + " " + word + " abc");

        same(shown.cells[5], std::string{"\xe0\xa4\x87"});
        same(shown.from(10), std::string{"abc           "});
    }
}
