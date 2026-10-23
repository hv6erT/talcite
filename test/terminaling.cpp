#include "proving.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/child.hpp"
#include "talcite/encoding.hpp"
#include "talcite/fs.hpp"
#include "talcite/loop.hpp"
#include "talcite/root.hpp"
#include "talcite/terminal.hpp"

using namespace talcite;
using namespace talcite::ui;

namespace {

// The test binary pretending to be a program: `tty` answers line by line, `prompted` shows a prompt
// with its marks and its folder (see proving.cpp).
Command pretending(const char* what) {
    Command made;

    made.program = fs::path{proving::mine()};
    made.arguments = {"--pretending", what};

    return made;
}

// The rows of a frame as text, without the spaces at their ends.
std::vector<std::string> rows(const View& one) {
    std::vector<std::string> made;

    for (int y = 0; y < one.size().high; ++y) {
        std::string line;

        for (int x = 0; x < one.size().wide; ++x)
            if (const char32_t letter = one.at({x, y}).letter; letter != 0) line += spelled(letter);

        while (!line.empty() && line.back() == ' ') line.pop_back();

        made.push_back(line);
    }

    return made;
}

// Whether a row of the frame holds the text.
bool showing(const View& one, const std::string& wanted) {
    for (const std::string& row : rows(one))
        if (row.find(wanted) != std::string::npos) return true;

    return false;
}

// Turns the loop, drawing the root at `size` each time, until `until` holds or five seconds pass.
template <typename Until>
bool awaited(Loop& loop, Root& top, Size size, Until until) {
    const auto began = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() - began < std::chrono::seconds{5}) {
        (void)loop.turn();

        if (until(top.render({{0, 0}, size}))) return true;

        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    return false;
}

// The bytes of a key as text.
std::string key(Key one, Modes modes = {}) { return text(Terminal::keyed(one, modes)); }

}

// --- Keys --------------------------------------------------------------------------------------

// Letters go as UTF-8, Ctrl with a letter as its control character, Alt as a leading escape.
TRIAL("Terminal: letters, Ctrl and Alt as xterm writes them") {
    same(key(Key{U'a'}), std::string{"a"});
    same(key(Key{U'ż'}), std::string{"ż"});
    same(key(Key{U'c', Mods{.ctrl = true}}), std::string{"\x03"});
    same(key(Key{U'C', Mods{.ctrl = true}}), std::string{"\x03"});
    same(key(Key{U'[', Mods{.ctrl = true}}), std::string{"\x1b"});
    same(key(Key{U' ', Mods{.ctrl = true}}), std::string(1, '\0'));
    same(key(Key{U'x', Mods{.alt = true}}), std::string{"\x1bx"});
}

// The keys of editing: Enter as a carriage return, Backspace as DEL, Shift+Tab as back tab.
TRIAL("Terminal: Enter, Backspace, Tab and Escape") {
    same(key(Key{Code::enter}), std::string{"\r"});
    same(key(Key{Code::backspace}), std::string{"\x7f"});
    same(key(Key{Code::tab}), std::string{"\t"});
    same(key(Key{Code::tab, Mods{.shift = true}}), std::string{"\x1b[Z"});
    same(key(Key{Code::escape}), std::string{"\x1b"});
}

// The arrows and Home and End: CSI in normal mode, SS3 in the application mode a program asks for,
// and with modifiers always CSI 1 ; m.
TRIAL("Terminal: arrows in both cursor modes and with modifiers") {
    Modes application;

    application.cursor_keys = true;

    same(key(Key{Code::up}), std::string{"\x1b[A"});
    same(key(Key{Code::up}, application), std::string{"\x1bOA"});
    same(key(Key{Code::left, Mods{.ctrl = true}}), std::string{"\x1b[1;5D"});
    same(key(Key{Code::right, Mods{.shift = true}}, application), std::string{"\x1b[1;2C"});
    same(key(Key{Code::home}), std::string{"\x1b[H"});
    same(key(Key{Code::end}, application), std::string{"\x1bOF"});
}

