#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "tal/clock.hpp"
#include "tal/executor.hpp"
#include "tal/log.hpp"
#include "tal/port.hpp"
#include "tal/task.hpp"
#include "tal/testing.hpp"

#include "quieting.hpp"

// The tasks of tal::core: running, awaiting and failing, moving between executors, results from
// other threads, timers on a manual clock, the owner's scope and its cancellation, and the ways of
// waiting for several tasks.

using namespace std::chrono_literals;

namespace {

// Counts the frames that are alive: each sentinel lives in a task's frame, so a frame destroyed
// without resuming still lowers the count.
struct Sentinel {
    std::atomic<int>& alive;

    // One more frame alive.
    explicit Sentinel(std::atomic<int>& counter) : alive(counter) { ++alive; }

    // One frame fewer.
    ~Sentinel() { --alive; }

    Sentinel(const Sentinel&) = delete;
    Sentinel& operator=(const Sentinel&) = delete;
};

// Goes from the loop `home` to the pool, and on to the io pool when `hopping`, counts itself in
// `gone` there and waits there for `let`, then comes back to `home` and counts itself in `back`;
// its frame counts in `alive`.
tal::Task<void> away(tal::Loop& home, tal::Pending<void> let, std::atomic<int>& gone, std::atomic<int>& back, std::atomic<int>& alive, bool hopping) {
    const Sentinel frame{alive};

    co_await tal::to(tal::pool());

    if (hopping) co_await tal::to(tal::io());

    ++gone;

    co_await let.task();
    co_await tal::to(home);

    ++back;
}

// Goes from the loop `home` to the pool, counts itself in `gone` there and works there (holding its
// thread) until `let`, then comes back to `home` and counts itself in `back`; its frame counts in
// `alive`.
tal::Task<void> busy(tal::Loop& home, const std::atomic<bool>& let, std::atomic<int>& gone, std::atomic<int>& back, std::atomic<int>& alive) {
    const Sentinel frame{alive};

    co_await tal::to(tal::pool());

    ++gone;

    while (!let.load()) std::this_thread::sleep_for(1ms);

    co_await tal::to(home);

    ++back;
}

// Goes to the pool, counts itself in `gone` there, waits there for `let` and ends there; its frame
// counts in `alive`.
tal::Task<void> ending(tal::Pending<void> let, std::atomic<int>& gone, std::atomic<int>& alive) {
    const Sentinel frame{alive};

    co_await tal::to(tal::pool());

    ++gone;

    co_await let.task();
}

// Goes to the pool, counts itself in `gone` there and waits there for `never`, which is never
// settled; its frame counts in `alive`.
tal::Task<void> stranded(tal::Pending<void> never, std::atomic<int>& gone, std::atomic<int>& alive) {
    const Sentinel frame{alive};

    co_await tal::to(tal::pool());

    ++gone;

    co_await never.task();
}

// Twice the value, after a hop through the loop.
tal::Task<int> doubled(int value) {
    co_await tal::to(tal::loop());

    co_return value * 2;
}

// The sum of two doubled values, awaited one after another.
tal::Task<int> summed(int one, int two) {
    const int first = co_await doubled(one);
    const int second = co_await doubled(two);

    co_return first + second;
}

// A task that fails with a trouble of the given kind.
tal::Task<int> failing(tal::Kind kind) {
    co_await tal::to(tal::loop());

    throw tal::Trouble{kind, "zawiodło"};
}

// The failure of the inner task passes through the outer one unchanged.
tal::Task<int> passing(tal::Kind kind) { co_return co_await failing(kind) + 1; }

// Waits for the pending result, keeping a sentinel in its frame and marking that it went on.
tal::Task<int> waiting(tal::Pending<int> pending, std::atomic<int>& alive, std::atomic<bool>& went_on) {
    const Sentinel kept{alive};
    const int got = co_await pending.task();

    went_on = true;

    co_return got;
}

// A port of a kind no program sets by default.
struct Speaker : tal::Adapter {
    static constexpr std::string_view kind = "speaker";

    // The adapter's name.
    [[nodiscard]] std::string_view name() const override { return "test"; }
};

// The value after `wait` on the clock of the loop.
tal::Task<int> late(std::chrono::milliseconds wait, int value) {
    co_await tal::after(wait);

    co_return value;
}

// One more than `given`, at once, without waiting for anything.
tal::Task<int> next_of(int given) { co_return given + 1; }

// Counts to `many` by awaiting `next_of` that many times in a loop.
tal::Task<int> counted(int many) {
    int made = 0;

    for (int one = 0; one < many; ++one) made = co_await next_of(made);

    co_return made;
}

// Sleeps an hour on the clock of the program's loop, keeping a sentinel in its frame.
tal::Task<void> sleeping(std::atomic<int>& alive) {
    const Sentinel kept{alive};

    co_await tal::after(1h);
}

}

