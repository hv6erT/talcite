#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "tal/bytes.hpp"
#include "tal/clock.hpp"
#include "tal/executor.hpp"
#include "tal/process.hpp"
#include "tal/task.hpp"
#include "tal/testing.hpp"

// The runner of `tal::testing` and the waiting it gives the trials: a turn of the loop that waits
// for work by a deadline, `UNTIL` on top of it, and the choosing of trials from the command line.

using namespace std::chrono_literals;

namespace {

// This binary started again with the given arguments and run to its end.
tal::Output again(std::vector<std::string> arguments) {
    tal::Loop loop;
    tal::Command made;

    made.program = tal::testing::mine();
    made.arguments = std::move(arguments);

    return tal::wait(loop, tal::Process::run(made));
}

}

// With nothing to do the turn waits until the deadline and says that no work ran.
TRIAL("Running: a turn with a deadline waits for it when nothing comes") {
    tal::Loop loop;

    const auto began = std::chrono::steady_clock::now();

    OUGHT(!loop.turn(began + 60ms));
    OUGHT(std::chrono::steady_clock::now() - began >= 60ms);
}

// Work posted from another thread ends the wait at once, long before the deadline.
TRIAL("Running: a turn with a deadline wakes for work posted from another thread") {
    tal::Loop loop;
    bool ran = false;

    std::thread poster{[&] {
        std::this_thread::sleep_for(20ms);
        loop.post([&] { ran = true; });
    }};

    const auto began = std::chrono::steady_clock::now();
    const bool worked = loop.turn(began + 30s);

    poster.join();

    OUGHT(worked);
    OUGHT(ran);
    OUGHT(std::chrono::steady_clock::now() - began < 10s);
}

// A timer that falls due before the deadline ends the wait and runs in the same turn.
TRIAL("Running: a turn with a deadline runs a timer falling due before it") {
    tal::Loop loop;
    bool rang = false;

    (void)loop.wake(std::chrono::steady_clock::now() + 30ms, [&] { rang = true; });

    const auto began = std::chrono::steady_clock::now();

    OUGHT(loop.turn(began + 30s));
    OUGHT(rang);
    OUGHT(std::chrono::steady_clock::now() - began < 10s);
}

// On a manual clock time moves only by hand, so a timer never ends the wait: the deadline does.
TRIAL("Running: on a manual clock a turn with a deadline waits past the timers") {
    auto manual = std::make_shared<tal::ManualClock>();
    tal::Loop loop{tal::Port<tal::Clock>{manual}};
    bool rang = false;

    (void)loop.wake(manual->now() + 1ms, [&] { rang = true; });

    const auto began = std::chrono::steady_clock::now();

    OUGHT(!loop.turn(began + 60ms));
    OUGHT(!rang);
    OUGHT(std::chrono::steady_clock::now() - began >= 60ms);

    manual->advance(1ms);

    const auto moved = std::chrono::steady_clock::now();

    OUGHT(loop.turn(moved + 30s));
    OUGHT(rang);
    OUGHT(std::chrono::steady_clock::now() - moved < 10s);
}

// `UNTIL` turns the loop until work posted from another thread makes the condition true.
TRIAL("Running: UNTIL waits for work that comes through the loop") {
    tal::Loop loop;
    int seen = 0;

    std::thread poster{[&] {
        for (int at = 0; at < 3; ++at) {
            std::this_thread::sleep_for(5ms);
            loop.post([&] { ++seen; });
        }
    }};

    UNTIL(loop, seen == 3);

    poster.join();

    SAME(seen, 3);
}

// A condition that another thread makes true without the loop is seen too, as the wait is cut
// into short slices.
TRIAL("Running: UNTIL sees a condition that changes without the loop") {
    tal::Loop loop;
    std::atomic<bool> set{false};

    std::thread setter{[&] {
        std::this_thread::sleep_for(20ms);
        set = true;
    }};

    const auto began = std::chrono::steady_clock::now();

    UNTIL(loop, set.load());

    setter.join();

    OUGHT(std::chrono::steady_clock::now() - began < 5s);
}

