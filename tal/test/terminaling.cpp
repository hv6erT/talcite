#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/clipboard.hpp"
#include "tal/document.hpp"
#include "tal/emulator.hpp"
#include "tal/executor.hpp"
#include "tal/fs.hpp"
#include "tal/key.hpp"
#include "tal/layout.hpp"
#include "tal/process.hpp"
#include "tal/scene.hpp"
#include "tal/stream.hpp"
#include "tal/task.hpp"
#include "tal/terminal.hpp"
#include "tal/trouble.hpp"
#include "tal/views.hpp"

#include "quieting.hpp"
#include "tal/testing.hpp"

// A terminal on a scene, its program held by the trial's hands: what the program writes lands on
// the screen and its answers go back, the end waits for the output, keys, pastes, the mouse and the
// wheel become the program's bytes, the history is walked, the prompt hidden, text selected and
// copied, links carried, synchronized drawing held back, and the size and the hang-up passed on.

namespace {

using namespace std::chrono_literals;

using tal::Bytes;
using tal::Code;
using tal::Command;
using tal::Document;
using tal::Exit;
using tal::Hooks;
using tal::Key;
using tal::Mods;
using tal::MouseButton;
using tal::Press;
using tal::Process;
using tal::Size;
using tal::Spot;
using tal::Stage;
using tal::Staged;
using tal::Stream;
using tal::Task;
using tal::Terminal;
using tal::testing::Scene;

// A program the trial holds by hand: its exit settles when the trial says, and what happens to it
// is counted.
struct Hand : tal::Spawned {
    explicit Hand(tal::Executor& where) : exited(exit.task(), where) {}

    tal::Pending<Exit> exit;
    tal::Shared<Exit> exited;
    std::shared_ptr<int> hung = std::make_shared<int>(0);
    std::vector<Size> sizes;

    // A number like a system's.
    [[nodiscard]] std::int64_t id() const override { return 7; }

    // How it ended, once the trial says.
    Task<Exit> ended() override { return exited.get(); }

    // Asked to stop, it goes on: the trial ends it.
    void stop() override {}

    // Nor does killing end it.
    void kill() override {}

    // A hang-up is counted, where the count may outlive the program.
    void hang_up() override { ++*hung; }

    // A new size is kept.
    void resize(int columns, int rows) override { sizes.push_back(Size{columns, rows}); }
};

// The port of processes the trials start their programs through: one program at a time, its output
// a pipe the trial writes, what the terminal writes to it gathered in `typed`.
class Hands : public tal::Processes {
public:
    explicit Hands(tal::Loop& on) : loop_(on), scope_(on) {}

    std::vector<Command> started;
    std::vector<std::string> opened;
    std::shared_ptr<Hand> hand;
    Stream screen;
    std::string typed;
    Stream ear;
    bool refusing = false;
    bool slow = false;
    tal::Pending<void> gate;

    // The adapter's name.
    [[nodiscard]] std::string_view name() const override { return "hands"; }

    // Starts the program: a pipe each way, and what comes to the program gathered.
    Task<Process> start(Command what) override {
        if (what.mode == tal::Mode::opened) {
            opened.push_back(what.arguments.at(0));

            co_return Process{};
        }

        started.push_back(what);

        if (refusing) throw tal::Trouble{tal::Kind::missing, "There is no such program."};

        if (slow) co_await gate.task();

        auto [theirs, ours] = tal::detail::piped();
        auto [read, written] = tal::detail::piped();

        screen = written;
        ear = theirs;
        hand = std::make_shared<Hand>(loop_);
        scope_.run(gathered(theirs));

        co_return Process{hand, ours, read, Stream{}};
    }

    // The program writes `said`.
    void write(std::string_view said) { tal::wait(loop_, screen.write(tal::bytes(said))); }

    // The program's output ends.
    void close() { screen.close(); }

    // The program stops reading: what is written to it fails from now on.
    void deafen() { ear.close(); }

    // The program exits with `code`.
    void end(int code) { hand->exit.settle(Exit{Exit::Ended::normal, code, 0}); }

private:
    // Gathers what comes to the program until its input ends.
    Task<void> gathered(Stream from) {
        for (;;) {
            const Bytes got = co_await from.read(4096);

            if (got.empty()) co_return;

            typed += tal::text::of(got);
        }
    }

    tal::Loop& loop_;
    tal::Scope scope_;
};

// A clipboard that keeps what it is given.
class Board : public tal::Clipboard {
public:
    std::string kept;

    // The adapter's name.
    [[nodiscard]] std::string_view name() const override { return "board"; }

    // Keeps the text.
    Task<bool> copy(std::string what) override {
        kept = std::move(what);

        co_return true;
    }

