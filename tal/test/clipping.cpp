#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/clipboard.hpp"
#include "tal/clock.hpp"
#include "tal/console.hpp"
#include "tal/executor.hpp"
#include "tal/path.hpp"
#include "tal/process.hpp"
#include "tal/stream.hpp"
#include "tal/task.hpp"

#include "quieting.hpp"
#include "tal/testing.hpp"

// The clipboard of the machine the user sits at, on scripted programs: the session's clipboard
// programs first, the terminal's OSC 52 after them, the program's own copy for pasting, and lists
// of files as text/uri-list.

namespace {

using namespace std::chrono_literals;

using tal::Handed;
using tal::Handing;
using tal::Path;

// The clipboard programs of a session, played by the scripted shell: which are installed, what the
// clipboard holds by type, the programs run, the status they end with, whether they hang and
// whether one that hung was killed.
struct Session {
    std::set<std::string> present;
    std::map<std::string, std::string> typed;
    std::vector<std::string> ran;
    int status = 0;
    bool stuck = false;
    bool killed = false;
    tal::Pending<int> never;
};

// Marks the session's program killed when the program, hanging, goes away.
struct Hanging {
    Session& session;

    // Marks it.
    ~Hanging() { session.killed = true; }
};

// The words of `line` after `from` up to the first redirection.
std::string execd(std::string_view line, std::size_t from) {
    const std::size_t end = std::min(line.find(" >", from), line.find(" 2>", from));

    return std::string{line.substr(from, end == std::string_view::npos ? std::string_view::npos : end - from)};
}

// Whether every program `line` asks `command -v` for is present.
bool found(const Session& session, std::string_view line) {
    for (std::size_t at = line.find("command -v "); at != std::string_view::npos; at = line.find("command -v ", at + 1)) {
        const std::size_t from = at + 11;
        const std::string named{line.substr(from, line.find(' ', from) - from)};

        if (!session.present.contains(named)) return false;
    }

    return true;
}

// The scripted /bin/sh: runs the first line whose programs are present, as the program it execs
// would, or ends with the status of its `exit`.
tal::Task<int> shell(tal::Running& with, Session& session) {
    const std::string whole = with.command.arguments.at(1);
    std::size_t from = 0;

    while (from <= whole.size()) {
        const std::size_t end = std::min(whole.find('\n', from), whole.size());
        const std::string_view line = std::string_view{whole}.substr(from, end - from);

        from = end + 1;

        if (line.starts_with("exit ")) co_return std::stoi(std::string{line.substr(5)});

        const std::size_t exec = line.find("exec ");

        if (exec == std::string_view::npos || !found(session, line)) continue;

        const std::string program = execd(line, exec + 5);

        session.ran.push_back(program);

        if (session.stuck) {
            const Hanging hanging{session};

            co_return co_await session.never.task();
        }

        const std::string input = tal::text::of(co_await tal::drained(with.input));
        const std::size_t typed = program.find("-t");
        std::string type = "text/plain";

        if (const std::size_t at = program.find("--type "); at != std::string::npos) type = program.substr(at + 7, program.find(' ', at + 7) - at - 7);
        else if (typed != std::string::npos && program.starts_with("xclip")) type = program.substr(typed + 3, program.find(' ', typed + 3) - typed - 3);

        if (program.find("--clear") != std::string::npos) {
            session.typed.clear();

            co_return 0;
        }

        if (program.starts_with("wl-paste") || program.find("-out") != std::string::npos) {
            if (!session.typed.contains(type)) co_return 1;

            co_await with.output.write(tal::bytes(session.typed[type]));

            co_return session.status;
        }

        session.typed = {{type, input}};

        co_return session.status;
    }

    co_return 0;
}

// A clipboard on a scripted console and shell, on a loop with a manual clock.
struct Desk {
    std::shared_ptr<tal::ManualClock> clock = std::make_shared<tal::ManualClock>();
    tal::Loop loop{tal::Port<tal::Clock>{clock}};
    std::shared_ptr<tal::ScriptedConsole> console = std::make_shared<tal::ScriptedConsole>();
    std::shared_ptr<tal::Scripted> processes = std::make_shared<tal::Scripted>(loop);
    Session session;
    tal::Port<tal::Clipboard> clipboard = tal::clipboard(tal::Port<tal::Console>{console}, tal::Port<tal::Processes>{processes});

