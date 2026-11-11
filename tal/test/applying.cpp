#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "farewell.hpp"
#include "tal/app.hpp"
#include "tal/capabilities.hpp"
#include "tal/clock.hpp"
#include "tal/console.hpp"
#include "tal/document.hpp"
#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/key.hpp"
#include "tal/node.hpp"
#include "tal/port.hpp"
#include "tal/style.hpp"
#include "tal/surface.hpp"
#include "tal/text.hpp"

#include "tal/testing.hpp"

// The application: `App` talks to the terminal only through the console port, so on a scripted
// console every byte is visible, the frame, dressing the terminal and undressing it, the title, and
// also what comes in: keys, the size and the mode. A manual clock, shared with the document, makes
// every wait exact.

namespace {

using namespace std::chrono_literals;

using tal::App;
using tal::Area;
using tal::Arranged;
using tal::Axis;
using tal::Canvas;
using tal::Caret;
using tal::Clock;
using tal::Code;
using tal::Color;
using tal::Colors;
using tal::Console;
using tal::Document;
using tal::Effect;
using tal::Hooks;
using tal::KeyEvent;
using tal::Look;
using tal::ManualClock;
using tal::Mouse;
using tal::MouseEvent;
using tal::Node;
using tal::Port;
using tal::Press;
using tal::Property;
using tal::Screen;
using tal::ScriptedConsole;
using tal::Setup;
using tal::Size;
using tal::Spot;
using tal::Timer;

using Line = std::shared_ptr<ScriptedConsole>;

// A scripted console, with `said` as its first reply.
Line scripted(std::string said = {}) { return std::make_shared<ScriptedConsole>(std::move(said)); }

// A scripted console that takes escape sequences: links go only to such a one.
Line sequenced() {
    Line made = scripted();

    made->sequences = true;

    return made;
}

// Whether `what` occurs anywhere in `where`.
bool holds(const std::string& where, const std::string& what) { return where.find(what) != std::string::npos; }

// Whether `where` ends with `what`.
bool ends(const std::string& where, const std::string& what) {
    return where.size() >= what.size() && where.compare(where.size() - what.size(), what.size(), what) == 0;
}

// A node that has a single letter in pure red in its upper left corner.
class Red : public Node {
public:
    // Paints it again, though nothing changed.
    void again() { repaint(); }

protected:
    // Puts the red `x` at (0, 0).
    void paint(Canvas& onto) const override {
        Look how;

        how.ink = Color{255, 0, 0};

        (void)onto.put({0, 0}, "x", how);
    }
};

// The red node with the focus and its caret in the third column of the first row, as an editor
// would have it, in a shape that can be changed.
class Pointed : public Red {
public:
    Pointed() { focusable(true); }

    // Asks for the caret in `form`, blinking or not.
    void shaped(Caret::Form form, bool blink) {
        form_ = form;
        blink_ = blink;

        repaint();
    }

    // Puts the caret at `where` in the content.
    void pointed(Spot where) {
        at_ = where;

        repaint();
    }

protected:
    // Where it was put, (2, 0) at first, in the shape asked for.
    std::optional<Caret> caret() const override { return Caret{at_, form_, blink_}; }

private:
    Spot at_{2, 0};
    Caret::Form form_ = Caret::Form::usual;
    bool blink_ = true;
};

// A node that writes its text at (0, 0) in its look.
class Worded : public Node {
public:
    Property<std::string> text{*this, Effect::paint, ""};

    Property<Look> look{*this, Effect::paint, Look{}};

protected:
    // Puts the text at the start.
    void paint(Canvas& onto) const override { (void)onto.put({0, 0}, *text, *look); }
};

// A node with "go" at (1, 0) under a hyperlink and "x" after it, outside the link.
class Linking : public Node {
public:
    explicit Linking(std::string address) : target(*this, Effect::paint, std::move(address)) {}

    Property<std::string> target;

    // Paints it again, though nothing changed.
    void again() { repaint(); }

protected:
    // Puts "go x" and links "go".
    void paint(Canvas& onto) const override {
        (void)onto.put({1, 0}, "go x", Look{});

        onto.link({{1, 0}, {2, 1}}, *target);
    }
};

// A node that writes text with escape sequences in it, as it could arrive in a file name.
class Hostile : public Node {
protected:
    // Puts the text at (0, 0).
    void paint(Canvas& onto) const override {
        (void)onto.put({0, 0}, std::string{"\x1b]52;c;cHduZWQ=\x07\x1bZ\xc2\x9b"
                                           "31m"},
                       Look{});
    }
};

// A node that places each child in two cells at (3, 1).
class Corner : public Node {
protected:
    // Every child in the same small place.
    void arrange(Area inside) override {
        (void)inside;

        for (Node& one : children()) one.placed(Area{{3, 1}, {2, 1}});
    }
};

// A node that places its children one under another, a row each.
class Stack : public Node {
protected:
    // Child after child, one row high.
    void arrange(Area inside) override {
        int y = inside.at.y;

        for (Node& one : children()) one.placed(Area{{inside.at.x, y++}, {inside.size.wide, 1}});
    }
};

// A node as wide as its text measures by the document's rendering, which it notes.
class Measured : public Node {
public:
    Property<std::string> text{*this, Effect::layout, ""};

    std::shared_ptr<tal::text::Rendering> seen = std::make_shared<tal::text::Rendering>();

    // The rendering it measures with now.
    [[nodiscard]] tal::text::Rendering now() const { return rendering(); }

protected:
    // The text's width by the rendering, one row.
    Size measure(tal::Limits room) const override {
        *seen = rendering();

        return Size{std::max(room.least.wide, tal::text::width(*text, rendering())), std::max(room.least.high, 1)};
    }
};

// A console, a manual clock and a document on that clock, for one application.
struct Rig {
    Line line = scripted();
    std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>();
    Document document{Port<Clock>{clock}};
    std::u32string keys;
    std::vector<Size> sizes;
    Hooks hooks;

    // The console as the application takes it.
    [[nodiscard]] Port<Console> console() const { return Port<Console>{line}; }

    // The clock as the application takes it.
    [[nodiscard]] Port<Clock> time() const { return Port<Clock>{clock}; }