TRIAL("Task: a value comes out of a chain of tasks awaited one after another") {
    tal::Loop loop;

    SAME(tal::wait(loop, summed(2, 3)), 10);
}

// A task that ends at once gives its value back without the awaiting one being suspended, so a
// loop of a million of them keeps the stack as it was, also in a build without optimization,
// where the compiler makes no tail call of the transfer from one coroutine to another.
TRIAL("Task: a million tasks that end at once, awaited in a loop, do not grow the stack") {
    tal::Loop loop;

    SAME(tal::wait(loop, counted(1000000)), 1000000);
}

TRIAL("Task: a failure passes through the awaiting task unchanged") {
    tal::Loop loop;

    try {
        (void)tal::wait(loop, passing(tal::Kind::denied));
        OUGHT(false);
    } catch (const tal::Trouble& why) {
        OUGHT(why.kind() == tal::Kind::denied);
        SAME(std::string{why.what()}, std::string{"zawiodło"});
    }
}

TRIAL("Task: to() moves the task to the pool and back to the loop's thread") {
    tal::Loop loop;

    const auto mine = std::this_thread::get_id();

    const auto [away, back] = tal::wait(loop, [](std::thread::id) -> tal::Task<std::tuple<std::thread::id, std::thread::id>> {
        co_await tal::to(tal::pool());

        const auto there = std::this_thread::get_id();

        co_await tal::to(tal::loop());

        co_return std::tuple{there, std::this_thread::get_id()};
    }(mine));

    OUGHT(away != mine);
    OUGHT(back == mine);
}

TRIAL("wait: a task that ends on the pool with a value, a failure or nothing still wakes the loop") {
    tal::Loop loop;

    for (int round = 0; round < 200; ++round) {
        SAME(tal::wait(loop, []() -> tal::Task<int> {
                 co_await tal::to(tal::pool());

                 co_return 7;
             }()),
             7);

        tal::wait(loop, []() -> tal::Task<void> { co_await tal::to(tal::pool()); }());

        THREW(tal::wait(loop, []() -> tal::Task<int> {
            co_await tal::to(tal::pool());

            throw std::logic_error{"na puli"};
        }()));
    }
}

TRIAL("Pending: a result settled on another thread resumes the task on its own executor") {
    tal::Loop loop;
    tal::Pending<int> pending;
    std::atomic<int> alive{0};
    std::atomic<bool> went_on{false};

    std::thread other{[pending]() mutable {
        std::this_thread::sleep_for(20ms);
        pending.settle(41);
    }};

    const auto mine = std::this_thread::get_id();
    std::thread::id resumed;

    const int got = tal::wait(loop, [](tal::Pending<int> given, std::atomic<int>& counter, std::atomic<bool>& flag,
                                       std::thread::id& where) -> tal::Task<int> {
        const int value = co_await waiting(std::move(given), counter, flag);

        where = std::this_thread::get_id();

        co_return value + 1;
    }(pending, alive, went_on, resumed));

    other.join();

    SAME(got, 42);
    OUGHT(went_on);
    OUGHT(resumed == mine);
    SAME(alive.load(), 0);
}

TRIAL("Pending: a result settled before the task waits is taken without suspending, and a second one is ignored") {
    tal::Loop loop;
    tal::Pending<int> pending;

    pending.settle(7);
    pending.settle(8);

    OUGHT(pending.settled());
    SAME(tal::wait(loop, pending.task()), 7);
}

TRIAL("Pending: a failure given to it comes out where the task waits") {
    tal::Loop loop;
    tal::Pending<void> pending;

    pending.fail(tal::Trouble{tal::Kind::refused, "odmowa"});

    try {
        tal::wait(loop, pending.task());
        OUGHT(false);
    } catch (const tal::Trouble& why) {
        OUGHT(why.kind() == tal::Kind::refused);
    }
}

TRIAL("after(): on a manual clock the task waits until the clock is moved past the time") {
    const auto time = std::make_shared<tal::ManualClock>();
    tal::Loop loop{tal::Port<tal::Clock>{time}};
    tal::Scope scope{loop};
    std::atomic<int> done{0};

    scope.run([](std::atomic<int>& into) -> tal::Task<void> { into = co_await late(500ms, 5); }(done));

    QUIETED(loop);

    SAME(done.load(), 0);
    OUGHT(scope.busy());

    time->advance(499ms);

    QUIETED(loop);

    SAME(done.load(), 0);

    time->advance(1ms);

    UNTIL(loop, done.load() == 5);

    OUGHT(!scope.busy());
}

