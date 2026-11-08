#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <thread>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/executor.hpp"
#include "tal/port.hpp"
#include "tal/process.hpp"
#include "tal/stream.hpp"
#include "tal/task.hpp"

#include "tal/testing.hpp"

// Streams and processes of tal::core: pipes in memory, running a program to its end, its streams,
// stopping and killing it, its terminal size, programs on their own and opened ones, through the
// scripted adapter whose programs the trials write. The system's adapter of platform/ will run
// the same trials with real programs.

namespace {

using tal::Command;
using tal::Exit;
using tal::Path;

using namespace std::chrono_literals;

// Counts the frames that are alive, as in the trials of tasks.
struct Sentinel {
    std::atomic<int>& alive;

    // One more frame alive.
    explicit Sentinel(std::atomic<int>& counter) : alive(counter) { ++alive; }

    // One frame fewer.
    ~Sentinel() { --alive; }

    Sentinel(const Sentinel&) = delete;
    Sentinel& operator=(const Sentinel&) = delete;
};

// A command for a program with the given arguments.
Command command(std::string_view program, std::vector<std::string> arguments = {}, tal::Mode mode = tal::Mode::piped) {
    Command made;

    made.program = Path{program};
    made.arguments = std::move(arguments);
    made.mode = mode;

    return made;
}

// A script that reads its input to the end, writes it back in capitals with a warning on its
// errors, and ends with code 3.
tal::Task<int> shouting(tal::Running& with) {
    tal::Bytes all = co_await tal::drained(with.input);

    for (std::byte& one : all)
        if (one >= std::byte{'a'} && one <= std::byte{'z'}) one = static_cast<std::byte>(static_cast<int>(one) - 32);

    co_await with.output.write(std::move(all));
    co_await with.errors.write(tal::bytes("uwaga"));

    co_return 3;
}

// A script that waits for input that never comes, keeping a sentinel in its frame.
tal::Task<int> waiting(tal::Running& with, std::atomic<int>& alive) {
    const Sentinel here{alive};

    (void)co_await with.input.read(1);

    co_return 0;
}

// The kind of trouble a task fails with, or none when it does not fail.
template <typename T>
std::optional<tal::Kind> refused(tal::Loop& loop, tal::Task<T> work) {
    try {
        (void)tal::wait(loop, std::move(work));
    } catch (const tal::Trouble& why) {
        return why.kind();
    }

    return std::nullopt;
}

}

TRIAL("Stream: a pipe gives what was written in order, waits for more and ends when closed") {
    tal::Loop loop;
    auto [reading, writing] = tal::detail::piped();

    tal::wait(loop, writing.write(tal::bytes("abc")));
    tal::wait(loop, writing.write(tal::bytes("def")));
    SAME(tal::text::of(tal::wait(loop, reading.read(4))), std::string{"abcd"});
    SAME(tal::text::of(tal::wait(loop, reading.read(10))), std::string{"ef"});

    const auto [got, wrote] = tal::wait(loop, tal::all(reading.read(10), [](tal::Stream into) -> tal::Task<int> {
        co_await tal::to(tal::pool());
        co_await into.write(tal::bytes("!"));

        co_return 1;
    }(writing)));

    SAME(tal::text::of(got), std::string{"!"});
    SAME(wrote, 1);

    writing.close();

    SAME(tal::wait(loop, reading.read(10)).size(), std::size_t{0});
    OUGHT(refused(loop, writing.write(tal::bytes("x"))) == tal::Kind::broken);
}

TRIAL("Stream: a closed reader breaks the writer; wrong ends, nothing held and two readers refuse") {
    tal::Loop loop;
    auto [reading, writing] = tal::detail::piped();

    OUGHT(refused(loop, reading.write(tal::bytes("x"))) == tal::Kind::unsupported);
    OUGHT(refused(loop, writing.read(1)) == tal::Kind::unsupported);
    OUGHT(refused(loop, tal::Stream{}.read(1)) == tal::Kind::missing);

    tal::Scope scope{loop};
    std::atomic<int> ended{0};

    scope.run([](tal::Stream from, std::atomic<int>& into) -> tal::Task<void> {
        if ((co_await from.read(1)).empty()) ++into;
    }(reading, ended));

    for (int at = 0; at < 5; ++at) (void)loop.turn();

    OUGHT(refused(loop, reading.read(1)) == tal::Kind::busy);

    reading.close();

    for (int at = 0; at < 5; ++at) (void)loop.turn();

    SAME(ended.load(), 1);
    OUGHT(refused(loop, writing.write(tal::bytes("x"))) == tal::Kind::broken);
}