    // Gives what it keeps.
    Task<std::optional<std::string>> paste() override { co_return kept; }
};

// A node that takes the focus and does nothing else.
struct Other : tal::Node {
    // Focusable, nothing more.
    Other() { focusable(true); }
};

// A node around the terminal whose default actions catch what the terminal left: keys, pastes and
// the mouse, counted.
struct Catcher : tal::Node {
    int keys = 0;
    int pastes = 0;
    int buttons = 0;
    Hooks hooks;

    // Counts what reaches its default actions.
    Catcher() {
        hooks.add(otherwise<tal::KeyEvent>([this](tal::KeyEvent&) { ++keys; }));
        hooks.add(otherwise<tal::PasteEvent>([this](tal::PasteEvent&) { ++pastes; }));
        hooks.add(otherwise<tal::MouseEvent>([this](tal::MouseEvent& one) {
            if (one.button == MouseButton::right) ++buttons;
        }));
    }
};

// A terminal filling a scene of `size`, its program started and focused, with what it told.
struct Shown {
    Scene scene;
    tal::Loop loop{scene.time()};
    std::shared_ptr<Hands> hands = std::make_shared<Hands>(loop);
    std::shared_ptr<Board> board = std::make_shared<Board>();
    Document document{scene.time(), tal::Port<tal::Processes>{hands}};
    Catcher* catcher = nullptr;
    Terminal* terminal = nullptr;
    Other* other = nullptr;
    std::vector<std::string> moved;
    std::vector<std::string> titles;
    std::vector<Staged> stages;
    std::vector<bool> switches;
    std::vector<Exit> finished;
    int rang = 0;
    int scrolled = 0;
    Hooks hooks;

    // Builds, shows and starts it.
    explicit Shown(Size size = {20, 5}, std::size_t kept = 100, bool refusing = false, bool starting = true, bool sharing = false) : scene({.screen = size}) {
        Command what;

        what.program = tal::Path{"sh"};
        hands->refusing = refusing;

        catcher = &document.children().add<Catcher>();

        if (sharing) {
            tal::Column& column = catcher->children().add<tal::Column>();

            terminal = &column.children().add<Terminal>(what, kept, tal::Port<tal::Processes>{hands}, tal::Port<tal::Clipboard>{board});
            other = &column.children().add<Other>();
            terminal->share(tal::Share{1, 0});
            other->share(tal::Share{1, 0});
        } else {
            terminal = &catcher->children().add<Terminal>(what, kept, tal::Port<tal::Processes>{hands}, tal::Port<tal::Clipboard>{board});
        }

        hooks.add(terminal->on<tal::Moved>([this](tal::Moved& one) { moved.push_back(one.folder); }));
        hooks.add(terminal->on<tal::Titled>([this](tal::Titled& one) { titles.push_back(one.title); }));
        hooks.add(terminal->on<tal::Marked>([this](tal::Marked& one) { stages.push_back(one.stage); }));
        hooks.add(terminal->on<tal::Switched>([this](tal::Switched& one) { switches.push_back(one.alternate); }));
        hooks.add(terminal->on<tal::Finished>([this](tal::Finished& one) { finished.push_back(one.exit); }));
        hooks.add(terminal->on<tal::Rang>([this](tal::Rang&) { ++rang; }));
        hooks.add(terminal->on<tal::Scrolled>([this](tal::Scrolled&) { ++scrolled; }));
        scene.show(document);
        terminal->focus();

        if (starting) terminal->run();

        settled();
    }

    // Lets the loop and the frames go until both are quiet.
    void settled() {
        for (int turn = 0; turn < 3; ++turn) {
            QUIETED(loop);
            scene.wait(0ms);
        }
    }

    // The program writes `said`, and everything settles.
    void shows(std::string_view said) {
        hands->write(said);
        settled();
    }

    // What came to the program since the last time, and forgets it.
    std::string typed() {
        settled();

        std::string made = hands->typed;

        hands->typed.clear();

        return made;
    }

    // The row `y` of the screen without the spaces at its end.
    std::string row(int y) const {
        std::string made = scene.row(y);

        while (!made.empty() && made.back() == ' ') made.pop_back();

        return made;
    }

    // What the mouse did at `at` with `mods`.
    void mouse(Press press, MouseButton button, Spot at, Mods mods = {}) {
        document.mouse(press, button, at, mods);
        settled();
    }
};

}