TRIAL("after(): a task on the pool that outlives the program's loop leaves the gone loop alone") {
    std::atomic<int> alive{0};
    std::optional<tal::Scope> scope;

    {
        tal::Loop first;

        scope.emplace(tal::pool());
        scope->run(sleeping(alive));

        UNTIL(first, alive.load() == 1 && first.next().has_value());
    }

    tal::Loop second;

    scope.reset();

    UNTIL(second, alive.load() == 0);
    OUGHT(!second.next().has_value());
}

TRIAL("Scope: a scope destroyed while its task waits destroys the task's frames without resuming them") {
    tal::Loop loop;
    tal::Pending<int> pending;
    std::atomic<int> alive{0};
    std::atomic<bool> went_on{false};

    {
        tal::Scope scope{loop};

        scope.run(waiting(pending, alive, went_on));

        UNTIL(loop, alive.load() == 1);
    }

    UNTIL(loop, alive.load() == 0);


    pending.settle(3);

    QUIETED(loop);

    OUGHT(!went_on);
}

TRIAL("Scope: cancelling a task that runs on the pool ends it at its next checkpoint") {
    tal::Loop loop;
    tal::Scope scope{loop};
    std::atomic<int> alive{0};
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};

    scope.run([](std::atomic<int>& counter, std::atomic<bool>& begun, std::atomic<bool>& end) -> tal::Task<void> {
        const Sentinel kept{counter};

        co_await tal::to(tal::pool());

        begun = true;

        for (int round = 0; round < 100000; ++round) {
            std::this_thread::sleep_for(1ms);
            co_await tal::checkpoint();
        }

        end = true;
    }(alive, started, finished));

    while (!started) {
        (void)loop.turn();
        std::this_thread::yield();
    }

    scope.cancel();

    UNTIL(loop, alive.load() == 0 && !scope.busy());


    SAME(alive.load(), 0);
    OUGHT(!finished);
    OUGHT(!scope.busy());
}

TRIAL("Scope: an unhandled failure of its work goes to the log, a cancellation does not") {
    const auto expected = tal::testing::expect_failure();
    tal::Loop loop;
    tal::Scope scope{loop};

    scope.run(failing(tal::Kind::broken));
    scope.run([]() -> tal::Task<void> {
        co_await tal::to(tal::loop());

        throw tal::Cancelled{};
    }());

    QUIETED(loop);

    const auto entries = tal::recent();
    int failures = 0;

    for (const std::string& one : entries)
        if (one.find("Unhandled failure in a task: zawiodło") != std::string::npos) ++failures;

    OUGHT(failures >= 1);
    OUGHT(expected.failures() == 1);

    for (const std::string& one : entries) OUGHT(one.find("cancelled") == std::string::npos);
}

TRIAL("first(): the first success wins and the other tasks are destroyed") {
    const auto time = std::make_shared<tal::ManualClock>();
    tal::Loop loop{tal::Port<tal::Clock>{time}};
    std::atomic<int> alive{0};
    tal::Scope scope{loop};
    std::atomic<int> got{0};

    scope.run([](std::atomic<int>& counter, std::atomic<int>& into) -> tal::Task<void> {
        std::vector<tal::Task<int>> racing;

        racing.push_back(late(300ms, 3));
        racing.push_back(late(100ms, 1));
        racing.push_back([](std::atomic<int>& kept) -> tal::Task<int> {
            const Sentinel here{kept};

            co_await tal::after(10s);

            co_return 9;
        }(counter));

        into = co_await tal::first(std::move(racing));
    }(alive, got));

    UNTIL(loop, alive.load() == 1);


    time->advance(100ms);

    UNTIL(loop, got.load() == 1 && alive.load() == 0);
}

TRIAL("first(): when every task fails, the last failure comes out") {
    tal::Loop loop;
    std::vector<tal::Task<int>> racing;

    racing.push_back(failing(tal::Kind::denied));
    racing.push_back(failing(tal::Kind::missing));

    try {
        (void)tal::wait(loop, tal::first(std::move(racing)));
        OUGHT(false);
    } catch (const tal::Trouble& why) {
        OUGHT(why.kind() == tal::Kind::denied || why.kind() == tal::Kind::missing);
    }
}