TRIAL("Stream: drained() gathers everything until the end, also from a writer that went away") {
    tal::Loop loop;
    tal::Stream reading;

    {
        auto [from, into] = tal::detail::piped();

        reading = from;
        tal::wait(loop, into.write(tal::bytes(std::string(100000, 'x'))));
        tal::wait(loop, into.write(tal::bytes("end")));
    }

    const tal::Bytes all = tal::wait(loop, tal::drained(reading));

    SAME(all.size(), std::size_t{100003});
    SAME(tal::text::of(std::span{all}.subspan(100000)), std::string{"end"});
}

TRIAL("Process: run() writes the input, gathers output and errors, and gives the exit code") {
    tal::Loop loop;
    const auto scripted = std::make_shared<tal::Scripted>(loop);

    scripted->add("shout", shouting);
    tal::defaults().set<tal::Processes>(scripted);

    const tal::Output got = tal::wait(loop, tal::Process::run(command("/usr/bin/shout", {"-v"}), tal::bytes("poirot")));

    OUGHT(got.exit == (Exit{Exit::Ended::normal, 3, 0}));
    SAME(tal::text::of(got.output), std::string{"POIROT"});
    SAME(tal::text::of(got.errors), std::string{"uwaga"});
    SAME(scripted->started().size(), std::size_t{1});
    OUGHT(scripted->started()[0].program == Path{"/usr/bin/shout"});
    SAME(scripted->started()[0].arguments, (std::vector<std::string>{"-v"}));

    Command merged = command("shout");

    merged.merged = true;

    const tal::Output together = tal::wait(loop, tal::Process::run(merged, tal::bytes("a")));

    SAME(tal::text::of(together.output), std::string{"Auwaga"});
    SAME(together.errors.size(), std::size_t{0});

    const tal::Output opened = tal::wait(loop, tal::Process::run(command("plik.pdf", {}, tal::Mode::opened), tal::bytes("x")));

    OUGHT(opened.exit.ended == Exit::Ended::lost);

    tal::defaults().clear<tal::Processes>();
}

TRIAL("Process: an unknown program refuses, a failing script is lost, and run() without an adapter refuses") {
    tal::Loop loop;
    tal::Scripted scripted{loop};

    scripted.add("failing", [](tal::Running&) -> tal::Task<int> {
        co_await tal::to(tal::loop());

        throw tal::Trouble{tal::Kind::broken, "zepsuty"};
    });

    OUGHT(refused(loop, scripted.start(command("none"))) == tal::Kind::missing);

    const tal::Process child = tal::wait(loop, scripted.start(command("failing")));

    OUGHT(tal::wait(loop, child.ended()).ended == Exit::Ended::lost);
    OUGHT(refused(loop, tal::Process::run(command("none"))) == tal::Kind::missing);
    OUGHT(refused(loop, tal::Process{}.ended()) == tal::Kind::missing);
}

TRIAL("Process: kill() and stop() end a waiting program, destroy its frames and end its streams") {
    tal::Loop loop;
    tal::Scripted scripted{loop};
    std::atomic<int> alive{0};

    scripted.add("wait", [&alive](tal::Running& with) { return waiting(with, alive); });

    const tal::Process killed = tal::wait(loop, scripted.start(command("wait")));
    const tal::Process stopped = tal::wait(loop, scripted.start(command("wait")));

    for (int at = 0; at < 5; ++at) (void)loop.turn();

    SAME(alive.load(), 2);
    OUGHT(killed.id() != stopped.id());
    OUGHT(killed.id() > 0);

    killed.kill();
    stopped.stop();

    const auto [first, second] = tal::wait(loop, tal::all(killed.ended(), stopped.ended()));

    OUGHT(first == (Exit{Exit::Ended::killed, 0, 9}));
    OUGHT(second == (Exit{Exit::Ended::killed, 0, 15}));
    OUGHT(tal::wait(loop, killed.ended()) == first);
    SAME(tal::wait(loop, tal::drained(killed.output())).size(), std::size_t{0});

    for (int at = 0; at < 5; ++at) (void)loop.turn();

    SAME(alive.load(), 0);
}

