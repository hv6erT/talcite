#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/app.hpp"
#include "tal/clock.hpp"
#include "tal/console.hpp"
#include "tal/controls.hpp"
#include "tal/document.hpp"
#include "tal/event.hpp"
#include "tal/executor.hpp"
#include "tal/fs.hpp"
#include "tal/key.hpp"
#include "tal/notify.hpp"
#include "tal/path.hpp"
#include "tal/port.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"

#include "quieting.hpp"
#include "tal/testing.hpp"

// Files dropped on the terminal: what it pastes is a drop when it names files that are all there,
// asked of the files of the defaults (here in memory); otherwise, and when nobody takes the drop,
// a paste. What comes while the files are asked waits and follows the drop in its order.

namespace {

using namespace std::chrono_literals;

using tal::Document;
using tal::Path;

// Files in memory whose looks hang while `stuck`.
class Slow : public tal::fs::Memory {
public:
    bool stuck = false;
    tal::Pending<tal::fs::Entry> never;

    using tal::fs::Memory::Memory;

    // The entry, or never while stuck.
    tal::Task<tal::fs::Entry> look(const Path& one) override {
        if (stuck) co_return co_await never.task();

        co_return co_await tal::fs::Memory::look(one);
    }
};

// A document on a loop with a manual clock, over files in memory with two files and a folder, and
// what reached it, in order.
struct Desk {
    std::shared_ptr<tal::ManualClock> clock = std::make_shared<tal::ManualClock>();
    tal::Loop loop{tal::Port<tal::Clock>{clock}};
    std::shared_ptr<Slow> files = std::make_shared<Slow>(tal::Port<tal::Clock>{clock});
    Document document{tal::Port<tal::Clock>{clock}};
    std::vector<std::string> heard;
    bool taking = true;
    tal::Hooks hooks;

    // The files made and put among the defaults, the document heard.
    Desk() {
        tal::wait(loop, files->make(Path{"/d"}));
        tal::wait(loop, files->write(Path{"/d/a b.txt"}, tal::bytes("a")));
        tal::wait(loop, files->write(Path{"/d/c.txt"}, tal::bytes("c")));
        tal::defaults().set<tal::fs::Files>(files);

        hooks.add(document.on<tal::DropEvent>([this](tal::DropEvent& one) {
            std::string said = "drop";

            for (const Path& each : one.paths) said += " " + std::string{each.stored()};

            heard.push_back(said + " [" + one.text + "]");

            if (taking) one.take();
        }));
        hooks.add(document.on<tal::PasteEvent>([this](tal::PasteEvent& one) { heard.push_back("paste [" + one.text + "]"); }));
        hooks.add(document.on<tal::KeyEvent>([this](tal::KeyEvent& one) { heard.push_back(std::string{"key "} + static_cast<char>(one.key.letter())); }));
        hooks.add(document.on<tal::MouseEvent>([this](tal::MouseEvent& one) {
            std::string said = "mouse";

            if (one.press == tal::Press::click) said = "click";
            if (one.press == tal::Press::double_click) said = "double click";
            if (one.press == tal::Press::wheel) said = "wheel " + std::to_string(one.steps) + (one.axis == tal::Axis::horizontal ? " across" : " down");
            if (one.mods.ctrl) said += " with ctrl";

            heard.push_back(said);
        }));
        hooks.add(document.on<tal::WindowFocused>([this](tal::WindowFocused&) { heard.push_back("focused"); }));
        hooks.add(document.on<tal::WindowBlurred>([this](tal::WindowBlurred&) { heard.push_back("blurred"); }));
    }

    // The defaults without the files.
    ~Desk() { tal::defaults().clear<tal::fs::Files>(); }

    Desk(const Desk&) = delete;
    Desk& operator=(const Desk&) = delete;

    // Lets the loop go until it is quiet.
    void settled() { QUIETED(loop); }
};

}

// Files that are all there make a drop with their paths and the text; one missing, or text that
// names no files, makes a paste; a drop nobody takes is a paste too.
TRIAL("Dropping: a drop of files that are there, a paste otherwise") {
    Desk desk;

    desk.document.paste("\"/d/a b.txt\" /d/c.txt /d ");
    desk.settled();

    OUGHT(desk.heard == (std::vector<std::string>{"drop /d/a b.txt /d/c.txt /d [\"/d/a b.txt\" /d/c.txt /d ]"}));

    desk.heard.clear();
    desk.document.paste("/d/c.txt /d/gone.txt");
    desk.settled();

    OUGHT(desk.heard == (std::vector<std::string>{"paste [/d/c.txt /d/gone.txt]"}));

    desk.heard.clear();
    desk.document.paste("hello world");

    OUGHT(desk.heard == (std::vector<std::string>{"paste [hello world]"}));

    desk.heard.clear();
    desk.taking = false;
    desk.document.paste("file:///d/c.txt\r\n");
    desk.settled();

    OUGHT(desk.heard == (std::vector<std::string>{"drop /d/c.txt [file:///d/c.txt\r\n]", "paste [file:///d/c.txt\r\n]"}));
}