// The keys with a number: Insert, Delete, the pages and F5–F12; F1–F4 as SS3; F13 as Shift+F1.
TRIAL("Terminal: function keys and the keys with a number") {
    same(key(Key{Code::remove}), std::string{"\x1b[3~"});
    same(key(Key{Code::pageup, Mods{.ctrl = true}}), std::string{"\x1b[5;5~"});
    same(key(Key{Code::f1}), std::string{"\x1bOP"});
    same(key(Key{Code::f4, Mods{.alt = true}}), std::string{"\x1b[1;3S"});
    same(key(Key{Code::f5}), std::string{"\x1b[15~"});
    same(key(Key{Code::f12}), std::string{"\x1b[24~"});
    same(key(Key{Code::f13}), std::string{"\x1b[1;2P"});
}

// --- A program in the node -------------------------------------------------------------------

// A program in the terminal sees a terminal of the node's size, gets what is sent as typed keys and
// a key pressed while the terminal has the focus, sees the new size when the node is drawn smaller,
// and its end comes up the tree.
TRIAL("Terminal: a program runs in the node") {
    Loop loop;
    Root top;

    const auto one = std::make_shared<Terminal>(pretending("tty"), Size{40, 5});

    std::optional<Exit> end;

    const ui::Hook heard = one->on<EndEvent>([&end](EndEvent& what) {
        end = what.exit();

        return true;
    });

    top.children().add(one);

    one->focus();
    one->run();

    ought(awaited(loop, top, {40, 5}, [](const View& seen) { return showing(seen, "tty 1 40x5"); }));

    one->send("dzień dobry\r");

    ought(awaited(loop, top, {40, 5}, [](const View& seen) { return showing(seen, "got:dzień dobry"); }));

    ought(awaited(loop, top, {30, 4}, [](const View&) { return true; }));

    KeyEvent typed{Key{U's'}};

    ought(top.dispatch(typed));

    one->send("ize\r");

    ought(awaited(loop, top, {30, 4}, [](const View& seen) { return showing(seen, "size 30x4"); }));

    one->send("quit\r");

    ought(awaited(loop, top, {30, 4}, [&end](const View&) { return end.has_value(); }));
    ought(end.has_value() && end->ended == Ended::normal && end->code == 0);
    ought(!one->running());
}

// A shell integrated with its terminal tells it its folder and its prompt. Asked not to show the
// prompt, the node ends its picture just above it, with what the shell wrote before it. The shell
// waits there idle until a command line goes to it.
TRIAL("Terminal: the folder and the prompt of a shell") {
    Loop loop;
    Root top;

    const auto one = std::make_shared<Terminal>(pretending("prompted"), Size{30, 3});

    std::string folder;
    std::vector<Marked> marks;

    const ui::Hook moved = one->on<FolderEvent>([&folder](FolderEvent& what) {
        folder = what.folder();

        return true;
    });

    const ui::Hook marked = one->on<MarkEvent>([&marks](MarkEvent& what) {
        marks.push_back(what.mark().kind);

        return true;
    });

    top.children().add(one);

    one->run();

    ought(awaited(loop, top, {30, 3}, [](const View& seen) { return showing(seen, "$"); }));
    same(folder, std::string{"/tmp/prompted"});
    ought(marks == (std::vector<Marked>{Marked::prompt, Marked::input}));
    ought(one->screen().waiting());

    one->prompt_shown(false);

    const std::vector<std::string> shown = rows(top.render({{0, 0}, {30, 3}}));

    same(shown[1], std::string{"one"});
    same(shown[2], std::string{"two"});
    ought(!showing(top.render({{0, 0}, {30, 3}}), "$"));

    // A command line given to the shell makes it busy until its next prompt.
    ought(one->idle());

    one->submit("ls");

    ought(!one->idle());

    one->send("quit\r");

    ought(awaited(loop, top, {30, 3}, [&one](const View&) { return !one->running(); }));
}