TRIAL("Process: a program in a terminal sees its size change; letting the process go does not end it") {
    tal::Loop loop;
    tal::Scripted scripted{loop};

    scripted.add("size", [](tal::Running& with) -> tal::Task<int> {
        for (;;) {
            if ((co_await with.input.read(1)).empty()) co_return 0;

            co_await with.output.write(tal::bytes(std::to_string(with.columns.load()) + "x" + std::to_string(with.rows.load()) + " "));
        }
    });

    Command terminal = command("size", {}, tal::Mode::terminal);

    terminal.columns = 120;
    terminal.rows = 40;

    tal::Process child = tal::wait(loop, scripted.start(terminal));
    const tal::Stream input = child.input();
    const tal::Stream output = child.output();

    tal::wait(loop, input.write(tal::bytes(".")));
    SAME(tal::text::of(tal::wait(loop, output.read(100))), std::string{"120x40 "});

    child.resize(100, 30);
    tal::wait(loop, input.write(tal::bytes(".")));
    SAME(tal::text::of(tal::wait(loop, output.read(100))), std::string{"100x30 "});

    child = tal::Process{};

    tal::wait(loop, input.write(tal::bytes(".")));
    SAME(tal::text::of(tal::wait(loop, output.read(100))), std::string{"100x30 "});

    input.close();
    SAME(tal::wait(loop, output.read(100)).size(), std::size_t{0});
}

TRIAL("Process: a detached program runs on its own and an opened one needs no script; both are lost") {
    tal::Loop loop;
    tal::Scripted scripted{loop};
    std::atomic<int> done{0};

    scripted.add("daemon", [&done](tal::Running& with) -> tal::Task<int> {
        co_await tal::to(tal::loop());
        co_await with.output.write(tal::bytes("nobody reads this"));

        ++done;

        co_return 0;
    });

    const tal::Process daemon = tal::wait(loop, scripted.start(command("daemon", {}, tal::Mode::detached)));

    OUGHT(tal::wait(loop, daemon.ended()).ended == Exit::Ended::lost);

    for (int at = 0; at < 10 && done.load() == 0; ++at) (void)loop.turn();

    SAME(done.load(), 1);

    const tal::Process opened = tal::wait(loop, scripted.start(command("https://example.org", {}, tal::Mode::opened)));

    SAME(opened.id(), std::int64_t{0});
    OUGHT(tal::wait(loop, opened.ended()).ended == Exit::Ended::lost);
    SAME(tal::wait(loop, tal::drained(opened.output())).size(), std::size_t{0});
    OUGHT(refused(loop, opened.input().write(tal::bytes("x"))) == tal::Kind::broken);
    SAME(scripted.started().size(), std::size_t{2});
}

TRIAL("Process: the scripted adapter going away ends every program it still runs") {
    tal::Loop loop;
    std::atomic<int> alive{0};
    tal::Process child;

    {
        tal::Scripted scripted{loop};

        scripted.add("wait", [&alive](tal::Running& with) { return waiting(with, alive); });
        child = tal::wait(loop, scripted.start(command("wait")));

        for (int at = 0; at < 5; ++at) (void)loop.turn();

        SAME(alive.load(), 1);
    }

    for (int at = 0; at < 5; ++at) (void)loop.turn();

    SAME(alive.load(), 0);
    OUGHT(tal::wait(loop, child.ended()) == (Exit{Exit::Ended::killed, 0, 9}));
    SAME(tal::wait(loop, tal::drained(child.output())).size(), std::size_t{0});
}

namespace {

// The trial program itself pretending to be another one (`--pretending`), so that the trials of
// the system's processes need no shell, no echo and no cmd on the machine.
Command pretended(std::vector<std::string> what, tal::Mode mode = tal::Mode::piped) {
    what.insert(what.begin(), "--pretending");

    Command made = command({}, std::move(what), mode);

    made.program = tal::testing::mine();

    return made;
}

// A program run to its end by the system's adapter, the built-in default of the port.
tal::Output ran(tal::Loop& loop, Command what, tal::Bytes input = {}) {
    return tal::wait(loop, tal::Process::run(std::move(what), std::move(input)));
}

// A program started by the system's adapter.
tal::Process started(tal::Loop& loop, Command what) { return tal::wait(loop, tal::Process::start(std::move(what))); }

}