TRIAL("all(): the results come together in order, and a failure cancels the others") {
    tal::Loop loop;

    const auto [one, two] = tal::wait(loop, tal::all(doubled(4), doubled(5)));

    SAME(one, 8);
    SAME(two, 10);

    const auto time = std::make_shared<tal::ManualClock>();
    tal::Loop timed{tal::Port<tal::Clock>{time}};
    std::atomic<int> alive{0};
    std::atomic<bool> failed{false};
    tal::Scope scope{timed};

    scope.run([](std::atomic<int>& counter, std::atomic<bool>& into) -> tal::Task<void> {
        try {
            (void)co_await tal::all(failing(tal::Kind::busy), [](std::atomic<int>& kept) -> tal::Task<int> {
                const Sentinel here{kept};

                co_await tal::after(10s);

                co_return 1;
            }(counter));
        } catch (const tal::Trouble& why) {
            into = why.kind() == tal::Kind::busy;
        }
    }(alive, failed));

    // The failing task moves to the program's loop, the first one made here.
    UNTIL(loop, ((void)timed.turn(), failed.load() && alive.load() == 0));


    OUGHT(failed);
    SAME(alive.load(), 0);
}

TRIAL("within(): a task that does not finish in time gives Overdue, one that does gives its value") {
    const auto time = std::make_shared<tal::ManualClock>();
    tal::Loop loop{tal::Port<tal::Clock>{time}};
    tal::Scope scope{loop};
    std::atomic<int> value{0};
    std::atomic<bool> overdue{false};

    scope.run([](std::atomic<int>& into) -> tal::Task<void> { into = co_await tal::within(1s, late(200ms, 6)); }(value));
    scope.run([](std::atomic<bool>& into) -> tal::Task<void> {
        try {
            (void)co_await tal::within(1s, late(5s, 7));
        } catch (const tal::Overdue&) {
            into = true;
        }
    }(overdue));

    QUIETED(loop);

    time->advance(200ms);

    UNTIL(loop, value.load() == 6);

    OUGHT(!overdue);

    time->advance(800ms);

    UNTIL(loop, overdue.load());

    OUGHT(!scope.busy());
}

TRIAL("Shared: two waiters get the same result of one run") {
    tal::Loop loop;
    std::atomic<int> runs{0};

    tal::Shared<int> shared{[](std::atomic<int>& counted) -> tal::Task<int> {
        ++counted;

        co_await tal::to(tal::pool());

        co_return 12;
    }(runs), loop};

    const auto [one, two] = tal::wait(loop, tal::all(shared.get(), shared.get()));

    SAME(one, 12);
    SAME(two, 12);
    SAME(runs.load(), 1);
    SAME(tal::wait(loop, shared.get()), 12);
}

TRIAL("Port: without an adapter a port refuses at construction; the program's default and the built-in clock are taken; held is only what was set") {
    try {
        const tal::Port<Speaker> none;
        OUGHT(false);
    } catch (const tal::Trouble& why) {
        OUGHT(why.kind() == tal::Kind::missing);
    }

    OUGHT(tal::defaults().held<Speaker>() == nullptr);

    const auto speaker = std::make_shared<Speaker>();

    tal::defaults().set<Speaker>(speaker);

    const tal::Port<Speaker> given;

    SAME(std::string{given->name()}, std::string{"test"});
    OUGHT(tal::defaults().held<Speaker>() == speaker);

    tal::defaults().clear<Speaker>();

    OUGHT(tal::defaults().held<Speaker>() == nullptr);

    const tal::Port<tal::Clock> time;

    SAME(std::string{time->name()}, std::string{"system"});
    OUGHT(tal::defaults().held<tal::Clock>() == nullptr);

    const tal::Optional<Speaker> empty;

    OUGHT(!empty);
}

TRIAL("Task: a thousand tasks hopping between the pool, the io pool and the loop, woken from other threads, all finish") {
    tal::Loop loop;
    tal::Scope scope{loop};
    std::atomic<int> finished{0};
    std::vector<tal::Pending<int>> pendings(1000);

    for (int at = 0; at < 1000; ++at)
        scope.run([](tal::Pending<int> pending, std::atomic<int>& done) -> tal::Task<void> {
            co_await tal::to(tal::pool());
            const int got = co_await pending.task();
            co_await tal::to(tal::io());
            co_await tal::to(tal::loop());

            if (got >= 0) ++done;
        }(pendings[static_cast<std::size_t>(at)], finished));

    std::thread settling{[&pendings] {
        for (std::size_t at = 0; at < pendings.size(); ++at) pendings[at].settle(static_cast<int>(at));
    }};

    for (int tries = 0; tries < 20000 && finished.load() < 1000; ++tries)
        if (!loop.turn()) std::this_thread::sleep_for(100us);

    settling.join();

    SAME(finished.load(), 1000);

    UNTIL(loop, !scope.busy());
}