TRIAL("Terminal: the program writes onto the screen, hears the answers and tells the tree") {
    Shown shown;

    SAME(shown.hands->started.size(), std::size_t{1});
    OUGHT(shown.hands->started[0].mode == tal::Mode::terminal);
    SAME(shown.hands->started[0].columns, 20);
    SAME(shown.hands->started[0].rows, 5);
    OUGHT(shown.terminal->running());
    OUGHT(shown.terminal->has_class("terminal"));

    shown.shows("hello\r\n\x1b[6n");

    SAME(shown.row(0), std::string{"hello"});
    SAME(shown.typed(), std::string{"\x1b[2;1R"});

    shown.shows("\x1b]2;tytuł\x07\x07\x1b]7;file://h/tmp\x07\x1b]133;A\x07$ \x1b[?1049h");

    OUGHT(shown.titles == (std::vector<std::string>{"tytuł"}));
    SAME(shown.rang, 1);
    OUGHT(shown.moved == (std::vector<std::string>{"/tmp"}));
    SAME(shown.stages.size(), std::size_t{1});
    OUGHT(shown.stages[0].stage == Stage::prompt);
    OUGHT(shown.switches == (std::vector<bool>{true}));

    shown.shows("\x1b]7;file://h/tmp\x07\x1b[?1049l");

    SAME(shown.moved.size(), std::size_t{1});
    OUGHT(shown.switches == (std::vector<bool>{true, false}));
}

TRIAL("Terminal: the end waits for the output, or comes 500 ms after the exit; nothing is told after it") {
    {
        Shown shown;

        shown.hands->end(3);
        shown.settled();

        OUGHT(shown.finished.empty());
        OUGHT(shown.terminal->running());

        shown.scene.wait(499ms);
        shown.settled();

        OUGHT(shown.finished.empty());

        shown.scene.wait(1ms);
        shown.settled();

        SAME(shown.finished.size(), std::size_t{1});
        OUGHT(shown.finished[0] == (Exit{Exit::Ended::normal, 3, 0}));
        OUGHT(!shown.terminal->running());

        shown.shows("late\x1b[6n\x1b]2;x\x07");

        SAME(shown.row(0), std::string{"late"});
        SAME(shown.typed(), std::string{});
        OUGHT(shown.titles.empty());
        SAME(shown.finished.size(), std::size_t{1});

        shown.terminal->send("x");

        SAME(shown.typed(), std::string{});

        shown.hands->close();
        shown.settled();

        SAME(shown.finished.size(), std::size_t{1});

        shown.scene.paste("y");

        SAME(shown.catcher->pastes, 1);

        const std::size_t before = shown.hands->started.size();

        shown.terminal->run();
        shown.settled();

        SAME(shown.hands->started.size(), before);
    }

    {
        Shown early;

        early.hands->end(3);
        early.settled();
        early.hands->close();
        early.settled();

        SAME(early.finished.size(), std::size_t{1});

        early.scene.wait(500ms);
        early.settled();

        SAME(early.finished.size(), std::size_t{1});
    }

    {
        Shown closed;

        closed.hands->close();
        closed.settled();

        OUGHT(closed.finished.empty());

        closed.hands->end(0);
        closed.settled();

        SAME(closed.finished.size(), std::size_t{1});
    }

    Shown refused{{20, 5}, 100, true};

    SAME(refused.finished.size(), std::size_t{1});
    OUGHT(refused.finished[0].ended == Exit::Ended::lost);
    OUGHT(!refused.terminal->running());
}