    // A session with `variables` set and `programs` installed.
    Desk(std::map<std::string, std::string> variables, std::set<std::string> programs) {
        for (auto& [name, value] : variables) console->environment[name] = value;

        session.present = std::move(programs);
        processes->add("sh", [this](tal::Running& with) { return shell(with, session); });
    }

    // Copies `what` and gives whether it reached a clipboard.
    bool copied(std::string what) { return tal::wait(loop, clipboard->copy(std::move(what))); }

    // What a paste gives.
    std::optional<std::string> pasted() { return tal::wait(loop, clipboard->paste()); }

    // Gives the files and tells whether they reached the system's clipboard.
    bool given(Handed what) { return tal::wait(loop, clipboard->give(std::move(what))); }

    // The files a paste of files gives.
    std::optional<Handed> taken() { return tal::wait(loop, clipboard->taken()); }
};

// Under Wayland the text goes to wl-copy, and the terminal hears nothing; a paste gives the
// program's own copy. Without wl-copy, X's xclip or xsel take it.
TRIAL("Clipboard: the session's clipboard program takes the copy") {
    Desk wayland{{{"WAYLAND_DISPLAY", "wayland-0"}, {"DISPLAY", ":0"}}, {"wl-copy", "xclip"}};

    OUGHT(wayland.copied("Poirot"));
    SAME(wayland.session.typed["text/plain"], std::string{"Poirot"});
    OUGHT(wayland.session.ran == (std::vector<std::string>{"wl-copy"}));
    SAME(wayland.console->written, std::string{});
    OUGHT(wayland.pasted() == std::optional<std::string>{"Poirot"});

    Desk x{{{"WAYLAND_DISPLAY", "wayland-0"}, {"DISPLAY", ":0"}}, {"xclip"}};

    OUGHT(x.copied("Hastings"));
    OUGHT(x.session.ran == (std::vector<std::string>{"xclip -selection clipboard -in"}));

    Desk xsel{{{"DISPLAY", ":0"}}, {"xsel"}};

    OUGHT(xsel.copied("Japp"));
    OUGHT(xsel.session.ran == (std::vector<std::string>{"xsel --clipboard --input"}));
}

// Without a clipboard program, over ssh, or when the program fails, the terminal is asked through
// OSC 52, in base64; a text too long for it reaches no clipboard, though a paste inside the program
// still gives it.
TRIAL("Clipboard: OSC 52 when no program takes it, and not past its limit") {
    Desk none{{}, {"wl-copy"}};

    OUGHT(none.copied("abc"));
    SAME(none.console->written, std::string{"\x1b]52;c;YWJj\x07"});
    OUGHT(none.processes->started().empty());

    none.console->written.clear();

    OUGHT(none.copied("a"));
    OUGHT(none.copied("ab"));
    SAME(none.console->written, std::string{"\x1b]52;c;YQ==\x07\x1b]52;c;YWI=\x07"});

    none.console->written.clear();

    const std::string long_one(64 * 1024 + 1, 'x');

    OUGHT(!none.copied(long_one));
    SAME(none.console->written, std::string{});
    OUGHT(none.pasted() == std::optional<std::string>{long_one});
    OUGHT(none.copied(std::string(64 * 1024, 'x')));

    Desk far{{{"WAYLAND_DISPLAY", "wayland-0"}, {"SSH_TTY", "/dev/pts/1"}}, {"wl-copy"}};

    OUGHT(far.copied("abc"));
    OUGHT(far.session.ran.empty());
    SAME(far.console->written, std::string{"\x1b]52;c;YWJj\x07"});

    Desk lacking{{{"DISPLAY", ":0"}}, {}};

    OUGHT(lacking.copied("abc"));
    SAME(lacking.console->written, std::string{"\x1b]52;c;YWJj\x07"});

    Desk failing{{{"WAYLAND_DISPLAY", "wayland-0"}}, {"wl-copy"}};

    failing.session.status = 1;

    OUGHT(failing.copied("abc"));
    OUGHT(failing.session.ran == (std::vector<std::string>{"wl-copy"}));
    SAME(failing.console->written, std::string{"\x1b]52;c;YWJj\x07"});

    Desk empty{{}, {}};

    OUGHT(!empty.pasted().has_value());
}

// A clipboard program that hangs is killed after a second and a half, and the terminal is asked
// instead; one that reads the clipboard is killed too, and the next way is tried.
TRIAL("Clipboard: a hanging program gives way to OSC 52") {
    Desk desk{{{"WAYLAND_DISPLAY", "wayland-0"}}, {"wl-copy"}};
    tal::Scope scope{desk.loop};
    std::optional<bool> done;

    desk.session.stuck = true;
    scope.run([](Desk& on, std::optional<bool>& into) -> tal::Task<void> { into = co_await on.clipboard->copy("abc"); }(desk, done));

    QUIETED(desk.loop);

    OUGHT(!done.has_value());

    desk.clock->advance(1499ms);

    QUIETED(desk.loop);

    OUGHT(!done.has_value());

    desk.clock->advance(1ms);

    UNTIL(desk.loop, done.has_value());

    QUIETED(desk.loop);

    OUGHT(*done);
    OUGHT(desk.session.killed);
    SAME(desk.console->written, std::string{"\x1b]52;c;YWJj\x07"});

    Desk reading{{{"WAYLAND_DISPLAY", "wayland-0"}}, {"wl-copy", "wl-paste"}};
    tal::Scope reader{reading.loop};
    std::optional<std::optional<Handed>> got;

    reading.session.stuck = true;
    reader.run([](Desk& on, std::optional<std::optional<Handed>>& into) -> tal::Task<void> { into = co_await on.clipboard->taken(); }(reading, got));

    QUIETED(reading.loop);

    reading.clock->advance(1500ms);

    QUIETED(reading.loop);

    OUGHT(reading.session.killed);
    OUGHT(!got.has_value());

    reading.clock->advance(1500ms);

    UNTIL(reading.loop, got.has_value());

    OUGHT(!got->has_value());
}

// Files go to the clipboard as text/uri-list, their paths as the text a paste gives; a paste of
// files reads them back, GNOME's mark of a cut first, unless its reader prints too much; a reader
// that fails gives nothing. A relative path refuses the whole list, and an empty list takes the
// files away.
TRIAL("Clipboard: files given and taken as a list of URIs") {
    Desk desk{{{"WAYLAND_DISPLAY", "wayland-0"}}, {"wl-copy", "wl-paste"}};
    const Handed two{{Path{"/a b/c.txt"}, Path{"/x/\xc5\xbc.txt"}}, Handing::cut};

    OUGHT(desk.given(two));
    SAME(desk.session.typed["text/uri-list"], std::string{"file:///a%20b/c.txt\r\nfile:///x/%C5%BC.txt\r\n"});
    OUGHT(desk.pasted() == std::optional<std::string>{"/a b/c.txt\n/x/\xc5\xbc.txt"});
    OUGHT(desk.taken() == std::optional<Handed>{two});

    desk.session.typed = {{"text/uri-list", "# a comment\r\nfile://localhost/one\r\nhttp://far/two\nfile://other/three\nfile:///bad%2\nfile:///nul%00\nfile:///%41%62%6a\n"}};

    OUGHT(desk.taken() == (std::optional<Handed>{Handed{{Path{"/one"}, Path{"/Abj"}}, Handing::copy}}));

    desk.session.typed = {{"x-special/gnome-copied-files", "copy\nfile:///big\n" + std::string(4 * 1024 * 1024, 'x')}, {"text/uri-list", "file:///small"}};

    OUGHT(desk.taken() == (std::optional<Handed>{Handed{{Path{"/small"}}, Handing::copy}}));

    desk.session.typed = {{"x-special/gnome-copied-files", "copy\nfile:///h"}, {"text/uri-list", "file:///i"}};
    desk.session.status = 1;

    OUGHT(!desk.taken().has_value());

    desk.session.status = 0;

    desk.session.typed = {{"x-special/gnome-copied-files", "cut\nfile:///g\n"}};

    OUGHT(desk.taken() == (std::optional<Handed>{Handed{{Path{"/g"}}, Handing::cut}}));

    desk.session.typed = {{"x-special/gnome-copied-files", "copy\nfile:///h"}, {"text/uri-list", "file:///i"}};

    OUGHT(desk.taken() == (std::optional<Handed>{Handed{{Path{"/h"}}, Handing::copy}}));

    desk.session.typed = {{"x-special/gnome-copied-files", "move\nfile:///h"}, {"text/uri-list", "file:///i"}};

    OUGHT(desk.taken() == (std::optional<Handed>{Handed{{Path{"/i"}}, Handing::copy}}));

    desk.session.typed = {{"text/plain", "words"}};

    OUGHT(!desk.taken().has_value());

    desk.session.typed = {{"text/uri-list", "not a uri"}};

    OUGHT(!desk.taken().has_value());

    OUGHT(!desk.given(Handed{{Path{"relative"}}, Handing::copy}));
    OUGHT(desk.pasted() == std::optional<std::string>{"/a b/c.txt\n/x/\xc5\xbc.txt"});

    desk.session.typed = {{"text/uri-list", "file:///kept"}};

    OUGHT(desk.given(Handed{}));
    OUGHT(desk.session.typed.empty());
    OUGHT(!desk.pasted().has_value());

    OUGHT(desk.copied("text"));
    OUGHT(desk.given(Handed{}));
    OUGHT(desk.pasted() == std::optional<std::string>{"text"});
}

// Without a clipboard program that carries types, or over ssh, files stay in the program's own
// copy: giving them reaches no clipboard, and taking them gives the own copy, until a text copied
// takes their place.
TRIAL("Clipboard: files kept in the own copy where no program carries them") {
    Desk plain{{{"DISPLAY", ":0"}}, {"xsel"}};
    const Handed one{{Path{"/one"}}, Handing::copy};

    OUGHT(!plain.given(one));
    OUGHT(plain.taken() == std::optional<Handed>{one});

    Desk far{{{"WAYLAND_DISPLAY", "wayland-0"}, {"SSH_CONNECTION", "1 2 3 4"}}, {"wl-copy", "wl-paste"}};

    OUGHT(!far.given(one));
    OUGHT(far.session.ran.empty());
    OUGHT(far.taken() == std::optional<Handed>{one});
    OUGHT(far.copied("words"));
    OUGHT(!far.taken().has_value());

    Desk x{{{"DISPLAY", ":0"}}, {"xclip"}};

    OUGHT(x.given(one));
    OUGHT(x.session.ran.back() == "xclip -selection clipboard -t text/uri-list -in");
    OUGHT(x.taken() == std::optional<Handed>{one});
}

// A scripted clipboard keeps what it is given while it takes it; a clipboard of its own kind
// carries no files; the built-in one is the system's.
TRIAL("Clipboard: the scripted one, one without files, and the built-in one") {
    tal::Loop loop;
    tal::ScriptedClipboard scripted;

    OUGHT(tal::wait(loop, scripted.copy("a")));
    OUGHT(tal::wait(loop, scripted.paste()) == std::optional<std::string>{"a"});

    scripted.takes = false;

    OUGHT(!tal::wait(loop, scripted.copy("b")));
    OUGHT(tal::wait(loop, scripted.paste()) == std::optional<std::string>{"a"});
    OUGHT(scripted.copied == (std::vector<std::string>{"a", "b"}));
    OUGHT(!tal::wait(loop, scripted.give(Handed{{Path{"/x"}}, Handing::copy})));
    OUGHT(!tal::wait(loop, scripted.taken()).has_value());

    scripted.takes = true;

    OUGHT(tal::wait(loop, scripted.give(Handed{{Path{"/x"}}, Handing::copy})));
    OUGHT(tal::wait(loop, scripted.taken()).has_value());

    // A clipboard of text only.
    class Bare : public tal::Clipboard {
    public:
        // Its name.
        std::string_view name() const override { return "bare"; }

        // Takes nothing.
        tal::Task<bool> copy(std::string what) override {
            (void)what;

            co_return false;
        }

        // Holds nothing.
        tal::Task<std::optional<std::string>> paste() override { co_return std::nullopt; }
    };

    Bare bare;

    OUGHT(!tal::wait(loop, bare.give(Handed{{Path{"/x"}}, Handing::copy})));
    OUGHT(!tal::wait(loop, bare.taken()).has_value());
    SAME(tal::Clipboard::builtin()->name(), std::string_view{"system"});
}

}