TRIAL("Scope: scopes destroyed while their results arrive from other threads leave no frame alive") {
    tal::Loop loop;
    std::atomic<int> alive{0};
    std::atomic<bool> went_on{false};

    for (int round = 0; round < 200; ++round) {
        std::vector<tal::Pending<int>> pendings(5);
        std::thread settling;

        {
            tal::Scope scope{loop};

            for (tal::Pending<int>& one : pendings) scope.run(waiting(one, alive, went_on));

            QUIETED(loop);

            settling = std::thread{[&pendings] {
                for (tal::Pending<int>& one : pendings) one.settle(1);
            }};
        }

        settling.join();

        QUIETED(loop);
    }

    UNTIL(loop, alive.load() == 0);
}

namespace {

// Comes from the loop `first` through the pool to the loop `second`, which it counts in `visited`,
// back to the pool, waits there for `let` and comes back to `first`, counting itself in `back`.
tal::Task<void> visiting(tal::Loop& first, tal::Loop& second, tal::Pending<void> let, std::atomic<int>& visited, std::atomic<int>& back) {
    co_await tal::to(tal::pool());
    co_await tal::to(second);

    ++visited;

    co_await tal::to(tal::pool());
    co_await let.task();
    co_await tal::to(first);

    ++back;
}

// A loop going while chains are away from it on the pools: one whose scope goes on ends on the
// pool, two come back to the loop (one through the io pool) and end there, one cancelled while it
// works on the pool comes back and is destroyed on the loop, two cancelled while they wait on the
// pool are destroyed there. They are let go in an order that ends with the chain ending on the pool
// (`ending_last`) or with one coming back. Afterwards all came back that should and no frame lives.
void gone_with(bool ending_last) {
    std::atomic<int> alive{0};
    std::atomic<int> gone{0};
    std::atomic<int> back{0};
    std::vector<tal::Pending<void>> lets(4);
    std::atomic<bool> let{false};
    std::thread opener;
    tal::Pending<void> never;

    {
        std::optional<tal::Scope> outer;
        tal::Loop loop;

        outer.emplace(loop);
        outer->run(away(loop, lets[0], gone, back, alive, false));
        outer->run(away(loop, lets[1], gone, back, alive, true));
        outer->run(ending(lets[3], gone, alive));

        tal::Scope inner{loop};

        inner.run(away(loop, lets[2], gone, back, alive, false));
        inner.run(stranded(never, gone, alive));
        inner.run(busy(loop, let, gone, back, alive));

        while (gone.load() < 6) (void)loop.turn(std::chrono::steady_clock::now() + 1ms);

        SAME(alive.load(), 6);

        opener = std::thread{[&lets, &let, ending_last] {
            std::this_thread::sleep_for(50ms);

            if (!ending_last) lets[3].settle();

            let.store(true);
            std::this_thread::sleep_for(20ms);

            for (const std::size_t one : {2U, 0U, 1U}) lets[one].settle();

            std::this_thread::sleep_for(20ms);

            if (ending_last) lets[3].settle();
        }};
    }

    opener.join();

    SAME(back.load(), 2);
    SAME(alive.load(), 0);
}

}

// A loop that goes first waits for the chains away from it on the pools: they come back to it, or
// end or are destroyed away, and none of them reaches the loop once it is gone; whichever comes
// last, a return or an end on the pool, lets the loop go.
TRIAL("Loop: going, it waits for the chains away on the pools, which come back or are destroyed") {
    gone_with(false);
    gone_with(true);
}

// A chain belongs to the loop it left first: another loop it passes through on the way goes without
// waiting for it, and the chain still comes back to its own.
TRIAL("Loop: a chain belongs to the loop it left first; another loop it passes through does not wait for it") {
    std::atomic<int> visited{0};
    std::atomic<int> back{0};
    tal::Pending<void> let;
    std::thread opener;

    {
        tal::Loop first;
        tal::Scope scope{first};

        {
            tal::Loop second;

            scope.run(visiting(first, second, let, visited, back));

            while (visited.load() < 1) {
                (void)first.turn(std::chrono::steady_clock::now() + 1ms);
                (void)second.turn(std::chrono::steady_clock::now() + 1ms);
            }
        }

        opener = std::thread{[&let] {
            std::this_thread::sleep_for(20ms);
            let.settle();
        }};

        while (back.load() < 1) (void)first.turn(std::chrono::steady_clock::now() + 1ms);
    }

    opener.join();

    SAME(visited.load(), 1);
    SAME(back.load(), 1);
}