TRIAL("Terminal: keys become xterm's bytes in the program's modes, and Enter at the prompt is a command") {
    tal::Modes plain;
    tal::Modes cursor;

    cursor.cursor_keys = true;

    SAME(tal::text::of(tal::keyed(Key{U'a'}, plain)), std::string{"a"});
    SAME(tal::text::of(tal::keyed(Key{U'ż'}, plain)), std::string{"ż"});
    SAME(tal::text::of(tal::keyed(Key{U'C', Mods{.ctrl = true}}, plain)), std::string{"\x03"});
    SAME(tal::text::of(tal::keyed(Key{U'x', Mods{.alt = true}}, plain)), std::string{"\x1bx"});
    SAME(tal::text::of(tal::keyed(Key{U' ', Mods{.ctrl = true}}, plain)), std::string(1, '\0'));
    SAME(tal::text::of(tal::keyed(Key{U'[', Mods{.ctrl = true}}, plain)), std::string{"\x1b"});
    SAME(tal::text::of(tal::keyed(Key{U'\\', Mods{.ctrl = true}}, plain)), std::string{"\x1c"});
    SAME(tal::text::of(tal::keyed(Key{U']', Mods{.ctrl = true}}, plain)), std::string{"\x1d"});
    SAME(tal::text::of(tal::keyed(Key{U'^', Mods{.ctrl = true}}, plain)), std::string{"\x1e"});
    SAME(tal::text::of(tal::keyed(Key{U'/', Mods{.ctrl = true}}, plain)), std::string{"\x1f"});
    SAME(tal::text::of(tal::keyed(Key{U'?', Mods{.ctrl = true}}, plain)), std::string{"\x7f"});
    SAME(tal::text::of(tal::keyed(Key{U'ż', Mods{.ctrl = true}}, plain)), std::string{"ż"});
    SAME(tal::text::of(tal::keyed(Key{Code::up}, plain)), std::string{"\x1b[A"});
    SAME(tal::text::of(tal::keyed(Key{Code::up}, cursor)), std::string{"\x1bOA"});
    SAME(tal::text::of(tal::keyed(Key{Code::up, Mods{.ctrl = true}}, cursor)), std::string{"\x1b[1;5A"});
    SAME(tal::text::of(tal::keyed(Key{Code::f13}, plain)), std::string{"\x1b[1;2P"});
    SAME(tal::text::of(tal::keyed(Key{Code::f20}, plain)), std::string{"\x1b[19;2~"});
    OUGHT(tal::keyed(Key{Code::none}, plain).empty());

    Shown shown;

    shown.scene.press(Key{U'a'});
    shown.scene.press(Key{Code::up});

    SAME(shown.typed(), std::string{"a\x1b[A"});

    shown.shows("\x1b]133;A\x07$ \x1b]133;B\x07");

    OUGHT(shown.terminal->idle());

    shown.scene.press(Key{Code::enter});

    SAME(shown.typed(), std::string{"\r"});
    OUGHT(!shown.terminal->idle());

    shown.shows("\x1b]133;A\x07");

    OUGHT(shown.terminal->idle());

    shown.terminal->submit("ls");

    SAME(shown.typed(), std::string{"ls\r"});
    OUGHT(!shown.terminal->idle());

    shown.hands->close();
    shown.hands->end(0);
    shown.settled();

    bool reached = false;

    shown.hooks.add(shown.document.on<tal::KeyEvent>([&reached](tal::KeyEvent&) { reached = true; }));
    shown.scene.press(Key{U'b'});

    OUGHT(reached);
    OUGHT(!shown.terminal->idle());
}

TRIAL("Terminal: a paste goes without escapes, in brackets when asked, and only when it fits") {
    Shown shown;

    shown.scene.paste("a\r\nb\nc\x1b[201~d\te\x01");

    SAME(shown.typed(), std::string{"a\rb\rc[201~d\te"});

    shown.shows("\x1b[?2004h");
    shown.scene.paste("x");

    SAME(shown.typed(), std::string{"\x1b[200~x\x1b[201~"});
    OUGHT(!shown.terminal->paste(std::string((std::size_t{1} << 20) + 1, 'y')));
    SAME(shown.typed(), std::string{});
    OUGHT(shown.terminal->paste("z"));
    SAME(shown.typed(), std::string{"\x1b[200~z\x1b[201~"});
}

// Files dropped on the outer terminal reach the program inside as the paste the outer one wrote,
// before the node around it sees the drop; with no program running the drop goes on up. The path
// is in double quotes, which terminals write alike on every system (on Windows a single quote is
// a letter of the name).
TRIAL("Terminal: files dropped reach the program as their paste") {
    Shown shown;
    auto files = std::make_shared<tal::fs::Memory>(shown.scene.time());
    int caught = 0;

    tal::wait(shown.loop, files->write(tal::Path{"/c.txt"}, tal::bytes("c")));
    tal::defaults().set<tal::fs::Files>(files);
    shown.hooks.add(shown.catcher->on<tal::DropEvent>([&caught](tal::DropEvent& one) {
        ++caught;
        one.take();
    }));
    shown.scene.paste("\"/c.txt\" ");

    QUIETED(shown.loop);

    SAME(shown.typed(), std::string{"\"/c.txt\" "});
    SAME(caught, 0);
    SAME(shown.catcher->pastes, 0);

    tal::defaults().clear<tal::fs::Files>();
}

// A terminal whose program does not run leaves a drop to the nodes above it.
TRIAL("Terminal: files dropped with no program go on up") {
    Shown idle{{20, 5}, 100, false, false};
    auto files = std::make_shared<tal::fs::Memory>(idle.scene.time());
    int caught = 0;

    tal::wait(idle.loop, files->write(tal::Path{"/c.txt"}, tal::bytes("c")));
    tal::defaults().set<tal::fs::Files>(files);
    idle.hooks.add(idle.catcher->on<tal::DropEvent>([&caught](tal::DropEvent& one) {
        ++caught;
        one.take();
    }));
    idle.scene.paste("\"/c.txt\" ");

    QUIETED(idle.loop);

    SAME(idle.catcher->pastes, 0);
    SAME(caught, 1);
    SAME(idle.hands->typed, std::string{});

    tal::defaults().clear<tal::fs::Files>();
}