    // Records the keys that reach the document, Esc as ⎋, and the document's size whenever it
    // is arranged.
    void listened() {
        hooks.add(document.on<KeyEvent>([this](KeyEvent& what) {
            keys += what.key.code() == Code::escape ? U'⎋' : what.key.letter();

            what.take();
        }));

        hooks.add(document.on<Arranged>([this](Arranged&) { sizes.push_back(document.area().size); }));
    }
};

// What went to the terminal for the first frame of a red `x`, after the probe.
std::string drawn(const Line& line, Setup wish = {}) {
    Rig rig;

    rig.line = line;

    (void)rig.document.children().add<Red>();

    App app{std::move(wish), rig.console(), rig.time()};

    app.show(rig.document);

    line->written.clear();

    (void)app.turn();

    return line->written;
}

// A setup that asks for the alternate screen, SGR mouse reports and bracketed paste.
Setup dressy() {
    Setup made;

    made.screen = Screen::apart;
    made.mouse = Mouse::sgr;
    made.bracketed_paste = true;

    return made;
}

// --- Colors --------------------------------------------------------------------------------------

TRIAL("App: the 256-color palette comes out as 38;5") { OUGHT(holds(drawn(scripted()), "\x1b[0;38;5;196mx")); }

TRIAL("App: sixteen colors come out as bright red") {
    Line line = scripted();

    line->environment["TERM"] = "vt220";

    OUGHT(holds(drawn(line), "\x1b[0;91mx"));
}

// The last of the eight plain colors and the first bright one, as ink and as paper.
TRIAL("App: sixteen colors split into plain and bright at the eighth") {
    Rig rig;

    rig.line->environment["TERM"] = "vt220";

    Worded& worded = rig.document.children().add<Worded>();
    Look look;

    look.ink = Color{192, 192, 192};
    look.paper = Color{128, 128, 128};
    worded.text = "x";
    worded.look = look;

    App app{Setup{}, rig.console(), rig.time()};

    app.show(rig.document);

    rig.line->written.clear();

    (void)app.turn();

    OUGHT(holds(rig.line->written, "\x1b[0;37;100mx"));
}

TRIAL("App: NO_COLOR leaves the letter without color") {
    Line line = scripted();

    line->environment["NO_COLOR"] = "1";

    const std::string said = drawn(line);

    OUGHT(holds(said, "\x1b[0mx"));
    OUGHT(!holds(said, "38;"));
}

// A wish beats a measurement: `Setup` says what to draw with, whatever the console claims.
TRIAL("App: Setup overrides the measured colors, and the abilities say so") {
    Setup wish;

    wish.colors = Colors::rgb;

    OUGHT(holds(drawn(scripted(), wish), "\x1b[0;38;2;255;0;0mx"));

    Rig rig;
    const App app{wish, rig.console(), rig.time()};

    OUGHT(app.abilities().colors == Colors::rgb);
}

TRIAL("App: the probe goes through the same console") {
    Rig rig;
    const App app{Setup{}, rig.console(), rig.time()};

    OUGHT(rig.line->written.rfind("\x1b[>c", 0) == 0);
}

// --- Dressing the terminal -----------------------------------------------------------------------

// The alternate screen goes first when dressing and last when undressing; otherwise the mouse and
// paste sequences would land on a different buffer from the one where they stay.
TRIAL("App: dressing and undressing go in opposite order") {
    Rig rig;
    std::string on;

    {
        const App app{dressy(), rig.console(), rig.time()};

        on = rig.line->written;

        rig.line->written.clear();
    }

    const std::string& off = rig.line->written;

    OUGHT(on.find("\x1b[?1049h") < on.find("\x1b[?1000h\x1b[?1002h\x1b[?1006h"));
    OUGHT(on.find("\x1b[?1006h") < on.find("\x1b[?2004h"));
    OUGHT(off.find("\x1b[?2004l") < off.find("\x1b[?1006l\x1b[?1002l\x1b[?1000l"));
    OUGHT(off.find("\x1b[?1000l") < off.find("\x1b[?1049l"));
    OUGHT(holds(off, "\x1b[?25h"));
}

// A terminal without SGR gets drags and the urxvt encoding too, and loses them in reverse order;
// hovering adds ?1003 on top and takes it off first.
TRIAL("App: the mouse without SGR asks for drags and the decimal encoding, hovering on top") {
    Rig rig;
    std::string on;

    {
        Setup wish;

        wish.mouse = Mouse::basic;
        wish.hover = true;

        const App app{wish, rig.console(), rig.time()};

        on = rig.line->written;

        rig.line->written.clear();
    }

    const std::string& off = rig.line->written;

    OUGHT(holds(on, "\x1b[?1000h\x1b[?1002h\x1b[?1015h\x1b[?1003h"));
    OUGHT(!holds(on, "\x1b[?1006h"));
    OUGHT(holds(off, "\x1b[?1003l\x1b[?1015l\x1b[?1002l\x1b[?1000l"));
}

// Hovering adds to mouse reports that are on; without them nobody would send that motion.
TRIAL("App: hovering without the mouse asks for nothing") {
    Rig rig;
    Setup wish;

    wish.mouse = Mouse::none;
    wish.hover = true;

    {
        const App app{wish, rig.console(), rig.time()};
    }

    OUGHT(!holds(rig.line->written, "\x1b[?1003"));
}

// The wish for block glyphs is bounded by what the terminal does not break up (here an octant
// takes two cells, a sextant one); the rendering of emoji follows the probe's widths, two columns
// meaning a joined sequence and a wide heart.
TRIAL("App: the blocks stay under the measured ceiling, the rendering follows the widths") {
    Rig rig;
    Setup wish;

    rig.line = scripted("\x1b[1;3R\x1b[1;3R\x1b[1;3R\x1b[1;2R\x1b[1;3R\x1b[?62;1c");
    wish.blocks = tal::Blocks::octants;

    const App app{wish, rig.console(), rig.time()};

    OUGHT(app.abilities().blocks == tal::Blocks::sextants);
    OUGHT(app.abilities().rendering == (tal::text::Rendering{true, true}));
}

TRIAL("App: without wishes the mouse and the screen follow the measurement") {
    Rig rig;

    rig.line->environment["TERM"] = "dumb";

    const App app{Setup{}, rig.console(), rig.time()};

    OUGHT(!holds(rig.line->written, "\x1b[?1000h"));
    OUGHT(!holds(rig.line->written, "\x1b[?47h"));
    OUGHT(!holds(rig.line->written, "\x1b[?1049h"));
}

// Stepping aside takes everything off, coming back puts it on again, and a nested step aside does
// not send a second set.
TRIAL("App: Pause takes off once and puts on once") {
    Rig rig;
    App app{dressy(), rig.console(), rig.time()};

    rig.line->written.clear();

    std::string away;

    {
        const App::Pause one{app};
        const App::Pause two{app};

        away = rig.line->written;

        rig.line->written.clear();
    }

    const std::string& back = rig.line->written;

    OUGHT(holds(away, "\x1b[?1049l"));
    OUGHT(away.find("\x1b[?1049l") == away.rfind("\x1b[?1049l"));
    OUGHT(holds(back, "\x1b[?1049h"));
    OUGHT(back.find("\x1b[?1049h") == back.rfind("\x1b[?1049h"));
}

// The cursor shows where the caret is and hides, once, when there is none. The dressing hides it,
// so after stepping aside the next frame shows it again; an editor would otherwise go on without
// one. The frame is drawn whole again, too.
TRIAL("App: the cursor follows the caret and shows again after stepping aside") {
    Rig rig;
    Pointed& pointed = rig.document.children().add<Pointed>();
    App app{Setup{}, rig.console(), rig.time()};

    app.show(rig.document);

    (void)app.turn();

    OUGHT(!holds(rig.line->written, "\x1b[?25h"));

    pointed.focus();

    (void)app.turn();

    OUGHT(holds(rig.line->written, "\x1b[1;3H\x1b[?25h"));

    pointed.pointed(Spot{100, 0});
    rig.line->written.clear();

    (void)app.turn();

    SAME(rig.line->written, std::string{"\x1b[?25l"});

    pointed.again();
    rig.line->written.clear();

    (void)app.turn();

    SAME(rig.line->written, std::string{});

    pointed.pointed(Spot{2, 0});

    {
        const App::Pause away{app};
    }

    rig.line->written.clear();

    (void)app.turn();

    OUGHT(holds(rig.line->written, "\x1b[2J"));
    OUGHT(holds(rig.line->written, "x"));
    OUGHT(holds(rig.line->written, "\x1b[?25h"));
}

// The cursor's shape goes to the terminal once, when it changes, before the cursor shows; a node
// that asks for none sends nothing about it. At the end the terminal's usual cursor comes back, and
// after stepping aside the shape goes again, since another program may have changed it.
TRIAL("App: the cursor's shape goes once, comes again after stepping aside, and goes back at the end") {
    {
        Rig rig;
        Pointed& pointed = rig.document.children().add<Pointed>();
        App app{Setup{}, rig.console(), rig.time()};

        app.show(rig.document);

        (void)app.turn();

        pointed.focus();

        (void)app.turn();

        OUGHT(holds(rig.line->written, "\x1b[?25h"));
        OUGHT(!holds(rig.line->written, " q"));
    }

    Rig rig;
    Pointed& pointed = rig.document.children().add<Pointed>();

    pointed.shaped(Caret::Form::underline, true);

    {
        App app{Setup{}, rig.console(), rig.time()};

        app.show(rig.document);

        (void)app.turn();

        pointed.focus();

        (void)app.turn();

        OUGHT(holds(rig.line->written, "\x1b[3 q"));
        OUGHT(rig.line->written.find("\x1b[3 q") < rig.line->written.rfind("\x1b[?25h"));

        rig.line->written.clear();

        pointed.shaped(Caret::Form::bar, false);

        (void)app.turn();

        OUGHT(holds(rig.line->written, "\x1b[6 q"));

        rig.line->written.clear();

        pointed.again();

        (void)app.turn();

        OUGHT(!holds(rig.line->written, " q"));

        {
            const App::Pause away{app};
        }

        OUGHT(holds(rig.line->written, "\x1b[0 q"));

        rig.line->written.clear();

        (void)app.turn();

        OUGHT(holds(rig.line->written, "\x1b[6 q"));

        rig.line->written.clear();
    }

    OUGHT(holds(rig.line->written, "\x1b[0 q"));
}

// The caret stands on the screen inside the content of the node with the focus; one outside that
// content is no caret, and without a node with the focus there is none either.
TRIAL("App: the document's caret stands on the screen, inside the content of its node") {
    Rig rig;

    rig.document.display(Size{10, 3}, Colors::rgb, true);

    Corner& corner = rig.document.children().add<Corner>();
    Pointed& pointed = corner.children().add<Pointed>();

    (void)rig.document.frame();

    OUGHT(!rig.document.caret().has_value());

    pointed.focus();

    (void)rig.document.frame();

    OUGHT(!rig.document.caret().has_value());

    pointed.pointed(Spot{1, 0});
    pointed.shaped(Caret::Form::block, false);

    (void)rig.document.frame();

    OUGHT(rig.document.caret() == (Caret{{4, 1}, Caret::Form::block, false}));

    pointed.pointed(Spot{0, 1});

    (void)rig.document.frame();

    OUGHT(!rig.document.caret().has_value());
}

// After a signal there is no time to undress the terminal the way `App` does; the console writes
// one fixed farewell instead. It must do everything `App` does when it undresses (the alternate
// screens, the mouse in either encoding with hovering, paste, the wrap, the cursor and its shape),
// and also end a synchronized frame the signal may have cut in half.
TRIAL("App: the farewell after a signal undoes everything App turns on") {
    const std::string farewell{tal::detail::farewell};

    // Each private mode `\x1b[?N` with its letter, and each cursor shape `\x1b[N q`, in the text.
    const auto sequences = [](const std::string& text) {
        std::vector<std::string> made;

        for (std::size_t at = text.find("\x1b["); at != std::string::npos; at = text.find("\x1b[", at + 1)) {
            std::size_t end = at + 2;
            const bool asked = end < text.size() && text[end] == '?';

            if (asked) ++end;

            while (end < text.size() && text[end] >= '0' && text[end] <= '9') ++end;

            if (asked && end < text.size() && (text[end] == 'h' || text[end] == 'l'))
                made.push_back(text.substr(at, end + 1 - at));
            else if (!asked && end + 1 < text.size() && text[end] == ' ' && text[end + 1] == 'q')
                made.push_back(text.substr(at, end + 2 - at));
        }

        return made;
    };

    Setup older;

    older.screen = Screen::older;
    older.mouse = Mouse::basic;
    older.hover = true;
    older.bracketed_paste = true;
    older.synchronized = true;

    Setup apart = dressy();

    apart.hover = true;
    apart.synchronized = true;

    for (const Setup& given : {older, apart}) {
        Rig rig;
        Pointed& pointed = rig.document.children().add<Pointed>();

        pointed.shaped(Caret::Form::underline, true);

        std::string on;

        {
            App app{given, rig.console(), rig.time()};

            app.show(rig.document);

            (void)app.turn();

            pointed.focus();

            (void)app.turn();

            on = rig.line->written;

            rig.line->written.clear();
        }

        const std::vector<std::string> off = sequences(rig.line->written);

        OUGHT(off.size() >= 9);

        for (const std::string& one : off) OUGHT(holds(farewell, one));

        OUGHT(holds(on, "\x1b[?2026h"));
        OUGHT(holds(farewell, "\x1b[?2026l"));
    }
}

// While stepped aside the terminal belongs to another program: nothing is drawn and the console is
// not heard, though a key waits there; the loop alone turns.
TRIAL("App: while stepped aside it neither draws nor hears, but the loop turns") {
    Rig rig;

    rig.listened();

    Red& red = rig.document.children().add<Red>();
    App app{Setup{}, rig.console(), rig.time()};

    app.show(rig.document);

    const App::Pause away{app};

    rig.line->replies.push_back("a");
    rig.line->written.clear();

    red.again();

    const int before = rig.line->rounds;
    int ran = 0;

    app.loop().post([&ran] { ++ran; });

    (void)app.turn();

    SAME(rig.line->written, std::string{});
    SAME(rig.line->rounds, before);
    SAME(ran, 1);
    OUGHT(rig.keys.empty());
}

// --- Frame ---------------------------------------------------------------------------------------

// A node painted again the same sends not a single byte: its rows are compared with the screen.
TRIAL("App: an unchanged repaint sends not a single byte, an idle turn draws nothing") {
    Rig rig;
    Red& red = rig.document.children().add<Red>();
    App app{Setup{}, rig.console(), rig.time()};

    app.show(rig.document);

    (void)app.turn();

    rig.line->written.clear();

    (void)app.turn();

    SAME(rig.line->written, std::string{});

    red.again();

    (void)app.turn();

    SAME(rig.line->written, std::string{});
}

TRIAL("App: the frame is synchronized when allowed") {
    Setup wish;

    wish.synchronized = true;

    const std::string said = drawn(scripted(), wish);

    OUGHT(said.rfind("\x1b[?2026h", 0) == 0);
    OUGHT(ends(said, "\x1b[?2026l"));
}

TRIAL("App: without a document a turn throws") {
    Rig rig;
    App app{Setup{}, rig.console(), rig.time()};
    bool thrown = false;

    OUGHT(app.shown() == nullptr);

    try {
        (void)app.turn();
    } catch (const std::logic_error&) {
        thrown = true;
    }

    OUGHT(thrown);
}

// --- Title ---------------------------------------------------------------------------------------

TRIAL("App: the title goes out as OSC 0") {
    Rig rig;
    App app{Setup{}, rig.console(), rig.time()};

    rig.line->written.clear();

    app.title("abc");

    SAME(rig.line->written, std::string{"\x1b]0;abc\x1b\\"});
}

TRIAL("App: a title forbidden in Setup does not go out") {
    Rig rig;
    Setup wish;

    wish.titles = false;

    App app{wish, rig.console(), rig.time()};

    rig.line->written.clear();

    app.title("abc");

    SAME(rig.line->written, std::string{});
}

// A title from a file name must not end the OSC early: an escape, a BEL, DEL and C1 (as UTF-8)
// stay out, everything else goes as it came.
TRIAL("App: the title leaves its control characters out") {
    Rig rig;
    App app{Setup{}, rig.console(), rig.time()};

    rig.line->written.clear();

    app.title("a\x1b\\b\x07"
              "c\xc2\x9b"
              "d\x7f\xc3\xa9\xc2\xa0\xc2");

    SAME(rig.line->written, std::string{"\x1b]0;a\\bcd\xc3\xa9\xc2\xa0\xc2\x1b\\"});
}

// The change goes through the loop, so only a turn carries it out, and it sends just the
// difference.
TRIAL("App: reconfigure sends just the difference") {
    Rig rig;
    App app{dressy(), rig.console(), rig.time()};
    Setup wish = dressy();

    wish.bracketed_paste = false;

    app.reconfigure(wish);

    rig.line->written.clear();

    (void)app.loop().turn();

    SAME(rig.line->written, std::string{"\x1b[?2004l"});
    OUGHT(!app.abilities().bracketed_paste);
}

// A new screen or mouse goes out whole: off the old one, on the new one, and the next frame is
// drawn from a cleared screen.
TRIAL("App: reconfiguring the screen switches it and draws the frame whole") {
    Rig rig;

    (void)rig.document.children().add<Red>();

    App app{dressy(), rig.console(), rig.time()};

    app.show(rig.document);

    (void)app.turn();

    Setup wish = dressy();

    wish.screen = Screen::older;

    app.reconfigure(wish);

    rig.line->written.clear();

    (void)app.turn();

    const std::string& said = rig.line->written;

    OUGHT(said.find("\x1b[?1049l") < said.find("\x1b[?47h"));
    OUGHT(said.find("\x1b[?47h") < said.rfind("\x1b[2J"));
    OUGHT(holds(said, "x"));
}

// --- Raw mode, input and size --------------------------------------------------------------------

TRIAL("App: raw mode from construction to the end, the probe nested inside it") {
    Rig rig;

    {
        const App app{Setup{}, rig.console(), rig.time()};

        SAME(rig.line->depth, 1);
        SAME(rig.line->entered, 2);
    }

    SAME(rig.line->depth, 0);
}

TRIAL("App: Pause takes off raw mode, coming back puts it on and measures the window again") {
    Rig rig;

    rig.line->window = Size{100, 30};

    App app{Setup{}, rig.console(), rig.time()};

    {
        const App::Pause one{app};
        const App::Pause two{app};

        SAME(rig.line->depth, 0);

        rig.line->window = Size{70, 20};
    }

    SAME(rig.line->depth, 1);
    OUGHT(app.abilities().size == (Size{70, 20}));
}

TRIAL("App: a key from the console reaches the document") {
    Rig rig;

    rig.listened();

    App app{Setup{}, rig.console(), rig.time()};

    app.show(rig.document);

    rig.line->replies.push_back("ab");

    (void)app.turn();

    OUGHT(rig.keys == U"ab");
}

// Bytes that arrived during the probe and were not an answer are keys pressed at startup. They go
// first, before anything fresh.
TRIAL("App: keys from the probe go before fresh ones") {
    Rig rig;

    rig.line = scripted("q\x1b[?62;1c");
    rig.listened();

    App app{Setup{}, rig.console(), rig.time()};

    app.show(rig.document);

    rig.line->replies.push_back("a");

    (void)app.turn();

    OUGHT(rig.keys == U"qa");
}

// The decoder's deadline is measured by the loop's clock: on a manual one the console is told the
// whole fifty milliseconds until the clock moves, and the Esc settles in the turn that finds it
// due.
TRIAL("App: a lone Esc waits fifty milliseconds and resolves by itself") {
    Rig rig;

    rig.listened();

    App app{Setup{}, rig.console(), rig.time()};

    app.show(rig.document);

    rig.line->replies.push_back("\x1b");

    (void)app.turn();
    (void)app.turn();

    OUGHT(rig.keys.empty());
    OUGHT(rig.line->patience.back() == std::optional{50ms});

    rig.clock->advance(50ms);

    (void)app.turn();

    OUGHT(rig.line->patience.back() == std::optional{0ms});
    OUGHT(rig.keys == U"⎋");
}

// With nothing to settle the console waits without a limit; work posted to the loop, from this
// thread or another, a timer set on it, or a stop interrupts that wait, so the application turns at
// once.
TRIAL("App: with no reason it waits without a limit, and the loop interrupts the wait") {
    Rig rig;
    App app{Setup{}, rig.console(), rig.time()};

    app.show(rig.document);

    (void)app.turn();

    OUGHT(!rig.line->patience.back().has_value());

    const int before = rig.line->interrupted;
    int ran = 0;

    app.loop().post([&ran] { ++ran; });

    std::thread other{[&app, &ran] { app.loop().post([&ran] { ++ran; }); }};

    other.join();

    (void)app.loop().wake(rig.clock->now() + 1h, [] {});

    SAME(rig.line->interrupted, before + 3);

    (void)app.turn();

    SAME(ran, 2);

    app.stop(7);

    SAME(rig.line->interrupted, before + 4);
    OUGHT(!app.turn());

    app.loop().stop();

    SAME(rig.line->interrupted, before + 5);
}

// The nearest of the loop's timers and the document's timers bounds the wait for the console. A
// timer withdrawn from the loop no longer counts.
TRIAL("App: the nearest timer of the loop or the document goes to the console as patience") {
    Rig rig;
    App app{Setup{}, rig.console(), rig.time()};

    app.show(rig.document);

    const Timer later = rig.document.after(30ms, [](Timer&) {});

    (void)app.turn();

    OUGHT(rig.line->patience.back() == std::optional{30ms});

    const std::uint64_t sooner = app.loop().wake(rig.clock->now() + 20ms, [] {});

    (void)app.turn();

    OUGHT(rig.line->patience.back() == std::optional{20ms});

    app.loop().unwake(sooner);

    (void)app.turn();

    OUGHT(rig.line->patience.back() == std::optional{30ms});
}

// What the document's timers have due rings in the turn after the wait, once.
TRIAL("App: a timer due rings in the turn") {
    Rig rig;
    App app{Setup{}, rig.console(), rig.time()};

    app.show(rig.document);

    (void)app.turn();

    int called = 0;

    const Timer now = rig.document.after(0ms, [&called](Timer&) { ++called; });

    (void)app.turn();
    (void)app.turn();

    SAME(called, 1);
    OUGHT(!now.waiting());
}

TRIAL("App: the size from the console at startup, and 80x25 without it") {
    Rig sized;
    Rig blind;

    sized.line->window = Size{100, 30};

    const App one{Setup{}, sized.console(), sized.time()};
    const App two{Setup{}, blind.console(), blind.time()};

    OUGHT(one.abilities().size == (Size{100, 30}));
    OUGHT(two.abilities().size == (Size{80, 25}));
}

// A new window size lays the document out again in it, and the frame goes whole on a cleared
// screen; the turn after it neither lays out nor draws.
TRIAL("App: a size change lays the document out again and draws everything") {
    Rig rig;

    rig.line->window = Size{40, 10};
    rig.listened();

    (void)rig.document.children().add<Red>();

    App app{Setup{}, rig.console(), rig.time()};

    app.show(rig.document);

    (void)app.turn();

    OUGHT(rig.sizes == (std::vector<Size>{{40, 10}}));

    rig.line->window = Size{50, 12};
    rig.line->written.clear();

    (void)app.turn();

    OUGHT(rig.sizes == (std::vector<Size>{{40, 10}, {50, 12}}));
    OUGHT(holds(rig.line->written, "\x1b[2J"));
    OUGHT(holds(rig.line->written, "x"));

    rig.line->written.clear();

    (void)app.turn();

    SAME(rig.sizes.size(), std::size_t{2});
    SAME(rig.line->written, std::string{});
}

// Resumed after being stopped: the shell had the terminal meanwhile, took our dressing off it and
// wrote over the screen. The dressing comes back and the frame is painted whole, once.
TRIAL("App: resuming after a stop dresses the terminal and paints everything") {
    Rig rig;

    (void)rig.document.children().add<Red>();

    App app{dressy(), rig.console(), rig.time()};

    app.show(rig.document);

    (void)app.turn();

    rig.line->written.clear();

    (void)app.turn();

    SAME(rig.line->written, std::string{});

    rig.line->comeback = true;

    (void)app.turn();

    OUGHT(holds(rig.line->written, "\x1b[?1049h"));
    OUGHT(holds(rig.line->written, "\x1b[2J"));
    OUGHT(holds(rig.line->written, "x"));

    rig.line->written.clear();

    (void)app.turn();

    SAME(rig.line->written, std::string{});
}

// The wheel turned sideways arrives as a wheel along the horizontal axis, where the pointer is.
TRIAL("App: a sideways wheel reaches the document along its axis") {
    Rig rig;
    struct Wheeled {
        Axis axis;
        int steps;
        Spot screen;
    };

    std::vector<Wheeled> heard;

    rig.hooks.add(rig.document.on<MouseEvent>([&heard](MouseEvent& what) { heard.push_back({what.axis, what.steps, what.screen}); }));

    App app{dressy(), rig.console(), rig.time()};

    app.show(rig.document);

    rig.line->replies.push_back("\x1b[<66;3;2M\x1b[<65;3;2M");

    (void)app.turn();

    SAME(heard.size(), std::size_t{2});
    OUGHT(heard.front().axis == Axis::horizontal);
    SAME(heard.front().steps, 1);
    OUGHT(heard.front().screen == (Spot{2, 1}));
    OUGHT(heard.back().axis == Axis::vertical);
    SAME(heard.back().steps, -1);
}

// The rendering the probe measured reaches the document, so its surface counts an emoji sequence
// as the terminal draws it: a terminal that draws the programmer as a woman and a laptop takes four
// cells for it, and one that draws the heart with the emoji selector narrow presents nothing.
TRIAL("App: the measured rendering reaches the document's surface") {
    Rig rig;

    rig.line = scripted("\x1b[1;3R\x1b[1;2R\x1b[1;6R\x1b[?62;1c");

    Worded& worded = rig.document.children().add<Worded>();

    worded.text = "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb|";

    App app{Setup{}, rig.console(), rig.time()};

    app.show(rig.document);

    (void)app.turn();

    OUGHT(app.abilities().rendering == (tal::text::Rendering{false, false}));
    SAME(rig.document.frame().at({4, 0}).cluster, std::string{"|"});
}

// A document that shows the other one on `x`, the way twa switches screens; the rest of a chunk
// read, typed ahead, belongs to the new one.
TRIAL("App: keys after switching the document in the middle of a chunk go to the new one") {
    Rig first;
    Rig second;

    first.listened();
    second.listened();

    App app{Setup{}, first.console(), first.time()};

    first.hooks.add(first.document.on<KeyEvent>(
        [&app, &second](KeyEvent& what) {
            if (what.key.letter() == U'x') app.show(second.document);
        },
        tal::Phase::capture));

    app.show(first.document);

    first.line->replies.push_back("axyz");

    (void)app.turn();

    OUGHT(first.keys == U"ax");
    OUGHT(second.keys == U"yz");
    OUGHT(app.shown() == &second.document);
}

// An escape sequence in text does not reach the terminal: not a write to the clipboard (OSC 52),
// not a request to identify itself (DECID, which the terminal answers on input), not a
// single-character CSI from C1. Only our own sequences leave for the terminal.
TRIAL("App: text with an escape sequence does not command the terminal") {
    Rig rig;

    (void)rig.document.children().add<Hostile>();

    App app{Setup{}, rig.console(), rig.time()};

    app.show(rig.document);

    rig.line->written.clear();

    (void)app.turn();

    const std::string& said = rig.line->written;

    OUGHT(!holds(said, "\x1b]52"));
    OUGHT(!holds(said, "\x1bZ"));
    OUGHT(!holds(said, "\xc2\x9b"));
    OUGHT(!holds(said, "\x07"));
    OUGHT(holds(said, "\xe2\x90\x9b]52;c;"));
}

// A frame someone else asked of the document took its damage away: the next frame the application
// draws is then compared whole, and that change still reaches the terminal. Otherwise only the rows
// the document painted are compared and written.
TRIAL("App: a frame nobody drew does not lose its rows") {
    Rig rig;
    Stack& stack = rig.document.children().add<Stack>();
    Worded& top = stack.children().add<Worded>();
    Worded& low = stack.children().add<Worded>();

    top.text = "aaa";
    low.text = "bbb";

    App app{Setup{}, rig.console(), rig.time()};

    app.show(rig.document);

    (void)app.turn();

    low.text = "ccc";

    rig.line->written.clear();

    (void)app.turn();

    OUGHT(holds(rig.line->written, "ccc"));
    OUGHT(!holds(rig.line->written, "aaa"));

    low.text = "ddd";

    (void)rig.document.frame();

    top.text = "eee";

    rig.line->written.clear();

    (void)app.turn();

    OUGHT(holds(rig.line->written, "eee"));
    OUGHT(holds(rig.line->written, "ddd"));

    low.text = "fff";

    rig.line->written.clear();

    (void)app.turn();

    OUGHT(holds(rig.line->written, "fff"));
    OUGHT(!holds(rig.line->written, "eee"));
}

// The console's double-click time goes to the document shown: two clicks 600 ms apart make a
// double click where the console allows 700 ms, though the document's own default is 400 ms.
TRIAL("App: the double-click time comes from the console") {
    Rig rig;
    std::vector<Press> heard;

    rig.line->twice = 700ms;
    rig.hooks.add(rig.document.on<MouseEvent>([&heard](MouseEvent& what) { heard.push_back(what.press); }));

    App app{dressy(), rig.console(), rig.time()};

    app.show(rig.document);

    rig.line->replies.push_back("\x1b[<0;1;1M\x1b[<0;1;1m");

    (void)app.turn();

    rig.clock->advance(600ms);
    rig.line->replies.push_back("\x1b[<0;1;1M\x1b[<0;1;1m");

    (void)app.turn();

    OUGHT(heard == (std::vector<Press>{Press::down, Press::up, Press::click, Press::down, Press::up, Press::double_click}));
}

// The double-click time is the longest wait that still makes a double click: the wait itself
// counts, a longer one does not, and a negative time counts as none, so only clicks at one instant
// pair up.
TRIAL("App: the document's double-click time bounds the wait between clicks") {
    Rig rig;
    std::vector<Press> heard;

    rig.document.display(Size{4, 1}, Colors::rgb, true);
    rig.hooks.add(rig.document.on<MouseEvent>([&heard](MouseEvent& what) {
        if (what.press == Press::click || what.press == Press::double_click) heard.push_back(what.press);
    }));

    (void)rig.document.frame();

    const auto clicked = [&rig] {
        rig.document.mouse(Press::down, tal::MouseButton::left, Spot{0, 0});
        rig.document.mouse(Press::up, tal::MouseButton::left, Spot{0, 0});
    };

    rig.document.double_click(100ms);

    clicked();
    rig.clock->advance(100ms);
    clicked();
    rig.clock->advance(101ms);
    clicked();
    rig.clock->advance(101ms);
    clicked();

    rig.document.double_click(-5ms);

    rig.clock->advance(1s);
    clicked();
    clicked();

    OUGHT(heard == (std::vector<Press>{Press::click, Press::double_click, Press::click, Press::click, Press::click,
                                       Press::double_click}));
}

// A node measures with the rendering of its document, the one the probe measured, and is laid out
// again when it changes; outside a document it measures as tal draws.
TRIAL("App: nodes measure with the document's rendering") {
    Rig rig;

    rig.line = scripted("\x1b[1;3R\x1b[1;2R\x1b[1;6R\x1b[?62;1c");

    Measured loose;

    OUGHT(loose.now() == tal::text::Rendering{});

    Measured& measured = rig.document.children().add<Measured>();

    measured.text = "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb";

    App app{Setup{}, rig.console(), rig.time()};

    app.show(rig.document);

    (void)app.turn();

    OUGHT(*measured.seen == (tal::text::Rendering{false, false}));
    SAME(measured.measured(tal::Limits{{0, 0}, {80, 25}}).wide, 4);

    rig.document.display(Size{80, 25}, Colors::palette, true, tal::text::Rendering{});

    OUGHT(rig.document.due());

    (void)rig.document.frame();

    OUGHT(*measured.seen == tal::text::Rendering{});
    SAME(measured.measured(tal::Limits{{0, 0}, {80, 25}}).wide, 2);
}

// --- Hyperlinks ----------------------------------------------------------------------------------

// What `App` writes for the first frame of the node, after the probe.
std::string linked(const Line& line, const std::string& address, Setup wish = {}) {
    Rig rig;

    rig.line = line;

    (void)rig.document.children().add<Linking>(address);

    App app{std::move(wish), rig.console(), rig.time()};

    app.show(rig.document);

    line->written.clear();

    (void)app.turn();

    return line->written;
}

// The link opens right before its first cell, with its number as the id, and ends right after its
// last one, before the plain text; an address goes out in printable ASCII, every other byte
// escaped.
TRIAL("App: a hyperlink goes out as OSC 8 around its cells, its address escaped") {
    const std::string said = linked(sequenced(), "https://example.org/a b/\xc3\xbc");
    const std::string closing = "\x1b]8;;\x1b\\";
    const std::size_t opened = said.find("\x1b]8;id=");
    const std::size_t address = said.find(";https://example.org/a%20b/%C3%BC\x1b\\go", opened);
    const std::size_t word = said.find("go", address);
    const std::size_t closed = said.find(closing);
    const std::size_t after = said.find('x', word);

    OUGHT(opened != std::string::npos);
    OUGHT(address != std::string::npos);
    OUGHT(said.find_first_not_of("0123456789", opened + 7) == address);
    OUGHT(address < closed);
    OUGHT(closed < after);
    OUGHT(said.find("\x1b]8", closed + closing.size()) == std::string::npos);
}

// A terminal that is not given links gets the cells as plain text: forbidden in `Setup`, on the
// Linux console, and for an address longer than terminals keep.
TRIAL("App: without hyperlinks the cells go out as plain text") {
    Setup forbidding;

    forbidding.links = false;

    const std::string forbidden = linked(sequenced(), "https://example.org/", forbidding);

    OUGHT(!holds(forbidden, "\x1b]8"));
    OUGHT(holds(forbidden, "go"));

#ifndef _WIN32
    Line kernel = sequenced();

    kernel->environment["TERM"] = "linux";

    OUGHT(!holds(linked(kernel, "https://example.org/"), "\x1b]8"));
#endif

    const std::string far = linked(sequenced(), "https://example.org/" + std::string(2064, 'a'));
    const std::string near = linked(sequenced(), "https://example.org/" + std::string(2063, 'a'));

    OUGHT(!holds(far, "\x1b]8"));
    OUGHT(holds(far, "go"));
    OUGHT(holds(near, "\x1b]8;id="));
}

// A link is part of the cell for comparing frames: the same frame again sends nothing, and a new
// address rewrites the link's cells under it, though the surface gave it the number the old one
// had.
TRIAL("App: the same link sends nothing again, a new address rewrites its cells") {
    Rig rig;

    rig.line = sequenced();

    Linking& linking = rig.document.children().add<Linking>("https://example.org/one");
    App app{Setup{}, rig.console(), rig.time()};

    app.show(rig.document);

    (void)app.turn();

    rig.line->written.clear();

    linking.again();

    (void)app.turn();

    SAME(rig.line->written, std::string{});

    linking.target = "https://example.org/two";

    (void)app.turn();

    OUGHT(holds(rig.line->written, ";https://example.org/two\x1b\\go"));
    OUGHT(ends(rig.line->written, "go\x1b]8;;\x1b\\"));
}

// --- Words the terminal counts otherwise ---------------------------------------------------------

// How a modelled terminal moves its cursor after a character: by so many cells, where zero joins
// the character to the cell before.
using Counting = int (*)(char32_t before, char32_t one);

// A terminal following UAX #29 with conjuncts: a spacing mark, a virama and a consonant after a
// virama join the cell before, so it draws "इक्का" in two cells where tal counts four.
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
                const std::size_t was = at;
                const std::optional<char32_t> one = tal::text::decoded(bytes, at);