TRIAL("System: run() gives what a program writes on its output and errors, and its exit code") {
    tal::Loop loop;

    OUGHT(tal::Port<tal::Processes>{}->name() == "system");

    const tal::Output said = ran(loop, pretended({"said", "Poirot"}));

    OUGHT(said.exit == (Exit{Exit::Ended::normal, 0, 0}));
    SAME(tal::text::of(said.output), std::string{"Poirot"});
    SAME(said.errors.size(), std::size_t{0});
    SAME(tal::text::of(ran(loop, pretended({"moaned", "zawiodło"})).errors), std::string{"zawiodło"});
    SAME(ran(loop, pretended({"code", "7"})).exit.code, 7);

    Command both = pretended({"both"});

    both.merged = true;

    const tal::Output merged = ran(loop, both);

    OUGHT(tal::text::of(merged.output).find("output") != std::string::npos);
    OUGHT(tal::text::of(merged.output).find("error") != std::string::npos);
    SAME(merged.errors.size(), std::size_t{0});
}

// The input goes whole and the outputs come whole, read both at once: a program that writes more
// than a pipe holds to both would otherwise stop and never end.
TRIAL("System: run() feeds the input and reads both outputs at once, byte for byte") {
    tal::Loop loop;
    const std::string given = std::string{"zażółć\0\xFF", sizeof("zażółć\0\xFF") - 1} + std::string(200000, 'x');

    SAME(tal::text::of(ran(loop, pretended({"echo"}), tal::bytes(given)).output), given);

    const tal::Output much = ran(loop, pretended({"much"}));

    SAME(much.output.size(), std::size_t{300000});
    SAME(much.errors.size(), std::size_t{300000});
}

// Arguments reach the program as they were given, also with spaces, quotes and nothing at all,
// which Windows has to put together into one line and the program to take apart again.
TRIAL("System: arguments, the folder and the environment reach the program as given") {
    tal::Loop loop;
    const std::vector<std::string> odd{"a b", "c\"d", "", "e\\", "zażółć"};
    std::vector<std::string> asked{"args"};

    asked.insert(asked.end(), odd.begin(), odd.end());

    std::string wanted;

    for (const std::string& one : odd) wanted += one + "\n";

    SAME(tal::text::of(ran(loop, pretended(asked)).output), wanted);

    const std::filesystem::path folder = std::filesystem::canonical(std::filesystem::temp_directory_path());
    Command where = pretended({"where"});

    where.folder = Path{folder.generic_string()};

    OUGHT((Path{tal::text::of(ran(loop, where).output)}.same)(where.folder));

    Command around = pretended({"around", "TAL_TRIAL_SAID"});

    around.environment.push_back({"TAL_TRIAL_SAID", "inspektor"});

    SAME(tal::text::of(ran(loop, around).output), std::string{"inspektor"});

    Command gone = pretended({"around", "PATH"});

    gone.environment.push_back({"PATH", std::nullopt});

    SAME(tal::text::of(ran(loop, gone).output), std::string{"<none>"});
}

TRIAL("System: a program that is not there refuses, a missing folder too") {
    tal::Loop loop;

    OUGHT(refused(loop, tal::Process::start(command("/nie/ma/takiego/programu"))) == tal::Kind::missing);

    Command nowhere = pretended({"where"});

    nowhere.folder = Path{"/nie/ma/takiego/folderu"};

    OUGHT(refused(loop, tal::Process::start(nowhere)).has_value());
}

// Ending a program: killing it at once, or asking it to end, with its group (its job on Windows).
TRIAL("System: kill() and stop() end a program that sleeps, and ended() tells how, to every waiter") {
    tal::Loop loop;
    const tal::Process killed = started(loop, pretended({"sleeping", "20000"}));
    const tal::Process stopped = started(loop, pretended({"sleeping", "20000"}));

    OUGHT(killed.id() > 0);
    OUGHT(killed.id() != stopped.id());

    killed.kill();
    stopped.stop();

    const auto [first, second] = tal::wait(loop, tal::all(killed.ended(), stopped.ended()));

#ifdef _WIN32
    OUGHT(first.ended != Exit::Ended::lost);
    OUGHT(second.ended != Exit::Ended::lost);
#else
    OUGHT(first == (Exit{Exit::Ended::killed, 0, 9}));
    OUGHT(second == (Exit{Exit::Ended::killed, 0, 15}));
#endif

    OUGHT(tal::wait(loop, killed.ended()) == first);
    SAME(tal::wait(loop, tal::drained(killed.output())).size(), std::size_t{0});
}

