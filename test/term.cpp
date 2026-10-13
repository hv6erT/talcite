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

// `Term` talks to the terminal only through `Console`, so with a fake every byte is visible: the
// frame, dressing the terminal and undressing it, the title, and also what comes in: keys, size and
// mode.

namespace {

// A root that has a single letter in pure red in its upper left corner.
class Red : public ui::Root {
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

// A root that records what reached it, and asks for a turn when told to.
class Heard : public Red {
public:
    std::u32string keys;
    std::vector<ui::Size> sizes;
    std::optional<std::chrono::milliseconds> wanted;

    // The turn the root asks for, as set by the trial.
    [[nodiscard]] std::optional<std::chrono::milliseconds> due() const override { return wanted; }

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

TRIAL("Term: the tree's request for a turn goes to the console as patience") {
    auto line = faking::terminal();

    Heard root;

    root.wanted = std::chrono::milliseconds{30};

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    (void)term.turn();

    ought(line->patience.back() == std::chrono::milliseconds{30});
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