TRIAL("Terminal: the mouse goes to a program that asked for it, in xterm's SGR encoding") {
    Shown shown;

    shown.shows("\x1b[?1000;1006h");
    shown.scene.click({2, 1});

    SAME(shown.typed(), std::string{"\x1b[<0;3;2M\x1b[<0;3;2m"});

    shown.scene.click({4, 3}, MouseButton::right);
    shown.mouse(Press::move, MouseButton::none, {5, 3});

    SAME(shown.typed(), std::string{"\x1b[<2;5;4M\x1b[<2;5;4m"});

    shown.shows("\x1b[?1002h");
    shown.mouse(Press::down, MouseButton::left, {1, 1}, Mods{.ctrl = true});
    shown.mouse(Press::move, MouseButton::left, {2, 1});
    shown.mouse(Press::move, MouseButton::none, {3, 1});
    shown.mouse(Press::up, MouseButton::left, {2, 1});

    SAME(shown.typed(), std::string{"\x1b[<16;2;2M\x1b[<32;3;2M\x1b[<0;3;2m"});

    shown.shows("\x1b[?1003h");
    shown.mouse(Press::move, MouseButton::none, {3, 1});
    shown.scene.wheel({1, 1}, 1);
    shown.scene.wheel({1, 1}, -2);

    SAME(shown.typed(), std::string{"\x1b[<35;4;2M\x1b[<64;2;2M\x1b[<65;2;2M\x1b[<65;2;2M"});

    shown.mouse(Press::down, MouseButton::left, {0, 0}, Mods{.shift = true});
    shown.mouse(Press::up, MouseButton::left, {0, 0}, Mods{.shift = true});

    SAME(shown.typed(), std::string{});

    shown.shows("\x1b[?1006l");
    shown.scene.click({2, 1});

    SAME(shown.typed(), std::string{});
}

TRIAL("Terminal: the wheel walks the history, or gives a full-screen program arrows") {
    Shown shown;
    std::string lines;

    for (int at = 0; at < 20; ++at) lines += "line " + std::to_string(at) + "\r\n";

    shown.shows(lines);

    SAME(shown.row(0), std::string{"line 16"});
    SAME(shown.terminal->extent(), std::size_t{21});
    SAME(shown.terminal->window(), std::size_t{5});
    SAME(shown.terminal->top(), std::size_t{16});

    const int was = shown.scrolled;

    shown.scene.wheel({1, 1}, 1);

    SAME(shown.row(0), std::string{"line 13"});
    SAME(shown.terminal->top(), std::size_t{13});
    OUGHT(shown.scrolled > was);
    OUGHT(!shown.scene.caret().has_value());

    shown.scene.wheel({1, 1}, -1);

    SAME(shown.row(0), std::string{"line 16"});

    shown.shows("\x1b[?1049h");
    shown.scene.wheel({1, 1}, 1);

    SAME(shown.typed(), std::string{"\x1b[A\x1b[A\x1b[A"});

    shown.shows("\x1b[?1h");
    shown.scene.wheel({1, 1}, -1);

    SAME(shown.typed(), std::string{"\x1bOB\x1bOB\x1bOB"});

    shown.scene.wheel({1, 1}, 1, tal::Axis::horizontal);

    SAME(shown.typed(), std::string{});
}

TRIAL("Terminal: Shift with the page keys walks the history; output or a key brings the live screen back") {
    Shown shown;
    std::string lines;

    for (int at = 0; at < 20; ++at) lines += "line " + std::to_string(at) + "\r\n";

    shown.shows(lines);
    shown.scene.press(Key{Code::pageup, Mods{.shift = true}});

    SAME(shown.row(0), std::string{"line 12"});
    SAME(shown.typed(), std::string{});

    shown.scene.press(Key{Code::pageup, Mods{.shift = true}});
    shown.scene.press(Key{Code::pageup, Mods{.shift = true}});
    shown.scene.press(Key{Code::pageup, Mods{.shift = true}});
    shown.scene.press(Key{Code::pageup, Mods{.shift = true}});
    shown.scene.press(Key{Code::pageup, Mods{.shift = true}});

    SAME(shown.row(0), std::string{"line 0"});
    SAME(shown.terminal->top(), std::size_t{0});

    shown.scene.press(Key{Code::pagedown, Mods{.shift = true}});

    SAME(shown.row(0), std::string{"line 4"});

    shown.shows("x");

    SAME(shown.row(0), std::string{"line 16"});

    shown.terminal->top(2);
    shown.settled();

    SAME(shown.row(0), std::string{"line 2"});

    shown.scene.press(Key{U'q'});

    SAME(shown.row(0), std::string{"line 16"});
    SAME(shown.typed(), std::string{"q"});

    shown.terminal->top(1000);
    shown.settled();

    SAME(shown.row(0), std::string{"line 16"});

    shown.shows("\x1b[?1049h");
    shown.scene.press(Key{Code::pageup, Mods{.shift = true}});

    SAME(shown.typed(), std::string{"\x1b[5;2~"});
}