// A program that will not end when asked is killed `patience` later, and two tasks that wait for
// its end both learn it.
TRIAL("System: stop() kills a program that ignores it three seconds later") {
    tal::Loop loop;
    const tal::Process stubborn = started(loop, pretended({"stubborn", "20000"}));

    // Its "ready" says it ignores the request already.
    SAME(tal::text::of(tal::wait(loop, tal::within(5000ms, stubborn.output().read(6)))), std::string{"ready\n"});

    const auto since = std::chrono::steady_clock::now();

    stubborn.stop();

    const auto [first, second] = tal::wait(loop, tal::all(stubborn.ended(), stubborn.ended()));
    const auto took = std::chrono::steady_clock::now() - since;

    OUGHT(first == second);
    OUGHT(took >= 2500ms);
    OUGHT(took < 10s);
#ifndef _WIN32
    OUGHT(first == (Exit{Exit::Ended::killed, 0, 9}));
#endif
}

// A read that waits for a program that says nothing is woken by the cancellation of its task at
// once, by the bell of the io thread, not when the program next speaks.
TRIAL("System: cancelling a task that waits for a silent program's output ends the wait at once") {
    tal::Loop loop;
    const tal::Process silent = started(loop, pretended({"sleeping", "20000"}));
    std::atomic<int> alive{0};

    {
        tal::Scope scope{loop};

        scope.run([](tal::Stream from, std::atomic<int>& counter) -> tal::Task<void> {
            const Sentinel here{counter};

            (void)co_await from.read(10);
        }(silent.output(), alive));

        for (int at = 0; at < 5; ++at) (void)loop.turn();

        // Time for the read to reach the io pool and block in the system there.
        std::this_thread::sleep_for(300ms);

        SAME(alive.load(), 1);
    }

    const auto since = std::chrono::steady_clock::now();

    while (alive.load() > 0 && std::chrono::steady_clock::now() - since < std::chrono::seconds{5}) {
        if (!loop.turn()) std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    SAME(alive.load(), 0);
    OUGHT(std::chrono::steady_clock::now() - since < std::chrono::seconds{2});

    silent.kill();
    (void)tal::wait(loop, silent.ended());
}

// A program in a terminal of its own sees a terminal of the size given, and its new size.
TRIAL("System: a program in a terminal sees one, of the size given and then of the new one") {
    tal::Loop loop;
    Command typed = pretended({"tty"}, tal::Mode::terminal);

    typed.columns = 120;
    typed.rows = 40;

    const tal::Process child = started(loop, typed);
    std::string heard;

    const auto until = [&](std::string_view wanted) {
        while (heard.find(wanted) == std::string::npos) {
            const tal::Bytes more = tal::wait(loop, tal::within(5000ms, child.output().read(4096)));

            if (more.empty()) return;

            heard += tal::text::of(more);
        }
    };

    until("tty 1 120x40");
    OUGHT(heard.find("tty 1 120x40") != std::string::npos);

    child.resize(100, 30);
    tal::wait(loop, child.input().write(tal::bytes("size\r\n")));
    until("size 100x30");
    OUGHT(heard.find("size 100x30") != std::string::npos);

    tal::wait(loop, child.input().write(tal::bytes("quit\r\n")));
    OUGHT(tal::wait(loop, tal::within(5000ms, child.ended())).ended == Exit::Ended::normal);
}

// A program let loose is nobody's child: lost to us at once, with a number of its own.
TRIAL("System: a program let loose has a number and is lost at once") {
    tal::Loop loop;
    const tal::Process loose = started(loop, pretended({"sleeping", "100"}, tal::Mode::detached));

    OUGHT(loose.id() > 0);
    OUGHT(tal::wait(loop, loose.ended()).ended == Exit::Ended::lost);
    SAME(tal::wait(loop, tal::drained(loose.output())).size(), std::size_t{0});
}
