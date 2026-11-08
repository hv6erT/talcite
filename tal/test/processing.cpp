#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/executor.hpp"
#include "tal/port.hpp"
#include "tal/process.hpp"
#include "tal/stream.hpp"
#include "tal/task.hpp"

#include "proving.hpp"

// Streams and processes of tal::core: pipes in memory, running a program to its end, its streams,
// stopping and killing it, its terminal size, programs on their own and opened ones, through the
// scripted adapter whose programs the trials write. The system's adapter of platform/ will run
// the same trials with real programs.

namespace {

using tal::Command;
using tal::Exit;
using tal::Path;

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
    same(tal::text(tal::wait(loop, reading.read(4))), std::string{"abcd"});
    same(tal::text(tal::wait(loop, reading.read(10))), std::string{"ef"});

    const auto [got, wrote] = tal::wait(loop, tal::all(reading.read(10), [](tal::Stream into) -> tal::Task<int> {
        co_await tal::to(tal::pool());
        co_await into.write(tal::bytes("!"));

        co_return 1;
    }(writing)));

    same(tal::text(got), std::string{"!"});
    same(wrote, 1);

    writing.close();

    same(tal::wait(loop, reading.read(10)).size(), std::size_t{0});
    ought(refused(loop, writing.write(tal::bytes("x"))) == tal::Kind::broken);
}

TRIAL("Stream: a closed reader breaks the writer; wrong ends, nothing held and two readers refuse") {
    tal::Loop loop;
    auto [reading, writing] = tal::detail::piped();

    ought(refused(loop, reading.write(tal::bytes("x"))) == tal::Kind::unsupported);
    ought(refused(loop, writing.read(1)) == tal::Kind::unsupported);
    ought(refused(loop, tal::Stream{}.read(1)) == tal::Kind::missing);

    tal::Scope scope{loop};
    std::atomic<int> ended{0};

    scope.run([](tal::Stream from, std::atomic<int>& into) -> tal::Task<void> {
        if ((co_await from.read(1)).empty()) ++into;
    }(reading, ended));

    for (int at = 0; at < 5; ++at) (void)loop.turn();

    ought(refused(loop, reading.read(1)) == tal::Kind::busy);

    reading.close();

    for (int at = 0; at < 5; ++at) (void)loop.turn();

    same(ended.load(), 1);
    ought(refused(loop, writing.write(tal::bytes("x"))) == tal::Kind::broken);
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

    same(all.size(), std::size_t{100003});
    same(tal::text(std::span{all}.subspan(100000)), std::string{"end"});
}

TRIAL("Process: run() writes the input, gathers output and errors, and gives the exit code") {
    tal::Loop loop;
    const auto scripted = std::make_shared<tal::Scripted>(loop);

    scripted->add("shout", shouting);
    tal::defaults().set<tal::Processes>(scripted);

    const tal::Output got = tal::wait(loop, tal::Process::run(command("/usr/bin/shout", {"-v"}), tal::bytes("poirot")));

    ought(got.exit == (Exit{Exit::Ended::normal, 3, 0}));
    same(tal::text(got.output), std::string{"POIROT"});
    same(tal::text(got.errors), std::string{"uwaga"});
    same(scripted->started().size(), std::size_t{1});
    ought(scripted->started()[0].program == Path{"/usr/bin/shout"});
    same(scripted->started()[0].arguments, (std::vector<std::string>{"-v"}));

    Command merged = command("shout");

    merged.merged = true;

    const tal::Output together = tal::wait(loop, tal::Process::run(merged, tal::bytes("a")));

    same(tal::text(together.output), std::string{"Auwaga"});
    same(together.errors.size(), std::size_t{0});

    const tal::Output opened = tal::wait(loop, tal::Process::run(command("plik.pdf", {}, tal::Mode::opened), tal::bytes("x")));

    ought(opened.exit.ended == Exit::Ended::lost);

    tal::defaults().clear<tal::Processes>();
}

TRIAL("Process: an unknown program refuses, a failing script is lost, and run() without an adapter refuses") {
    tal::Loop loop;
    tal::Scripted scripted{loop};

    scripted.add("failing", [](tal::Running&) -> tal::Task<int> {
        co_await tal::to(tal::loop());

        throw tal::Trouble{tal::Kind::broken, "zepsuty"};
    });

    ought(refused(loop, scripted.start(command("none"))) == tal::Kind::missing);

    const tal::Process child = tal::wait(loop, scripted.start(command("failing")));

    ought(tal::wait(loop, child.ended()).ended == Exit::Ended::lost);
    ought(refused(loop, tal::Process::run(command("none"))) == tal::Kind::missing);
    ought(refused(loop, tal::Process{}.ended()) == tal::Kind::missing);
}

TRIAL("Process: kill() and stop() end a waiting program, destroy its frames and end its streams") {
    tal::Loop loop;
    tal::Scripted scripted{loop};
    std::atomic<int> alive{0};

    scripted.add("wait", [&alive](tal::Running& with) { return waiting(with, alive); });

    const tal::Process killed = tal::wait(loop, scripted.start(command("wait")));
    const tal::Process stopped = tal::wait(loop, scripted.start(command("wait")));

    for (int at = 0; at < 5; ++at) (void)loop.turn();

    same(alive.load(), 2);
    ought(killed.id() != stopped.id());
    ought(killed.id() > 0);

    killed.kill();
    stopped.stop();

    const auto [first, second] = tal::wait(loop, tal::all(killed.ended(), stopped.ended()));

    ought(first == (Exit{Exit::Ended::killed, 0, 9}));
    ought(second == (Exit{Exit::Ended::killed, 0, 15}));
    ought(tal::wait(loop, killed.ended()) == first);
    same(tal::wait(loop, tal::drained(killed.output())).size(), std::size_t{0});

    for (int at = 0; at < 5; ++at) (void)loop.turn();

    same(alive.load(), 0);
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
    same(tal::text(tal::wait(loop, output.read(100))), std::string{"120x40 "});

    child.resize(100, 30);
    tal::wait(loop, input.write(tal::bytes(".")));
    same(tal::text(tal::wait(loop, output.read(100))), std::string{"100x30 "});

    child = tal::Process{};

    tal::wait(loop, input.write(tal::bytes(".")));
    same(tal::text(tal::wait(loop, output.read(100))), std::string{"100x30 "});

    input.close();
    same(tal::wait(loop, output.read(100)).size(), std::size_t{0});
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

    ought(tal::wait(loop, daemon.ended()).ended == Exit::Ended::lost);

    for (int at = 0; at < 10 && done.load() == 0; ++at) (void)loop.turn();

    same(done.load(), 1);

    const tal::Process opened = tal::wait(loop, scripted.start(command("https://example.org", {}, tal::Mode::opened)));

    same(opened.id(), std::int64_t{0});
    ought(tal::wait(loop, opened.ended()).ended == Exit::Ended::lost);
    same(tal::wait(loop, tal::drained(opened.output())).size(), std::size_t{0});
    ought(refused(loop, opened.input().write(tal::bytes("x"))) == tal::Kind::broken);
    same(scripted.started().size(), std::size_t{2});
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

        same(alive.load(), 1);
    }

    for (int at = 0; at < 5; ++at) (void)loop.turn();

    same(alive.load(), 0);
    ought(tal::wait(loop, child.ended()) == (Exit{Exit::Ended::killed, 0, 9}));
    same(tal::wait(loop, tal::drained(child.output())).size(), std::size_t{0});
}