TRIAL("Terminal: the rows of the prompt may hide under a command line of the program's own") {
    Shown shown;

    shown.shows("a\r\nb\r\n\x1b]133;A\x07$ ");

    SAME(shown.row(2), std::string{"$"});
    OUGHT(shown.scene.caret().has_value() && shown.scene.caret()->at == (Spot{2, 2}));

    shown.terminal->prompt = false;
    shown.settled();

    SAME(shown.row(4), std::string{"b"});
    SAME(shown.row(3), std::string{"a"});
    OUGHT(!shown.scene.caret().has_value());

    shown.terminal->prompt = true;
    shown.settled();

    SAME(shown.row(2), std::string{"$"});
}

TRIAL("Terminal: the cursor in the form the program asks for, shown while it may be") {
    Shown shown;

    shown.shows("ab\x1b[5 q");

    std::optional<tal::Caret> got = shown.scene.caret();

    OUGHT(got.has_value());
    OUGHT(got->at == (Spot{2, 0}));
    OUGHT(got->form == tal::Caret::Form::bar && got->blink);

    shown.shows("\x1b[?25l");

    OUGHT(!shown.scene.caret().has_value());

    shown.shows("\x1b[?25h");

    Other& other = shown.document.children().add<Other>();

    shown.settled();
    other.focus();
    shown.settled();

    OUGHT(!shown.scene.caret().has_value());
}

TRIAL("Terminal: the focus coming and going is told to a program that asked") {
    Shown shown;

    Other& other = shown.document.children().add<Other>();

    shown.settled();
    other.focus();
    shown.settled();
    shown.terminal->focus();
    shown.settled();

    SAME(shown.typed(), std::string{});

    shown.shows("\x1b[?1004h");
    other.focus();
    shown.settled();
    shown.terminal->focus();

    SAME(shown.typed(), std::string{"\x1b[O\x1b[I"});
}

TRIAL("Terminal: the mouse selects text, a double click a word; the selection rides with the output") {
    Shown shown{{10, 5}};

    shown.shows("one two\r\nthree four five\r\nsix");

    OUGHT(shown.terminal->selected().empty());

    shown.mouse(Press::down, MouseButton::left, {4, 0});
    shown.mouse(Press::move, MouseButton::left, {2, 3});
    shown.mouse(Press::up, MouseButton::left, {2, 3});

    SAME(shown.terminal->selected(), std::string{"two\nthree four five\nsix"});
    OUGHT(shown.scene.at({4, 0}).look.reverse);
    OUGHT(!shown.scene.at({3, 0}).look.reverse);

    shown.shows("\r\n\r\n\r\n");

    SAME(shown.row(0), std::string{" five"});
    SAME(shown.terminal->selected(), std::string{"two\nthree four five\nsix"});

    OUGHT(tal::wait(shown.loop, shown.terminal->copy()));
    SAME(shown.board->kept, std::string{"two\nthree four five\nsix"});

    shown.scene.twice({1, 1});

    SAME(shown.terminal->selected(), std::string{"six"});

    shown.terminal->unselect();
    shown.settled();

    OUGHT(shown.terminal->selected().empty());
    OUGHT(!tal::wait(shown.loop, shown.terminal->copy()));

    shown.shows("\x1b[?1000;1006h");
    shown.mouse(Press::down, MouseButton::left, {0, 1}, Mods{.shift = true});
    shown.mouse(Press::move, MouseButton::left, {2, 1}, Mods{.shift = true});

    SAME(shown.terminal->selected(), std::string{"six"});
    SAME(shown.typed(), std::string{});

    shown.scene.resize({12, 5});
    shown.settled();

    OUGHT(shown.terminal->selected().empty());
    SAME(shown.hands->hand->sizes.back(), (Size{12, 5}));
}

TRIAL("Terminal: links go onto the screen with their addresses") {
    Shown shown;

    shown.shows("\x1b]8;;https://x.pl/a\x07" "ab\x1b]8;;\x07" "c");

    const std::uint32_t link = shown.scene.at({0, 0}).link;

    OUGHT(link != 0);
    SAME(shown.scene.screen().address(link), std::string_view{"https://x.pl/a"});
    SAME(shown.scene.at({1, 0}).link, link);
    SAME(shown.scene.at({2, 0}).link, std::uint32_t{0});
}