// Keys, the mouse, pastes and the window's focus that come while the files are asked wait, and
// follow the drop in their order; a drop among them is asked in its turn, and what came after it
// waits for it again.
TRIAL("Dropping: what comes meanwhile waits for the drop") {
    Desk desk;

    desk.document.paste("/d/c.txt");
    desk.document.key(tal::Key{U'a'});
    desk.document.mouse(tal::Press::move, tal::MouseButton::none, {0, 0});
    desk.document.window(false);
    desk.document.paste("\"/d/a b.txt\"");
    desk.document.key(tal::Key{U'b'});
    desk.document.paste("plain");
    desk.document.window(true);

    OUGHT(desk.heard.empty());

    desk.settled();

    OUGHT(desk.heard == (std::vector<std::string>{"drop /d/c.txt [/d/c.txt]", "key a", "mouse", "blurred", "drop /d/a b.txt [\"/d/a b.txt\"]",
                                                   "key b", "paste [plain]", "focused"}));

    desk.heard.clear();
    desk.document.key(tal::Key{U'c'});

    OUGHT(desk.heard == (std::vector<std::string>{"key c"}));
}

// Files that do not answer within a second and a half make a paste, and what waited follows it.
TRIAL("Dropping: files that hang make a paste in time") {
    Desk desk;

    desk.files->stuck = true;
    desk.document.paste("/d/c.txt");
    desk.document.key(tal::Key{U'a'});
    desk.settled();

    OUGHT(desk.heard.empty());

    desk.clock->advance(1499ms);
    desk.settled();

    OUGHT(desk.heard.empty());

    desk.clock->advance(1ms);
    desk.settled();

    OUGHT(desk.heard == (std::vector<std::string>{"paste [/d/c.txt]", "key a"}));
}

// A document with no loop running to ask on takes what names files as a paste.
TRIAL("Dropping: without a loop the drop is a paste") {
    Document alone{tal::Port<tal::Clock>{std::make_shared<tal::ManualClock>()}};
    std::vector<std::string> heard;
    tal::Hooks hooks;

    THREW((void)tal::loop());

    hooks.add(alone.on<tal::PasteEvent>([&heard](tal::PasteEvent& one) { heard.push_back(one.text); }));
    hooks.add(alone.on<tal::DropEvent>([&heard](tal::DropEvent&) { heard.push_back("drop"); }));
    alone.paste("/d/c.txt");

    OUGHT(heard == (std::vector<std::string>{"/d/c.txt"}));
}

// The drop goes to the node with the focus, and a field there takes it as the text the terminal
// wrote, before the nodes above see it; the window's focus goes to the document itself.
TRIAL("Dropping: a field takes a drop as text, the window's focus is the document's") {
    Desk desk;
    tal::Field& field = desk.document.children().add<tal::Field>();
    std::vector<std::string> beside;
    tal::Hooks own;

    own.add(field.on<tal::WindowFocused>([&beside](tal::WindowFocused&) { beside.push_back("field heard the window"); }));
    desk.document.display(tal::Display{.screen = {20, 3}});
    (void)desk.document.frame();
    field.focus();

    OUGHT(field.focused());

    desk.document.paste("/d/c.txt");
    desk.settled();
    desk.document.window(true);

    SAME(*field.text, std::string{"/d/c.txt"});
    OUGHT(desk.heard == (std::vector<std::string>{"focused"}));
    OUGHT(beside.empty());

    (void)desk.document.children().remove(field);
    desk.document.paste("/d/c.txt");
    desk.settled();

    OUGHT(desk.heard.back() == "drop /d/c.txt [/d/c.txt]");
}

// The mouse waits with all it said and the moment it came: two clicks a second apart, both waiting
// for a drop, stay two clicks, and the wheel keeps its steps, its axis and its keys.
TRIAL("Dropping: the mouse that waited comes as it was") {
    Desk desk;

    desk.document.paste("/d/c.txt");
    desk.document.mouse(tal::Press::down, tal::MouseButton::left, {1, 0});
    desk.document.mouse(tal::Press::up, tal::MouseButton::left, {1, 0});
    desk.clock->advance(1000ms);
    desk.document.mouse(tal::Press::down, tal::MouseButton::left, {1, 0});
    desk.document.mouse(tal::Press::up, tal::MouseButton::left, {1, 0});
    desk.document.mouse(tal::Press::wheel, tal::MouseButton::none, {0, 0}, tal::Mods{.ctrl = true}, 3, tal::Axis::horizontal);
    desk.settled();

    OUGHT(desk.heard == (std::vector<std::string>{"drop /d/c.txt [/d/c.txt]", "mouse", "mouse", "click", "mouse", "mouse", "click",
                                                   "wheel 3 across with ctrl"}));
}