                if (!one) {
                    at = was + 1;

                    continue;
                }

                lettered(*one);

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

        std::string spelled;

        tal::text::encoded(spelled, one);

        if (advance == 0) {
            if (x > 0) cells[static_cast<std::size_t>(x - 1)] += spelled;

            return;
        }

        if (x < 24) cells[static_cast<std::size_t>(x)] = spelled;

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

// An application on a 24 by 2 window with one worded node, drawn by turns.
struct Writing {
    Rig rig;
    Worded* worded = nullptr;
    std::unique_ptr<App> app;

    // The node with `text`, shown, its first frame not yet drawn.
    explicit Writing(const std::string& text) {
        rig.line->window = Size{24, 2};

        worded = &rig.document.children().add<Worded>();
        worded->text = text;

        app = std::make_unique<App>(Setup{}, rig.console(), rig.time());
        app->show(rig.document);
    }

    // Draws the text and returns what went to the terminal for it.
    std::string drawn(const std::string& text) {
        worded->text = text;

        rig.line->written.clear();

        (void)app->turn();

        return rig.line->written;
    }
};

// A word with a cluster goes out whole: erased first, then the jump to the cell after it, because
// the terminal's cursor stands where its own count put it.
TRIAL("App: a word the terminal may count otherwise is erased, written whole and followed by a jump") {
    const std::string word = "\xe0\xa4\x87\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\x95\xe0\xa4\xbe";

    Writing writing{word + " (2026)"};

    OUGHT(holds(writing.drawn(word + " (2026)"), "\x1b[1;1H\x1b[4X" + word + "\x1b[1;5H (2026)"));

    // An unchanged word stays; a change after it goes by itself.
    const std::string later = writing.drawn(word + " (2027)");

    OUGHT(!holds(later, word));
    OUGHT(!erasing(later));
    OUGHT(holds(later, "7"));

    // A changed word takes the rest of the row with it.
    const std::string changed = word.substr(0, word.size() - 3) + "\xe0\xa5\x80";

    OUGHT(holds(writing.drawn(changed + " (2027)"), "\x1b[4X" + changed + "\x1b[1;5H (2027)"));
}

// A cluster is cut and measured by the terminal's own rules; tal gives a spacing mark and a
// prepended character a cell of their own, a terminal following UAX #29 joins them to their
// neighbour. Their words go out whole, a double-width character in them as one letter.
TRIAL("App: a cluster, a spacing mark and a prepended character make their word one the terminal may count otherwise") {
    const std::pair<std::string, int> words[] = {{"\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\x95", 2},
                                                 {"\xe0\xa4\x95\xe0\xa4\xbe", 2},
                                                 {"\xd8\x80\xd9\xa1", 2},
                                                 {"\xe6\x97\xa5\xe0\xa4\x95\xe0\xa4\xbe", 4}};

    for (const auto& [word, wide] : words) {
        Writing writing{"ab " + word + " cd"};

        OUGHT(holds(writing.drawn("ab " + word + " cd"),
                    "\x1b[1;1Hab \x1b[" + std::to_string(wide) + "X" + word + "\x1b[1;" + std::to_string(4 + wide) + "H cd"));

        // Changes on both sides of an unchanged word close to it: the word is not rewritten as a
        // gap between them.
        const std::string around = writing.drawn("ax " + word + " yd");

        OUGHT(!holds(around, word));
        OUGHT(holds(around, "\x1b[1;" + std::to_string(5 + wide) + "Hy"));
    }
}

// ECH erases in the current background, so the word's look is set before it.
TRIAL("App: the erasing before a word goes in the word's own look") {
    const std::string word = "\xe0\xa4\x95\xe0\xa4\xbe";

    Writing writing{word};
    Look blue;

    blue.paper = Color{0, 0, 200};
    writing.worded->look = blue;

    OUGHT(holds(writing.drawn(word), "m\x1b[2X" + word));
}

TRIAL("App: plain words go out in one piece, without erasing and without extra jumps") {
    Writing writing{"Heat (1995) abc"};

    const std::string said = writing.drawn("Heat (1995) abc");

    OUGHT(holds(said, "\x1b[1;1HHeat (1995) abc"));
    OUGHT(!erasing(said));
}

// Whatever the terminal's count, the columns after the word stay where tal put them, and nothing
// of the previous picture is left under a word drawn narrower.
TRIAL("App: after a word the terminal counts otherwise, the columns stay in place") {
    const std::string word = "\xe0\xa4\x87\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\x95\xe0\xa4\xbe";
    const std::string other = "\xe0\xa4\x87\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\x95\xe0\xa5\x80";

    for (const Counting counting : {&narrower, &wider}) {
        Writing writing{"Heat (1995) abc"};
        Modelled shown{counting};

        // Draws the text and lets the model read what went out.
        const auto drawing = [&](const std::string& text) { shown.fed(writing.drawn(text)); };

        drawing("Heat (1995) abc");
        drawing(word + " (2026) abc");

        SAME(shown.from(4), std::string{" (2026) abc         "});

        if (counting == &narrower) SAME(shown.cells[2] + shown.cells[3], std::string{"  "});

        drawing(word + " (2026) abd");

        SAME(shown.from(4), std::string{" (2026) abd         "});

        drawing(other + " (2026) abd");

        SAME(shown.from(4), std::string{" (2026) abd         "});

        // A word that went out again takes the next one with it, though that one is unchanged: a
        // terminal counting wider drew over it.
        const std::string twice = "\xe0\xa4\x95\xe0\xa4\xbe\xe0\xa4\x95\xe0\xa4\xbe";
        const std::string changed = "\xe0\xa4\x95\xe0\xa5\x80\xe0\xa4\x95\xe0\xa4\xbe";

        drawing(twice + " " + word + " abc");
        drawing(changed + " " + word + " abc");

        SAME(shown.cells[5], std::string{"\xe0\xa4\x87"});
        SAME(shown.from(10), std::string{"abc           "});
    }
}

}