TRIAL("Terminal: a program drawing in synchronized output holds the picture back, at most 200 ms") {
    Shown shown;

    shown.shows("old");
    shown.shows("\x1b[?2026h\x1b[1;1Hnew");

    SAME(shown.row(0), std::string{"old"});

    shown.shows("\x1b[?2026l");

    SAME(shown.row(0), std::string{"new"});

    shown.shows("\x1b[?2026h\x1b[1;1Hzzz");

    SAME(shown.row(0), std::string{"new"});

    shown.scene.wait(199ms);
    shown.settled();

    SAME(shown.row(0), std::string{"new"});

    shown.scene.wait(1ms);
    shown.settled();

    SAME(shown.row(0), std::string{"zzz"});
}

TRIAL("Terminal: a new size goes to the program, and going away hangs it up") {
    std::shared_ptr<int> hung;

    {
        Shown shown;

        hung = shown.hands->hand->hung;
        shown.scene.resize({30, 6});
        shown.settled();

        SAME(shown.terminal->screen().size(), (Size{30, 6}));
        SAME(shown.hands->hand->sizes.back(), (Size{30, 6}));
        SAME(*hung, 0);

        shown.terminal->hang_up();

        SAME(*hung, 1);
    }

    SAME(*hung, 2);
}

TRIAL("Terminal: a program pouring out text keeps the loop going and ends on its last line") {
    Shown shown{{40, 5}, 1000};
    std::string much;

    for (int at = 0; at < 20000; ++at) much += "row " + std::to_string(at) + " of a flood of text\r\n";

    for (std::size_t at = 0; at < much.size(); at += 4096) shown.hands->write(std::string_view{much}.substr(at, 4096));

    shown.hands->write("end");
    shown.settled();

    SAME(shown.row(3), std::string{"row 19999 of a flood of text"});
    SAME(shown.row(4), std::string{"end"});
    SAME(shown.terminal->screen().history(), std::size_t{1000});
}

TRIAL("Terminal: before the start what is sent waits, a new size meanwhile follows the start, and it starts once") {
    Shown shown{{20, 5}, 100, false, false};

    shown.hands->slow = true;
    shown.terminal->send("early");
    shown.terminal->run();
    shown.settled();
    shown.scene.resize({25, 5});
    shown.terminal->run();
    shown.hands->gate.settle();
    shown.settled();

    SAME(shown.hands->started.size(), std::size_t{1});
    SAME(shown.hands->started[0].columns, 20);
    SAME(shown.hands->hand->sizes.back(), (Size{25, 5}));
    SAME(shown.typed(), std::string{"early"});
}

TRIAL("Terminal: what a program no longer reads is dropped, and the queue takes more") {
    Shown shown;

    shown.hands->deafen();
    shown.settled();

    const std::string much((std::size_t{1} << 20) - 1000, 'x');

    OUGHT(shown.terminal->paste(much));
    shown.settled();
    OUGHT(shown.terminal->paste(much));
}

TRIAL("Terminal: a hold of synchronized output has its own 200 ms, however much comes meanwhile") {
    Shown shown;

    shown.shows("\x1b[?2026hA");
    shown.scene.wait(50ms);
    shown.shows("\x1b[?2026l");

    SAME(shown.row(0), std::string{"A"});

    shown.scene.wait(50ms);
    shown.shows("\x1b[?2026h\x1b[1;1HB");
    shown.scene.wait(100ms);
    shown.settled();

    SAME(shown.row(0), std::string{"A"});

    shown.scene.wait(99ms);
    shown.settled();

    SAME(shown.row(0), std::string{"A"});

    shown.scene.wait(1ms);
    shown.settled();

    SAME(shown.row(0), std::string{"B"});

    shown.shows("\x1b[?2026l\x1b[?2026h\x1b[1;1HC");
    shown.scene.wait(150ms);
    shown.shows("D");
    shown.scene.wait(50ms);
    shown.settled();

    SAME(shown.row(0), std::string{"CD"});
}

TRIAL("Terminal: the mouse focuses the terminal, leaves it the buttons it does not use, and reports no others") {
    Shown shown{{20, 6}, 100, false, true, true};

    shown.other->focus();
    shown.settled();
    shown.scene.click({1, 1});

    OUGHT(shown.terminal->focused());

    shown.scene.click({1, 1}, MouseButton::right);

    OUGHT(shown.catcher->buttons > 0);

    shown.shows("abc\r\ndef\r\nghi");
    shown.mouse(Press::down, MouseButton::left, {0, 0});
    shown.mouse(Press::move, MouseButton::left, {1, 4});
    shown.mouse(Press::up, MouseButton::left, {1, 4});

    SAME(shown.terminal->selected(), std::string{"abc\ndef\ngh"});

    shown.mouse(Press::down, MouseButton::left, {0, 4});
    shown.mouse(Press::move, MouseButton::left, {2, 1});
    shown.mouse(Press::up, MouseButton::left, {2, 1});

    SAME(shown.terminal->selected(), std::string{"abc\ndef\ngh"});

    shown.shows("\x1b[?1000;1006h");
    shown.mouse(Press::down, MouseButton::back, {1, 1});
    shown.mouse(Press::up, MouseButton::back, {1, 1});

    SAME(shown.typed(), std::string{});
}