// What a handler of the drop does at once comes at once, before what waited; what waited and was
// left by a handler that threw comes first with the next input.
TRIAL("Dropping: a handler's own input, and a handler that throws") {
    Desk desk;
    bool throwing = false;
    tal::Hooks own;

    desk.taking = false;
    own.add(desk.document.on<tal::DropEvent>([&desk, &throwing](tal::DropEvent& one) {
        if (throwing) throw tal::Trouble{tal::Kind::other, "thrown"};

        one.take();
        desk.document.key(tal::Key{U'z'});
    }));
    desk.document.paste("/d/c.txt");
    desk.document.key(tal::Key{U'a'});
    desk.settled();

    OUGHT(desk.heard == (std::vector<std::string>{"drop /d/c.txt [/d/c.txt]", "key z", "key a"}));

    desk.heard.clear();
    throwing = true;
    desk.document.paste("/d/c.txt");
    desk.document.key(tal::Key{U'b'});

    THREW(desk.settled());

    desk.heard.clear();
    throwing = false;
    desk.document.key(tal::Key{U'c'});

    OUGHT(desk.heard == (std::vector<std::string>{"key b", "key c"}));
}

// A document that goes while its drop is checked leaves nothing behind that would reach it.
TRIAL("Dropping: a document gone during the check") {
    Desk desk;
    auto alone = std::make_unique<Document>(tal::Port<tal::Clock>{desk.clock});
    int heard = 0;
    tal::Hooks own;

    own.add(alone->on<tal::DropEvent>([&heard](tal::DropEvent&) { ++heard; }));
    own.add(alone->on<tal::PasteEvent>([&heard](tal::PasteEvent&) { ++heard; }));
    alone->paste("/d/c.txt");
    alone->key(tal::Key{U'a'});
    alone.reset();
    desk.settled();
    desk.clock->advance(2s);
    desk.settled();

    SAME(heard, 0);
}

// A document that goes at any step of a check whose files answered leaves nothing behind either:
// the answer it would have heard is dropped.
TRIAL("Dropping: a document gone at any step of the answer") {
    for (int steps = 0; steps < 8; ++steps) {
        Desk desk;
        auto alone = std::make_unique<Document>(tal::Port<tal::Clock>{desk.clock});
        int heard = 0;
        tal::Hooks own;

        own.add(alone->on<tal::DropEvent>([&heard](tal::DropEvent& one) {
            ++heard;
            one.take();
        }));
        own.add(alone->on<tal::PasteEvent>([&heard](tal::PasteEvent&) { ++heard; }));
        desk.files->stuck = true;
        alone->paste("/d/c.txt");
        desk.files->never.settle(tal::fs::Entry{});

        for (int turn = 0; turn < steps; ++turn) (void)desk.loop.turn();

        const int before = heard;

        alone.reset();
        desk.settled();
        desk.clock->advance(2s);
        desk.settled();

        SAME(heard, before);
        OUGHT(heard <= 1);
    }
}

// An application checks the drops of the document it shows on its own loop, even when the program's
// loop is another one, made first, that nobody turns: the bracketed paste of the terminal comes as
// a drop at the application's turns. A document shown no more lets go of the loop, and a drop it
// was checking comes as a paste first at its next input.
TRIAL("Dropping: on the application's loop") {
    Desk desk;
    auto console = std::make_shared<tal::ScriptedConsole>();
    auto shown = std::make_unique<tal::App>(tal::Setup{}, tal::Port<tal::Console>{console}, tal::Port<tal::Clock>{desk.clock});

    shown->show(desk.document);
    console->replies.push_back("\x1b[200~/d/c.txt\x1b[201~");

    for (int turn = 0; turn < 4; ++turn) (void)shown->turn();

    OUGHT(desk.heard == (std::vector<std::string>{"drop /d/c.txt [/d/c.txt]"}));

    desk.heard.clear();
    desk.files->stuck = true;
    console->replies.push_back("\x1b[200~/d/c.txt\x1b[201~a");

    for (int turn = 0; turn < 4; ++turn) (void)shown->turn();

    OUGHT(desk.heard.empty());

    shown.reset();
    desk.files->stuck = false;
    desk.document.key(tal::Key{U'b'});

    OUGHT(desk.heard == (std::vector<std::string>{"paste [/d/c.txt]", "key a", "key b"}));
}