// What the program writes and its exit come on their own threads, so the exit may be known first.
// The end still comes after all it wrote, its marks included, and nothing is told after the end.
TRIAL("Terminal: the end comes after all that the program wrote") {
    Loop loop;
    Root top;

    const auto one = std::make_shared<Terminal>(pretending("farewell"), Size{60, 4});

    std::vector<Marked> marks;
    std::size_t before = 0;
    std::optional<Exit> end;

    const ui::Hook marked = one->on<MarkEvent>([&marks](MarkEvent& what) {
        marks.push_back(what.mark().kind);

        return true;
    });

    const ui::Hook heard = one->on<EndEvent>([&end, &marks, &before](EndEvent& what) {
        end = what.exit();
        before = marks.size();

        return true;
    });

    top.children().add(one);

    one->run();

    ought(awaited(loop, top, {60, 4}, [&end](const View&) { return end.has_value(); }));
    ought(end.has_value() && end->ended == Ended::normal && end->code == 0);
    ought(marks == (std::vector<Marked>{Marked::done}));
    same(before, std::size_t{1});
    ought(showing(top.render({{0, 0}, {60, 4}}), "bye"));
    ought(!one->running());
}

// A program may leave another one behind that holds the terminal open, so the end of what is
// written to it does not come. The end of the program comes anyway, soon after its exit, with what
// it wrote.
TRIAL("Terminal: a program that leaves another holding the terminal still ends") {
    Loop loop;
    Root top;

    const auto one = std::make_shared<Terminal>(pretending("forsaking"), Size{30, 3});

    std::optional<Exit> end;

    const ui::Hook heard = one->on<EndEvent>([&end](EndEvent& what) {
        end = what.exit();

        return true;
    });

    top.children().add(one);

    const auto began = std::chrono::steady_clock::now();

    one->run();

    ought(awaited(loop, top, {30, 3}, [&end](const View&) { return end.has_value(); }));
    ought(std::chrono::steady_clock::now() - began < std::chrono::seconds{2});
    ought(showing(top.render({{0, 0}, {30, 3}}), "left"));
    ought(!one->running());
}

// Shift+Page Up walks the history back and Shift+Page Down forward again; the program gets neither.
TRIAL("Terminal: Shift with the page keys walks the history") {
    Loop loop;
    Root top;

    const auto one = std::make_shared<Terminal>(pretending("tty"), Size{20, 2});

    top.children().add(one);

    one->focus();
    one->run();

    ought(awaited(loop, top, {20, 2}, [](const View& seen) { return showing(seen, "tty 1"); }));

    one->send("a\rb\rc\r");

    ought(awaited(loop, top, {20, 2}, [](const View& seen) { return showing(seen, "got:c"); }));

    KeyEvent back{Key{Code::pageup, Mods{.shift = true}}};

    ought(top.dispatch(back));
    ought(one->scrolled() > 0);

    for (int turn = 0; turn < 10; ++turn) {
        KeyEvent again{Key{Code::pagedown, Mods{.shift = true}}};

        (void)top.dispatch(again);
    }

    same(one->scrolled(), 0);

    one->send("quit\r");

    ought(awaited(loop, top, {20, 2}, [&one](const View&) { return !one->running(); }));
}

// The cursor's shape: the one the node was given while the program asks for none, the program's
// own once it asks (DECSCUSR), and the given one again when the program asks for the terminal's
// usual cursor.
TRIAL("Terminal: the cursor's shape is the program's when it asks, otherwise the one given") {
    Loop loop;
    Root top;

    const auto one = std::make_shared<Terminal>(pretending("tty"), Size{40, 5});

    one->caret_shape(CaretShape{Caret::underline, false});

    top.children().add(one);

    one->focus();
    one->run();

    ought(awaited(loop, top, {40, 5}, [](const View& seen) { return showing(seen, "tty 1 40x5"); }));
    ought(top.caret_shape() == (CaretShape{Caret::underline, false}));

    one->send("bar\r");

    ought(awaited(loop, top, {40, 5}, [](const View& seen) { return showing(seen, "barred"); }));
    ought(top.caret_shape() == (CaretShape{Caret::bar, true}));

    one->send("usual\r");

    ought(awaited(loop, top, {40, 5}, [](const View& seen) { return showing(seen, "as usual"); }));
    ought(top.caret_shape() == (CaretShape{Caret::underline, false}));

    one->send("quit\r");

    ought(awaited(loop, top, {40, 5}, [&one](const View&) { return !one->running(); }));
}