TRIAL("Terminal: a hidden prompt shortens the history walked, and the view keeps within it") {
    Shown shown;
    std::string lines;

    for (int at = 0; at < 20; ++at) lines += "line " + std::to_string(at) + "\r\n";

    shown.shows(lines + "\x1b]133;A\x07$ ");
    shown.terminal->top(0);
    shown.settled();

    SAME(shown.row(0), std::string{"line 0"});

    shown.terminal->prompt = false;
    shown.settled();

    SAME(shown.terminal->top(), std::size_t{0});
    SAME(shown.row(0), std::string{"line 0"});
    SAME(shown.terminal->extent(), std::size_t{20});
}

TRIAL("Terminal: a double click on half a wide character takes its word; a selection loses what left the history") {
    {
        Shown shown;

        shown.shows("日本 x\r\nab、cd");
        shown.scene.twice({1, 0});

        SAME(shown.terminal->selected(), std::string{"日本"});

        shown.scene.twice({2, 1});

        SAME(shown.terminal->selected(), std::string{"ab、cd"});

        shown.mouse(Press::down, MouseButton::left, {0, 0});
        shown.mouse(Press::move, MouseButton::left, {1, 1});
        shown.mouse(Press::up, MouseButton::left, {1, 1});

        SAME(shown.terminal->selected(), std::string{"日本 x\nab"});

        shown.shows("\x1b[?1049h");

        SAME(shown.terminal->selected(), std::string{});
    }

    Shown few{{10, 3}, 2};

    few.shows("a\r\nb\r\nc");
    few.mouse(Press::down, MouseButton::left, {0, 0});
    few.mouse(Press::move, MouseButton::left, {0, 2});
    few.mouse(Press::up, MouseButton::left, {0, 2});

    SAME(few.terminal->selected(), std::string{"a\nb\nc"});
    OUGHT(!few.scene.at({1, 2}).look.reverse);

    few.shows("\r\n\r\n\r\n\r\n");

    SAME(few.terminal->selected(), std::string{"c"});
    OUGHT(!few.terminal->screen().row(-2).cells.empty());

    few.shows("\x1b[?1049h");

    OUGHT(few.terminal->selected().empty());
}

TRIAL("Terminal: the terminal's own colors under the program's defaults, links over wide characters, a steady cursor") {
    Shown shown;

    shown.document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "terminal", "style": {"ink": "#ff0000"}}], "glyphs": {}})"));
    shown.shows("x\x1b[32my\x1b[0m\x1b]8;;https://x.pl/\x07日\x1b]8;;\x07\x1b[2 q");

    OUGHT(shown.scene.at({0, 0}).look.ink == (tal::Color{255, 0, 0}));
    OUGHT(shown.scene.at({1, 0}).look.ink != (tal::Color{255, 0, 0}));
    OUGHT(shown.scene.at({2, 0}).link != 0);
    SAME(shown.scene.at({3, 0}).link, shown.scene.at({2, 0}).link);
    OUGHT(shown.scene.caret().has_value() && !shown.scene.caret()->blink);
    OUGHT(shown.scene.caret()->form == tal::Caret::Form::block);
}

TRIAL("Terminal: Ctrl with a click on a link follows it; a plain click selects") {
    Shown shown;

    std::vector<std::string> told;
    tal::Hooks hooks;

    hooks.add(shown.terminal->on<tal::Followed>([&told](tal::Followed& one) { told.push_back(one.address); }));
    shown.shows("\x1b]8;;https://x.pl/\x07link\x1b]8;;\x07 text");
    shown.scene.click({1, 0});

    OUGHT(shown.hands->opened.empty());

    shown.scene.wait(1s);
    shown.mouse(Press::down, MouseButton::left, {1, 0}, Mods{.ctrl = true});
    shown.mouse(Press::up, MouseButton::left, {1, 0}, Mods{.ctrl = true});

    OUGHT(shown.hands->opened == (std::vector<std::string>{"https://x.pl/"}));
    OUGHT(shown.terminal->selected().empty());

    shown.mouse(Press::down, MouseButton::left, {6, 0}, Mods{.ctrl = true});
    shown.mouse(Press::up, MouseButton::left, {6, 0}, Mods{.ctrl = true});

    SAME(shown.hands->opened.size(), std::size_t{1});
    OUGHT(told == (std::vector<std::string>{"https://x.pl/"}));
}