// A condition that never comes fails the trial at the deadline, with the place and the condition.
TRIAL("Running: a wait that never comes fails with the condition") {
    tal::Loop loop;
    std::string said;

    const auto began = std::chrono::steady_clock::now();

    try {
        tal::testing::waited(__FILE__, __LINE__, "never came: false", loop, [] { return false; }, 80ms);
    } catch (const tal::testing::Failed& why) {
        said = why.what();
    }

    OUGHT(said.find("running.cpp:") == 0);
    OUGHT(said.find("never came: false") != std::string::npos);
    OUGHT(std::chrono::steady_clock::now() - began >= 80ms);
}

// `came` sleeps until another thread makes the condition true, and `SOON` passes on the same.
TRIAL("Running: came and SOON wait for a condition without a loop") {
    std::atomic<int> set{0};

    std::thread setter{[&] {
        std::this_thread::sleep_for(20ms);
        set = 1;
        std::this_thread::sleep_for(20ms);
        set = 2;
    }};

    const auto began = std::chrono::steady_clock::now();

    OUGHT(tal::testing::came([&] { return set.load() >= 1; }));
    SOON(set.load() == 2);

    setter.join();

    OUGHT(std::chrono::steady_clock::now() - began < 5s);
}

// A condition that never comes makes `came` answer false at the deadline, not before, and the wait
// behind `SOON` fails the trial there with the place and the condition.
TRIAL("Running: came gives up at its deadline, and SOON fails with the condition") {
    const auto began = std::chrono::steady_clock::now();

    OUGHT(!tal::testing::came([] { return false; }, 60ms));
    OUGHT(std::chrono::steady_clock::now() - began >= 60ms);

    std::string said;

    try {
        tal::testing::waited(__FILE__, __LINE__, "never came: false", [] { return false; }, 60ms);
    } catch (const tal::testing::Failed& why) {
        said = why.what();
    }

    OUGHT(said.find("running.cpp:") == 0);
    OUGHT(said.find("never came: false") != std::string::npos);
}

// `--list` names every trial with its file; `--file` and `--only` choose which run.
TRIAL("Running: the command line lists and chooses the trials") {
    const tal::Output listed = again({"--list"});
    const std::string list = tal::text::of(listed.output);

    SAME(listed.exit.code, 0);
    OUGHT(list.find("running.cpp  Running: the command line lists and chooses the trials\n") != std::string::npos);
    OUGHT(list.find("watching.cpp  Testing: a trial that oversleeps when asked\n") != std::string::npos);

    const tal::Output chosen = again({"--file", "watching.cpp", "--file", "pathing.cpp", "--only",
                                      "Testing: a trial that oversleeps when asked"});
    const std::string ran = tal::text::of(chosen.output);

    SAME(chosen.exit.code, 0);
    OUGHT(ran.find("  ok    Testing: a trial that oversleeps when asked\n") != std::string::npos);
    OUGHT(ran.find("Passed 1 of 1.") != std::string::npos);

    const tal::Output none = again({"--file", "nothing.cpp"});

    OUGHT(tal::text::of(none.output).find("Passed 0 of 0.") != std::string::npos);
}

// An unknown option and an unknown pretended program end with code 2 and say why.
TRIAL("Running: an unknown option or program is refused") {
    const tal::Output wrong = again({"--nonsense"});

    SAME(wrong.exit.code, 2);
    OUGHT(tal::text::of(wrong.errors).find("Usage: ") == 0);

    const tal::Output unknown = again({"--pretending", "nobody"});

    SAME(unknown.exit.code, 2);
    OUGHT(tal::text::of(unknown.errors).find("Unknown pretending: nobody") != std::string::npos);
}

// A pretended program gets the arguments after its name, in UTF-8 as given.
TRIAL("Running: a pretended program gets its own arguments") {
    const tal::Output got = again({"--pretending", "args", "jeden", "zażółć gęślą jaźń", ""});

    SAME(got.exit.code, 0);
    SAME(tal::text::of(got.output), std::string{"jeden\nzażółć gęślą jaźń\n\n"});
}
