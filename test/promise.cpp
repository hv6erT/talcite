#include "tal/testing.hpp"
#include "waiting.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "talcite/bell.hpp"
#include "talcite/clock.hpp"
#include "talcite/loop.hpp"
#include "talcite/executor.hpp"
#include "talcite/pool.hpp"
#include "talcite/promise.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

using namespace talcite;
using namespace std::chrono_literals;

namespace {

// Waits until the flag is up, at most ten seconds; returns whether it came up.
bool risen(const std::atomic<bool>& flag) {
    return tal::testing::came([&flag] { return flag.load(); });
}

// Turns the loop until the flag rises, at most five seconds; returns whether it did.
bool turned(Loop& loop, const std::atomic<bool>& flag) {
    const auto until = std::chrono::steady_clock::now() + 5s;

    while (!flag.load() && std::chrono::steady_clock::now() < until) {
        (void)loop.turn();

        std::this_thread::sleep_for(1ms);
    }

    return flag.load();
}

// Waits until the counter reaches `want`, at most ten seconds; returns whether it did.
bool reached(const std::atomic<int>& counter, int want) {
    return tal::testing::came([&counter, want] { return counter.load() >= want; });
}

// A job that holds its worker until it is let go, at most five seconds.
struct Holding {
    std::atomic<bool> held{false};
    std::atomic<bool> released{false};

    // The job itself.
    void operator()() {
        held.store(true);

        (void)risen(released);
    }
};

// An exception that is not a `std::exception`.
struct Odd {};

}

// --- Making and getting -----------------------------------------------------------------------

// Work starts when the promise is made, not when somebody asks for the result.
TRIAL("run: the body starts at once, without anyone asking") {
    std::atomic<bool> ran{false};

    Promise<int> one = run(io(), [&ran] {
        ran.store(true);

        return 7;
    });

    OUGHT(risen(ran));
    SAME(std::move(one).get(), 7);
}

TRIAL("run: get gives the value and leaves the handle empty") {
    Promise<std::string> one = run(io(), [] { return std::string{"siedem"}; });

    OUGHT(static_cast<bool>(one));
    SAME(std::move(one).get(), std::string{"siedem"});
    OUGHT(!static_cast<bool>(one));
}

TRIAL("run: what the body throws comes back from get as it was") {
    Promise<int> one = run(io(), []() -> int { throw std::out_of_range{"za daleko"}; });

    std::string said;

    try {
        (void)std::move(one).get();
    } catch (const std::out_of_range& why) {
        said = why.what();
    }

    SAME(said, std::string{"za daleko"});
}

// Every error a promise holds is a `std::exception`, so every consumer can rely on it.
TRIAL("run: something that is not an exception becomes Rejected") {
    Promise<int> one = run(io(), []() -> int { throw Odd{}; });

    bool rejected = false;

    try {
        (void)std::move(one).get();
    } catch (const Rejected&) {
        rejected = true;
    }

    OUGHT(rejected);
}

// A body that throws `Cancelled` ends its promise cancelled, not failed: `recover` does not see it.
TRIAL("run: a body that throws Cancelled ends cancelled, and recover does not see it") {
    std::atomic<bool> recovered{false};

    Promise<int> one = run(io(), []() -> int { throw Cancelled{}; }).recover([&recovered](const std::exception&) {
        recovered.store(true);

        return 1;
    });

    bool cancelled = false;

    try {
        (void)std::move(one).get();
    } catch (const Cancelled&) {
        cancelled = true;
    }

    OUGHT(cancelled);
    OUGHT(!recovered.load());
}

TRIAL("run: a body may take its work, and its stop is the current one") {
    std::atomic<bool> matched{false};

    Promise<void> one = run(io(), [&matched](Work& work) { matched.store(Stop::current() == &work.stop()); });

    std::move(one).get();

    OUGHT(matched.load());
    OUGHT(Stop::current() == nullptr);
}

TRIAL("run: a body that returns a promise is adopted") {
    Promise<int> one = run(io(), [] { return run(io(), [] { return 41; }).then([](int got) { return got + 1; }); });

    SAME(std::move(one).get(), 42);
}

TRIAL("ready and failed: already settled") {
    Promise<int> one = ready(5);

    OUGHT(one.ready());
    SAME(std::move(one).get(), 5);

    Promise<void> none = ready();

    OUGHT(none.ready());
    std::move(none).get();

    Promise<int> bad = failed<int>(std::make_exception_ptr(std::runtime_error{"nie"}));

    THREW((void)std::move(bad).get());
}

TRIAL("Promise: an empty handle refuses everything that needs a promise") {
    Promise<int> empty;

    OUGHT(!static_cast<bool>(empty));
    OUGHT(!empty.ready());
    SAME(empty.progress(), (Progress{}));

    bool refused = false;

    try {
        (void)std::move(empty).then([](int got) { return got; });
    } catch (const std::logic_error&) {
        refused = true;
    }

    OUGHT(refused);

    empty.cancel();
}

// --- The chain --------------------------------------------------------------------------------

TRIAL("then: passes the value on and changes its type") {
    Promise<std::string> one =
        ready(20).then([](int got) { return got + 1; }).then([](int got) { return std::to_string(got); });

    SAME(std::move(one).get(), std::string{"21"});
}

TRIAL("then: after void the callback takes nothing, and a void callback gives void") {
    std::atomic<int> seen{0};

    Promise<void> one = ready().then([] { return 3; }).then([&seen](int got) { seen.store(got); });

    std::move(one).get();

    SAME(seen.load(), 3);
}

TRIAL("then: a promise returned from the callback is adopted, a shared one too") {
    Promise<int> one = ready(1).then([](int got) { return run(io(), [got] { return got * 10; }); });

    SAME(std::move(one).get(), 10);

    SharedPromise<int> shared = ready(4).share();

    Promise<int> two = ready().then([shared] { return shared; });

    SAME(std::move(two).get(), 4);
}

TRIAL("then: an error skips the callbacks and reaches the end") {
    std::atomic<bool> ran{false};

    Promise<int> one = run(io(), []() -> int { throw std::runtime_error{"zepsute"}; }).then([&ran](int got) {
        ran.store(true);

        return got;
    });

    THREW((void)std::move(one).get());

    OUGHT(!ran.load());
}

TRIAL("then: what the callback throws fails the next promise") {
    Promise<int> one = ready(1).then([](int) -> int { throw std::runtime_error{"w środku"}; });

    std::string said;

    try {
        (void)std::move(one).get();
    } catch (const std::exception& why) {
        said = why.what();
    }

    SAME(said, std::string{"w środku"});
}

// A callback for something that has already ended does not run inside the call that registers it:
// the caller may still be setting up what the callback touches.
TRIAL("then: on a settled promise the callback never runs inside then") {
    const std::thread::id mine = std::this_thread::get_id();

    std::atomic<bool> inside{true};

    Promise<void> one = ready(1).then([&inside, mine](int) { inside.store(std::this_thread::get_id() == mine); });

    std::move(one).get();

    OUGHT(!inside.load());
}

// A callback may hold what can only be moved, a promise or a settle, since the node keeps it.
TRIAL("then: the callback may hold a promise") {
    Promise<int> inner = ready(5);

    Promise<int> one = ready(1).then([inner = std::move(inner)](int got) mutable { return std::move(inner).get() + got; });

    SAME(std::move(one).get(), 6);
}

// A value that can only be moved goes through a promise that is not shared: through `then`, out of
// `get`, from a settle and inside an outcome.
TRIAL("then: a value that can only be moved passes through") {
    Promise<int> one = ready(std::make_unique<int>(7)).then([](std::unique_ptr<int> got) { return *got + 1; });

    SAME(std::move(one).get(), 8);

    auto [two, settle] = promised<std::unique_ptr<int>>();

    OUGHT(settle.value(std::make_unique<int>(3)));
    SAME(*std::move(two).get(), 3);

    Outcome<std::unique_ptr<int>> three = ready(std::make_unique<int>(5)).outcome().get();

    OUGHT(three.ok());
    SAME(*std::move(three).value(), 5);
}

// A value refused by a promise nobody holds any more is let go of at once, so whatever it guards is
// released.
TRIAL("settle: a refused value is let go of") {
    const auto watched = std::make_shared<int>(0);

    std::optional<Settle<std::shared_ptr<int>>> kept;

    {
        auto [made, settle] = promised<std::shared_ptr<int>>();

        kept.emplace(std::move(settle));
    }

    OUGHT(!kept->value(std::shared_ptr<int>{watched}));
    SAME(watched.use_count(), 1L);
}

TRIAL("recover: turns an error into a value or into a promise") {
    Promise<int> one = failed<int>(std::make_exception_ptr(std::runtime_error{"nie"})).recover([](const std::exception& why) {
        return std::string{why.what()} == "nie" ? 9 : 0;
    });

    SAME(std::move(one).get(), 9);

    Promise<int> two = failed<int>(std::make_exception_ptr(std::runtime_error{"nie"})).recover([](const std::exception&) {
        return run(io(), [] { return 8; });
    });

    SAME(std::move(two).get(), 8);
}

TRIAL("recover: a value passes untouched, and what recover throws is the new error") {
    std::atomic<bool> ran{false};

    Promise<int> one = ready(3).recover([&ran](const std::exception&) {
        ran.store(true);

        return 0;
    });

    SAME(std::move(one).get(), 3);
    OUGHT(!ran.load());

    Promise<int> two = failed<int>(std::make_exception_ptr(std::runtime_error{"pierwszy"})).recover([](const std::exception&) -> int {
        throw std::runtime_error{"drugi"};
    });

    std::string said;

    try {
        (void)std::move(two).get();
    } catch (const std::exception& why) {
        said = why.what();
    }

    SAME(said, std::string{"drugi"});
}

TRIAL("finally: runs after a value and after an error, and the ending passes through") {
    std::atomic<int> ran{0};

    Promise<int> one = ready(4).finally([&ran] { ++ran; });

    SAME(std::move(one).get(), 4);

    Promise<int> two = failed<int>(std::make_exception_ptr(std::runtime_error{"nie"})).finally([&ran] { ++ran; });

    THREW((void)std::move(two).get());

    SAME(ran.load(), 2);
}

TRIAL("finally: what it throws becomes the error") {
    Promise<int> one = ready(4).finally([] { throw std::runtime_error{"sprzątanie"}; });

    std::string said;

    try {
        (void)std::move(one).get();
    } catch (const std::exception& why) {
        said = why.what();
    }

    SAME(said, std::string{"sprzątanie"});
}

// After a cancellation `finally` runs only when the work really stopped: here when the producer
// lets go of its settle, not when the owner gives up. And it never runs inside `cancel()`.
TRIAL("finally: after a cancellation it waits for the end of the work, off the cancelling thread") {
    auto [source, settle] = promised<int>();

    std::atomic<bool> cleaned{false};
    std::atomic<bool> other{false};

    const std::thread::id mine = std::this_thread::get_id();

    Promise<int> one = std::move(source).finally([&cleaned, &other, mine] {
        other.store(std::this_thread::get_id() != mine);
        cleaned.store(true);
    });

    one.cancel();

    OUGHT(settle.stop().stopped());

    std::this_thread::sleep_for(50ms);

    OUGHT(!cleaned.load());

    (void)settle.value(1);

    OUGHT(risen(cleaned));
    OUGHT(other.load());
}

// Waits for `what` in a coroutine, telling when a stop resumed it with `Cancelled`.
Promise<void> waiting_for(Promise<int> what, std::shared_ptr<std::atomic<bool>> resumed) {
    try {
        (void)co_await std::move(what);
    } catch (const Cancelled&) {
        resumed->store(true);

        throw;
    }
}

// What a coroutine waits for is part of its work: given up, the coroutine resumes at once with
// `Cancelled`, but its `finally` runs only when what it waited for has really stopped, here when
// the producer lets go of its settle.
TRIAL("finally: a coroutine given up while it waits runs finally only after what it waited for") {
    auto [source, settle] = promised<int>();

    const auto resumed = std::make_shared<std::atomic<bool>>(false);

    std::atomic<bool> cleaned{false};

    Promise<void> one = waiting_for(std::move(source), resumed).finally([&cleaned] { cleaned.store(true); });

    one.cancel();

    OUGHT(risen(*resumed));
    OUGHT(settle.stop().stopped());

    std::this_thread::sleep_for(50ms);

    OUGHT(!cleaned.load());

    (void)settle.value(1);

    OUGHT(risen(cleaned));
}

// A value from `Settle` sends the callbacks and the finish of the settled node to the pool apart,
// so a cancellation of the chain can meet a callback just as it starts, on another thread. The
// callback that starts then is already counted: `finally` comes only after it, never before, and
// the callback never starts after `finally` has run. Many rounds, each cancelling at another
// moment.
TRIAL("finally: a cancellation that meets a callback as it starts does not let finally run first") {
    for (int round = 0; round < 400; ++round) {
        struct Seen {
            std::atomic<int> started{0};
            std::atomic<bool> busy{false};
            std::atomic<bool> finished{false};
            std::atomic<bool> early{false};
        };

        const auto seen = std::make_shared<Seen>();

        auto [source, settle] = promised<void>();

        std::optional<Promise<void>> tail;

        tail.emplace(std::move(source)
                         .then([seen] {
                             if (seen->finished.load()) seen->early.store(true);

                             seen->busy.store(true);
                             seen->started.fetch_add(1);

                             std::this_thread::sleep_for(200us);

                             seen->busy.store(false);
                         })
                         .finally([seen] {
                             if (seen->busy.load()) seen->early.store(true);

                             seen->finished.store(true);
                         }));

        std::thread settling{[settle = std::move(settle)]() mutable { (void)settle.value(); }};

        std::this_thread::sleep_for(std::chrono::microseconds{round % 40 * 5});

        tail.reset();
        settling.join();

        OUGHT(risen(seen->finished));

        std::this_thread::sleep_for(1ms);

        OUGHT(!seen->early.load());
    }
}

// A chain moved to an executor (a loop) cleans up there after a cancellation too, so that code
// bound to that executor's thread never runs on another one.
TRIAL("finally: after a cancellation it runs on the executor the chain moved to") {
    ManualExecutor executor;

    auto [source, settle] = promised<int>();

    std::atomic<bool> cleaned{false};

    Promise<int> one = std::move(source).on(executor).finally([&cleaned] { cleaned.store(true); });

    one.cancel();

    (void)settle.value(1);

    SOON(executor.waiting() != 0);

    OUGHT(!cleaned.load());

    (void)executor.turn();

    OUGHT(cleaned.load());
}

// A chain moved to an executor cleans up there also when nothing cancels it. A manual executor has
// no thread of its own, so `finally` runs there as a task of its own, after the task that brought
// the value, never inside it, whichever thread finished the work before it.
TRIAL("finally: after a move to an executor it runs there, as a task of its own") {
    ManualExecutor executor;

    std::atomic<bool> cleaned{false};

    Promise<int> one = ready(4).on(executor).finally([&cleaned] { cleaned.store(true); });

    SOON(executor.waiting() != 0);

    SAME(executor.turn(), std::size_t{1});
    OUGHT(!cleaned.load());
    OUGHT(!one.ready());

    SOON(executor.waiting() != 0);

    SAME(executor.turn(), std::size_t{1});
    OUGHT(cleaned.load());
    OUGHT(one.ready());
    SAME(std::move(one).get(), 4);
}

// The work before the move may finish on a thread of the pool after the executor has already taken
// the value: a producer's `Settle` sends the value and its own finish to the pool apart. `finally`
// still runs on the thread that turns the executor, never on the one where the work finished. Many
// rounds, since which of the two comes first is the pool's to decide.
TRIAL("finally: after a move to an executor it never runs where the work finished") {
    const std::thread::id mine = std::this_thread::get_id();

    ManualExecutor executor;

    for (int round = 0; round < 200; ++round) {
        auto [source, settle] = promised<int>();

        std::atomic<bool> cleaned{false};
        std::atomic<bool> elsewhere{false};

        Promise<int> one = std::move(source).on(executor).finally([&cleaned, &elsewhere, mine] {
            if (std::this_thread::get_id() != mine) elsewhere.store(true);

            cleaned.store(true);
        });

        std::thread settling{[settle = std::move(settle), round]() mutable { (void)settle.value(round); }};

        const auto until = std::chrono::steady_clock::now() + 5s;

        while (!cleaned.load() && std::chrono::steady_clock::now() < until) {
            (void)executor.turn();

            std::this_thread::sleep_for(std::chrono::microseconds{round % 20 * 50 + 10});
        }

        settling.join();

        OUGHT(cleaned.load());
        OUGHT(!elsewhere.load());
    }
}

// The same after `here`: on the loop's thread `finally` may run at once, where the work finishes
// there, and goes to the loop where it finishes on the pool; either way it runs on the loop's
// thread.
TRIAL("finally: after here it never runs where the work finished") {
    const std::thread::id mine = std::this_thread::get_id();

    Loop loop;

    for (int round = 0; round < 200; ++round) {
        auto [source, settle] = promised<int>();

        std::atomic<bool> cleaned{false};
        std::atomic<bool> elsewhere{false};

        Promise<int> one = std::move(source).here().finally([&cleaned, &elsewhere, mine] {
            if (std::this_thread::get_id() != mine) elsewhere.store(true);

            cleaned.store(true);
        });

        std::thread settling{[settle = std::move(settle), round]() mutable { (void)settle.value(round); }};

        const auto until = std::chrono::steady_clock::now() + 5s;

        while (!cleaned.load() && std::chrono::steady_clock::now() < until) {
            (void)loop.turn();

            std::this_thread::sleep_for(std::chrono::microseconds{round % 20 * 50 + 10});
        }

        settling.join();

        OUGHT(cleaned.load());
        OUGHT(!elsewhere.load());
    }
}

TRIAL("outcome: gives the ending as a value") {
    Outcome<int> good = ready(2).outcome().get();

    OUGHT(good.ok());
    SAME(good.value(), 2);

    Outcome<int> bad = failed<int>(std::make_exception_ptr(std::runtime_error{"nie"})).outcome().get();

    OUGHT(!bad.ok());
    OUGHT(!bad.cancelled());
    OUGHT(bad.error() != nullptr);
    THREW((void)bad.value());

    Outcome<int> gone = run(io(), []() -> int { throw Cancelled{}; }).outcome().get();

    OUGHT(gone.cancelled());
}

// --- Owners -----------------------------------------------------------------------------------

TRIAL("owners: destroying the tail stops the source") {
    auto [source, settle] = promised<int>();

    {
        Promise<int> tail = std::move(source).then([](int got) { return got; }).then([](int got) { return got + 1; });

        OUGHT(!settle.stop().stopped());
    }

    OUGHT(settle.stop().stopped());
    OUGHT(!settle.value(3));
}

TRIAL("owners: cancelling the tail stops a body that waits") {
    std::atomic<bool> heard{false};
    std::atomic<bool> began{false};

    Promise<void> tail = run(io(), [&heard, &began](Work& work) {
                             began.store(true);

                             heard.store(work.stop().wait(5s));
                         }).then([] {});

    OUGHT(risen(began));

    tail.cancel();

    OUGHT(risen(heard));
}

TRIAL("owners: a body cancelled before it starts never runs") {
    ManualExecutor executor;

    std::atomic<bool> ran{false};

    Promise<void> one = run(executor, [&ran] { ran.store(true); });

    one.cancel();

    SAME(executor.turn(), std::size_t{1});

    OUGHT(!ran.load());
}

// A pool lane takes a job back when its owners leave before it starts: it does not wait in the
// queue for a worker to find it abandoned.
TRIAL("owners: a job abandoned before it starts leaves the pool queue at once") {
    Pool mine{1};

    Holding blocking;

    (void)mine.post([&blocking] { blocking(); }, Lane::cpu);

    OUGHT(risen(blocking.held));

    std::atomic<bool> ran{false};

    Promise<void> one = run(mine.cpu(), [&ran] { ran.store(true); });

    one.cancel();

    blocking.released.store(true);

    mine.drain();

    OUGHT(!ran.load());
}

TRIAL("owners: detach lets the work go on and its error goes to lost") {
    std::mutex guard;
    std::condition_variable stirred;
    std::string heard;

    lost([&](const std::exception& why) {
        const std::lock_guard<std::mutex> held{guard};

        heard = why.what();

        stirred.notify_all();
    });

    auto [source, settle] = promised<int>();

    std::move(source).detach();

    OUGHT(!settle.stop().stopped());

    (void)settle.fail("zgubione");

    {
        std::unique_lock<std::mutex> held{guard};

        (void)stirred.wait_for(held, 5s, [&] { return !heard.empty(); });
    }

    lost({});

    SAME(heard, std::string{"zgubione"});
}

TRIAL("owners: a stop hook runs on the io lane, and a withdrawn one does not") {
    auto [source, settle] = promised<int>();

    std::atomic<bool> ran{false};
    std::atomic<bool> withdrawn{false};

    Hook kept = settle.stop().when([&ran] { ran.store(true); });

    {
        Hook gone = settle.stop().when([&withdrawn] { withdrawn.store(true); });
    }

    source.cancel();

    OUGHT(risen(ran));

    std::this_thread::sleep_for(20ms);

    OUGHT(!withdrawn.load());
}

#ifndef _WIN32

TRIAL("owners: the stop's descriptor becomes readable when the work is stopped") {
    auto [source, settle] = promised<int>();

    const Stop::Handle heard = settle.stop().heard();

    pollfd one{heard, POLLIN, 0};

    SAME(::poll(&one, 1, 0), 0);

    source.cancel();

    SAME(::poll(&one, 1, 1000), 1);
}

#endif

// --- Producers --------------------------------------------------------------------------------

TRIAL("Settle: the first settling wins, the rest say no") {
    auto [made, settle] = promised<int>();

    OUGHT(settle.value(1));
    OUGHT(!settle.value(2));
    OUGHT(!settle.fail("późno"));

    SAME(std::move(made).get(), 1);
}

TRIAL("Settle: abandoned unsettled, it breaks its promise") {
    std::optional<Promise<int>> made;

    {
        auto [one, settle] = promised<int>();

        made.emplace(std::move(one));
    }

    bool broken = false;

    try {
        (void)std::move(*made).get();
    } catch (const Broken&) {
        broken = true;
    }

    OUGHT(broken);
}

// Settling runs no continuation in the call, so a producer may settle under its own lock even when
// the continuation reaches for the same lock.
TRIAL("Settle: settling under a lock the continuation needs does not deadlock") {
    std::mutex guard;

    auto [made, settle] = promised<int>();

    Promise<int> after = std::move(made).then([&guard](int got) {
        const std::lock_guard<std::mutex> held{guard};

        return got + 1;
    });

    {
        const std::lock_guard<std::mutex> held{guard};

        (void)settle.value(1);

        std::this_thread::sleep_for(20ms);
    }

    SAME(std::move(after).get(), 2);
}

TRIAL("Settle: void settles without a value") {
    auto [made, settle] = promised<void>();

    OUGHT(settle.value());

    std::move(made).get();
}

// --- Executors -----------------------------------------------------------------------------------

TRIAL("on: the rest of the chain runs on the given executor, at its turn") {
    ManualExecutor executor;

    std::atomic<bool> ran{false};

    Promise<void> one = ready(1).on(executor).then([&ran](int) { ran.store(true); });

    SOON(executor.waiting() != 0);

    OUGHT(!ran.load());
    SAME(executor.turn(), std::size_t{1});
    OUGHT(ran.load());
    OUGHT(one.ready());
}

TRIAL("on: an executor that is gone cancels the rest") {
    auto executor = std::make_unique<ManualExecutor>();

    auto [made, settle] = promised<int>();

    Promise<int> one = std::move(made).on(*executor);

    executor.reset();

    (void)settle.value(1);

    THREW((void)std::move(one).get());
}

TRIAL("here: moves the rest to the loop of this thread, and without a loop it refuses") {
    {
        Loop loop;

        std::atomic<bool> ran{false};

        const std::thread::id mine = std::this_thread::get_id();
        std::atomic<bool> home{false};

        Promise<void> one = run(io(), [] { return 1; }).here().then([&ran, &home, mine](int) {
            home.store(std::this_thread::get_id() == mine);
            ran.store(true);
        });

        UNTIL(loop, ran.load());
        OUGHT(home.load());
    }

    Promise<int> kept = ready(1);

    bool refused = false;

    try {
        (void)std::move(kept).here();
    } catch (const std::logic_error&) {
        refused = true;
    }

    OUGHT(refused);
    OUGHT(static_cast<bool>(kept));
}

// --- Home executors ------------------------------------------------------------------------------

// After `here` the rest of the chain runs on the loop's thread, also where its ending comes from
// elsewhere: a deadline that passes on the clock's thread, a promise a callback returned and that
// ends on the pool, a continuation hung on a promise that has already ended. A join keeps an
// executor only when all its members share it.

TRIAL("timeout: after here a deadline that passes fails the chain on the loop, so recover runs there") {
    Loop loop;
    ManualClock clock;

    const std::thread::id mine = std::this_thread::get_id();

    auto [made, settle] = promised<int>();

    std::atomic<bool> recovered{false};
    std::atomic<bool> home{false};

    Promise<int> one = std::move(made).here().timeout(50ms, clock).recover([&recovered, &home, mine](const std::exception&) {
        home.store(std::this_thread::get_id() == mine);
        recovered.store(true);

        return 0;
    });

    clock.advance(50ms);

    OUGHT(turned(loop, recovered));
    OUGHT(home.load());
}

TRIAL("then: a promise a callback returns after here brings its ending back to the loop") {
    Loop loop;

    const std::thread::id mine = std::this_thread::get_id();

    std::atomic<bool> ran{false};
    std::atomic<bool> home{false};

    Promise<void> one = run(io(), [] { return 1; })
                            .here()
                            .then([](int) { return run(io(), [] { return 2; }); })
                            .then([&ran, &home, mine](int got) {
                                home.store(std::this_thread::get_id() == mine && got == 2);
                                ran.store(true);
                            });

    OUGHT(turned(loop, ran));
    OUGHT(home.load());
}

TRIAL("here: a continuation hung on a promise that has already ended runs on the loop") {
    Loop loop;

    const std::thread::id mine = std::this_thread::get_id();

    Promise<int> ended = run(io(), [] { return 1; }).here();

    UNTIL(loop, ended.ready());

    std::atomic<bool> ran{false};
    std::atomic<bool> home{false};

    Promise<void> one = std::move(ended).then([&ran, &home, mine](int) {
        home.store(std::this_thread::get_id() == mine);
        ran.store(true);
    });

    OUGHT(turned(loop, ran));
    OUGHT(home.load());
}

// A promise that has ended on the loop and moves on to the pool goes there straight: `on` takes the
// ending to its own executor, with no detour through the loop, so it needs no turn of the loop.
TRIAL("on: a promise that has ended on the loop moves to the pool without the loop") {
    Loop loop;

    Promise<int> ended = run(io(), [] { return 1; }).here();

    UNTIL(loop, ended.ready());

    std::atomic<bool> ran{false};

    Promise<void> moved = std::move(ended).on(io()).then([&ran](int) { ran.store(true); });

    OUGHT(risen(ran));
}

// Waiting on the loop's thread for what ends only when that loop turns would hang for ever: it is
// refused at once. What has already ended there, or ends elsewhere, can be waited for.
TRIAL("get: waiting on the loop's thread for what ends on that loop is refused") {
    Loop loop;

    auto [made, settle] = promised<int>();

    Promise<int> stuck = std::move(made).here();

    bool refused = false;

    try {
        (void)std::move(stuck).get();
    } catch (const std::logic_error&) {
        refused = true;
    }

    OUGHT(refused);

    SAME(run(io(), [] { return 3; }).get(), 3);
}

TRIAL("all: a join dwells at the executor its members share, and nowhere when they differ") {
    Loop loop;

    const std::thread::id mine = std::this_thread::get_id();

    const auto joined = [&loop](std::vector<Promise<int>> among) {
        Promise<std::vector<int>> made = all(std::move(among));

        UNTIL(loop, made.ready());

        return made;
    };

    std::vector<Promise<int>> both;

    both.push_back(run(io(), [] { return 1; }).here());
    both.push_back(run(io(), [] { return 2; }).here());

    std::atomic<bool> ran{false};
    std::atomic<bool> home{false};

    Promise<void> shared = joined(std::move(both)).then([&ran, &home, mine](std::vector<int>) {
        home.store(std::this_thread::get_id() == mine);
        ran.store(true);
    });

    OUGHT(turned(loop, ran));
    OUGHT(home.load());

    // One member on the loop, one on the pool: the join has no executor, so what hangs on it after
    // its end does not wait for the loop.
    std::vector<Promise<int>> mixed;

    mixed.push_back(run(io(), [] { return 1; }).here());
    mixed.push_back(run(io(), [] { return 2; }));

    std::atomic<bool> loose{false};

    Promise<void> apart = joined(std::move(mixed)).then([&loose](std::vector<int>) { loose.store(true); });

    OUGHT(risen(loose));
}

// --- Time -------------------------------------------------------------------------------------

TRIAL("later: ends when the clock gets there, not before") {
    ManualClock clock;

    Promise<void> one = later(100ms, clock);

    clock.advance(99ms);

    std::this_thread::sleep_for(20ms);

    OUGHT(!one.ready());

    clock.advance(1ms);

    std::move(one).get();
}

TRIAL("later: abandoned, it leaves the clock at once") {
    ManualClock clock;

    Promise<void> one = later(1h, clock);

    SAME(clock.waiting(), std::size_t{1});

    one.cancel();

    SAME(clock.waiting(), std::size_t{0});
}

TRIAL("timeout: a late one fails with Overdue and stops the work") {
    ManualClock clock;

    auto [made, settle] = promised<int>();

    Promise<int> one = std::move(made).timeout(50ms, clock);

    clock.advance(50ms);

    bool overdue = false;

    try {
        (void)std::move(one).get();
    } catch (const Overdue&) {
        overdue = true;
    }

    OUGHT(overdue);
    OUGHT(settle.stop().wait(5s));
}

TRIAL("timeout: one in time gives its value and leaves the clock") {
    ManualClock clock;

    auto [made, settle] = promised<int>();

    Promise<int> one = std::move(made).timeout(50ms, clock);

    (void)settle.value(6);

    SAME(std::move(one).get(), 6);

    SOON(clock.waiting() == 0);

    SAME(clock.waiting(), std::size_t{0});
}

TRIAL("timeout: on the real clock too") {
    Promise<void> one = run(io(), [](Work& work) { (void)work.stop().wait(5s); }).timeout(20ms);

    THREW(std::move(one).get());
}

// --- Shared -----------------------------------------------------------------------------------

TRIAL("SharedPromise: every view gets the value, and get gives it by reference") {
    SharedPromise<std::string> shared = run(io(), [] { return std::string{"wspólne"}; }).share();

    Promise<std::string> one = shared.view();
    Promise<std::string> two = shared.view();

    SAME(std::move(one).get(), std::string{"wspólne"});
    SAME(std::move(two).get(), std::string{"wspólne"});
    SAME(shared.get(), std::string{"wspólne"});
    SAME(shared.view().get(), std::string{"wspólne"});
}

TRIAL("SharedPromise: a view that gives up takes nothing from the others") {
    auto [made, settle] = promised<int>();

    SharedPromise<int> shared = std::move(made).share();

    Promise<int> impatient = shared.view();
    Promise<int> patient = shared.view();

    impatient.cancel();

    OUGHT(!settle.stop().stopped());

    (void)settle.value(5);

    SAME(std::move(patient).get(), 5);
}

TRIAL("SharedPromise: when every owner leaves, the work stops") {
    auto [made, settle] = promised<int>();

    {
        SharedPromise<int> shared = std::move(made).share();
        SharedPromise<int> copy = shared;

        Promise<int> one = shared.view();

        shared.cancel();
        one.cancel();

        OUGHT(!settle.stop().stopped());
    }

    OUGHT(settle.stop().stopped());
}

// --- Joins ------------------------------------------------------------------------------------

TRIAL("all: the values in the order given, and an empty set at once") {
    std::vector<Promise<int>> among;

    for (int at = 0; at < 5; ++at) among.push_back(run(io(), [at] {
        std::this_thread::sleep_for(std::chrono::milliseconds{(5 - at) * 5});

        return at;
    }));

    SAME(all(std::move(among)).get(), (std::vector<int>{0, 1, 2, 3, 4}));

    SAME(all(std::vector<Promise<int>>{}).get(), (std::vector<int>{}));

    all(std::vector<Promise<void>>{}).get();
}

TRIAL("all: the first error fails it and stops the rest") {
    auto [made, settle] = promised<int>();

    std::vector<Promise<int>> among;

    among.push_back(std::move(made));
    among.push_back(failed<int>(std::make_exception_ptr(std::runtime_error{"zepsute"})));

    THREW((void)all(std::move(among)).get());

    OUGHT(settle.stop().wait(5s));
}

TRIAL("all: a list of different types, void among them") {
    std::tuple<int, std::monostate, std::string> got = all(ready(1), ready(), ready(std::string{"trzy"})).get();

    SAME(std::get<0>(got), 1);
    SAME(std::get<2>(got), std::string{"trzy"});
}

TRIAL("all: void promises") {
    std::atomic<int> ran{0};

    std::vector<Promise<void>> among;

    for (int at = 0; at < 5; ++at) among.push_back(run(io(), [&ran] { ++ran; }));

    all(std::move(among)).get();

    SAME(ran.load(), 5);
}

TRIAL("any: the first value wins and the rest stop; all failing gives the last error") {
    auto [slow, settle] = promised<int>();

    std::vector<Promise<int>> among;

    among.push_back(failed<int>(std::make_exception_ptr(std::runtime_error{"pierwszy"})));
    among.push_back(std::move(slow));
    among.push_back(ready(3));

    SAME(any(std::move(among)).get(), 3);

    OUGHT(settle.stop().wait(5s));

    std::vector<Promise<int>> bad;

    bad.push_back(failed<int>(std::make_exception_ptr(std::runtime_error{"jeden"})));
    bad.push_back(failed<int>(std::make_exception_ptr(std::runtime_error{"dwa"})));

    THREW((void)any(std::move(bad)).get());

    THREW((void)any(std::vector<Promise<int>>{}).get());
}

TRIAL("race: the first ending of any kind wins") {
    auto [slow, settle] = promised<int>();

    std::vector<Promise<int>> among;

    among.push_back(std::move(slow));
    among.push_back(failed<int>(std::make_exception_ptr(std::runtime_error{"szybki"})));

    THREW((void)race(std::move(among)).get());

    OUGHT(settle.stop().wait(5s));
}

TRIAL("settled: every ending in the order given, never failing") {
    std::vector<Promise<int>> among;

    among.push_back(ready(1));
    among.push_back(failed<int>(std::make_exception_ptr(std::runtime_error{"nie"})));
    among.push_back(run(io(), []() -> int { throw Cancelled{}; }));

    const std::vector<Outcome<int>> got = settled(std::move(among)).get();

    SAME(got.size(), std::size_t{3});
    OUGHT(got[0].ok());
    OUGHT(!got[1].ok() && !got[1].cancelled());
    OUGHT(got[2].cancelled());
}

// --- Progress ---------------------------------------------------------------------------------

TRIAL("progress: what the producer reports, through the chain and summed in a join") {
    auto [one, first] = promised<int>();
    auto [two, second] = promised<int>();

    first.progress(Progress{1, 4});
    second.progress(Progress{3, 4});

    Promise<int> chained = std::move(one).then([](int got) { return got; });

    SAME(chained.progress(), (Progress{1, 4}));

    std::vector<Promise<int>> among;

    among.push_back(std::move(chained));
    among.push_back(std::move(two));

    Promise<std::vector<int>> joined = all(std::move(among));

    SAME(joined.progress(), (Progress{4, 8}));

    (void)first.value(1);
    (void)second.value(2);

    (void)std::move(joined).get();
}

TRIAL("progress: a body reports through its work, and an ended one is whole") {
    std::atomic<bool> told{false};
    std::atomic<bool> go{false};

    Promise<void> one = run(io(), [&told, &go](Work& work) {
        work.progress(Progress{5, 10});

        told.store(true);

        (void)risen(go);
    });

    OUGHT(risen(told));

    SAME(one.progress(), (Progress{5, 10}));

    go.store(true);

    SOON(one.ready());

    SAME(one.progress(), (Progress{10, 10}));
    SAME(one.progress().fraction(), 1.0);
}

// --- Waiting inside work ----------------------------------------------------------------------

// A body that waits in `get` hears the stop of its own work: it gives up the promise it waited for
// (whose work stops in turn) and leaves with `Cancelled` at once.
TRIAL("get: a stopped body stops waiting and stops what it waited for") {
    auto [inner, settle] = promised<int>();

    std::atomic<bool> waiting{false};
    std::atomic<bool> left{false};

    Promise<int> outer = run(io(), [&waiting, &left, inner = std::move(inner)]() mutable {
        waiting.store(true);

        try {
            return std::move(inner).get();
        } catch (const Cancelled&) {
            left.store(true);

            throw;
        }
    });

    OUGHT(risen(waiting));

    std::this_thread::sleep_for(20ms);

    outer.cancel();

    OUGHT(risen(left));
    OUGHT(settle.stop().stopped());
}

// A computing task that waits for other computing work gives its slot back for the wait. Twice as
// many such tasks as slots would otherwise fill every slot with waiting, and the work they wait for
// would never get one.
TRIAL("get: a computing body waiting for computing work does not hold its slot") {
    const int many = static_cast<int>(pooling().most()) * 2;

    std::vector<Promise<int>> among;

    for (int at = 0; at < many; ++at)
        among.push_back(run(cpu(), [at] { return run(cpu(), [at] { return at; }).get() + 1; }));

    const std::vector<int> got = all(std::move(among)).timeout(5s).get();

    for (int at = 0; at < many; ++at) SAME(got[static_cast<std::size_t>(at)], at + 1);
}

// A callback that blocks in `get` on something whose continuation waits in the same thread's queue
// does not wait for itself.
TRIAL("get: inside a callback, waiting for a chain that ends on this thread does not deadlock") {
    Promise<int> one = ready(1).then([](int got) { return ready(got).then([](int more) { return more + 1; }).get(); });

    SAME(std::move(one).get(), 2);
}

// --- Coroutines -------------------------------------------------------------------------------

namespace {

// Adds two values that come from the pool.
Promise<int> added(int one, int two) {
    const int first = co_await run(io(), [one] { return one; });
    const int second = co_await run(io(), [two] { return two; });

    co_return first + second;
}

// Throws instead of giving its value.
int thrown() { throw std::runtime_error{"w korutynie"}; }

// Fails after one step. The coroutine ends with `co_return`, whose value throws: a coroutine of a
// value that only throws gets the warning of a function that returns nothing from MSVC (C4033).
Promise<int> failing() {
    co_await ready();

    co_return thrown();
}

// Counts up `many` times through promises that are already settled.
Promise<int> counted(int many) {
    int sum = 0;

    for (int at = 0; at < many; ++at) sum += co_await ready(1);

    co_return sum;
}

// Waits for `what` and notes whether it went on with `Cancelled` on the thread `mine`.
Promise<void> resumed(Promise<int> what, std::thread::id mine, std::atomic<bool>& home, std::atomic<bool>& done) {
    try {
        (void)co_await std::move(what);
    } catch (const Cancelled&) {
        home.store(std::this_thread::get_id() == mine);
    }

    done.store(true);
}

// Waits for `what` and notes whether it was cancelled there.
Promise<void> waited(Promise<int> what, std::atomic<bool>& cancelled, std::atomic<bool>& done) {
    try {
        (void)co_await std::move(what);
    } catch (const Cancelled&) {
        cancelled.store(true);
    }

    done.store(true);
}

}

TRIAL("coroutine: co_await gives the value, co_return ends the promise") {
    SAME(added(2, 3).get(), 5);
}

TRIAL("coroutine: an error thrown inside fails the promise") {
    std::string said;

    try {
        (void)failing().get();
    } catch (const std::exception& why) {
        said = why.what();
    }

    SAME(said, std::string{"w korutynie"});
}

// A loop in a coroutine is a loop, not a chain: a hundred thousand turns in constant memory and on
// a constant stack.
TRIAL("coroutine: a long loop of awaits") {
    SAME(counted(100000).get(), 100000);
}

// When the owner leaves, the coroutine resumes with `Cancelled` where it waited, and what it waited
// for is stopped.
TRIAL("coroutine: abandoned while waiting, it resumes with Cancelled and stops what it waited for") {
    auto [inner, settle] = promised<int>();

    std::atomic<bool> cancelled{false};
    std::atomic<bool> done{false};

    Promise<void> one = waited(std::move(inner), cancelled, done);

    one.cancel();

    OUGHT(risen(done));
    OUGHT(cancelled.load());
    OUGHT(settle.stop().stopped());
}

// A coroutine that waits for a promise after `here` and is abandoned resumes on the loop, where it
// would have resumed with the ending, not on the pool.
TRIAL("coroutine: abandoned while waiting after here, it resumes on the loop") {
    Loop loop;

    auto [inner, settle] = promised<int>();

    std::atomic<bool> home{false};
    std::atomic<bool> done{false};

    Promise<void> one = resumed(std::move(inner).here(), std::this_thread::get_id(), home, done);

    one.cancel();

    OUGHT(turned(loop, done));
    OUGHT(home.load());
}

// --- An executor that ends -----------------------------------------------------------------------

// An executor that ends with work of promises in its queue throws it away, and each chain that
// waited for it ends cancelled at once instead of never: no `then` or `recover` runs, only the
// cleanup (`finally`, a coroutine going on with `Cancelled`), on the loop's thread, while the loop
// goes away. The work before the loop comes from a manual executor turned on this thread, so that
// it has fully finished when the loop ends.

TRIAL("Loop: an ending loop abandons the chains it holds: they end cancelled, and finally runs on its thread") {
    ManualExecutor before;

    const std::thread::id mine = std::this_thread::get_id();

    std::atomic<bool> ran{false};
    std::atomic<bool> cleaned{false};
    std::atomic<bool> home{false};

    std::optional<Promise<int>> tail;

    {
        Loop loop;

        tail.emplace(run(before, [] { return 1; })
                         .here()
                         .then([&ran](int got) {
                             ran.store(true);

                             return got;
                         })
                         .finally([&cleaned, &home, mine] {
                             home.store(std::this_thread::get_id() == mine);
                             cleaned.store(true);
                         }));

        SAME(before.turn(), std::size_t{1});
        SAME(loop.waiting(), std::size_t{1});
    }

    OUGHT(tail->ready());

    // A chain left hanging would make the wait below hang the trial too.
    if (!tail->ready()) return;

    OUGHT(!ran.load());
    OUGHT(cleaned.load());
    OUGHT(home.load());

    const Outcome<int> got = std::move(*tail).outcome().get();

    OUGHT(got.cancelled());
}

TRIAL("Loop: an ending loop resumes a coroutine waiting there with Cancelled, on its thread") {
    ManualExecutor before;

    std::atomic<bool> home{false};
    std::atomic<bool> done{false};

    std::optional<Promise<void>> one;

    {
        Loop loop;

        one.emplace(resumed(run(before, [] { return 1; }).here(), std::this_thread::get_id(), home, done));

        SAME(before.turn(), std::size_t{1});
        OUGHT(!done.load());
    }

    OUGHT(done.load());
    OUGHT(home.load());
    OUGHT(one->ready());
}

TRIAL("Loop: an ending loop cancels a deadline and a continuation it was to deliver") {
    ManualClock clock;

    std::atomic<bool> recovered{false};
    std::atomic<bool> ran{false};

    std::optional<Promise<int>> late;
    std::optional<Promise<void>> hung;

    auto [made, settle] = promised<int>();

    {
        Loop loop;

        late.emplace(std::move(made).here().timeout(50ms, clock).recover([&recovered](const std::exception&) {
            recovered.store(true);

            return 0;
        }));

        Promise<int> ended = run(io(), [] { return 2; }).here();

        UNTIL(loop, ended.ready());

        hung.emplace(std::move(ended).then([&ran](int) { ran.store(true); }));

        clock.advance(50ms);

        OUGHT(loop.waiting() >= 2);
    }

    OUGHT(late->ready());
    OUGHT(hung->ready());

    if (!late->ready() || !hung->ready()) return;

    OUGHT(!recovered.load());
    OUGHT(std::move(*late).outcome().get().cancelled());

    OUGHT(!ran.load());
    OUGHT(std::move(*hung).outcome().get().cancelled());
}

// A `finally` waiting for its own task on an executor that ends runs then, and the chain after it
// ends cancelled; one whose chain the owner gave up before runs too, on the loop's thread while the
// loop goes away; and what it throws, with nowhere to go, reaches the sink of `lost`.
TRIAL("finally: an executor that ends runs the cleanup it held, also one given up before, and loses what it throws") {
    ManualExecutor before;

    {
        auto executor = std::make_unique<ManualExecutor>();

        std::atomic<bool> cleaned{false};

        Promise<int> tail = run(before, [] { return 1; }).on(*executor).finally([&cleaned] { cleaned.store(true); });

        SAME(before.turn(), std::size_t{1});
        SAME(executor->turn(), std::size_t{1});
        SAME(executor->waiting(), std::size_t{1});
        OUGHT(!cleaned.load());

        executor.reset();

        OUGHT(cleaned.load());
        OUGHT(tail.ready());

        if (tail.ready()) OUGHT(std::move(tail).outcome().get().cancelled());
    }

    {
        const std::thread::id mine = std::this_thread::get_id();

        std::atomic<bool> cleaned{false};
        std::atomic<bool> home{false};

        {
            Loop loop;

            Promise<int> tail = run(before, [] { return 1; }).here().finally([&cleaned, &home, mine] {
                home.store(std::this_thread::get_id() == mine);
                cleaned.store(true);
            });

            SAME(before.turn(), std::size_t{1});

            tail.cancel();

            OUGHT(!cleaned.load());
        }

        OUGHT(cleaned.load());
        OUGHT(home.load());
    }

    {
        std::atomic<bool> seen{false};

        lost([&seen](const std::exception& why) {
            if (std::string_view{why.what()} == "sprzątanie") seen.store(true);
        });

        auto executor = std::make_unique<ManualExecutor>();

        Promise<int> tail = run(before, [] { return 1; }).on(*executor).finally([] { throw std::runtime_error{"sprzątanie"}; });

        SAME(before.turn(), std::size_t{1});
        SAME(executor->turn(), std::size_t{1});

        executor.reset();

        OUGHT(risen(seen));
        OUGHT(tail.ready());

        if (tail.ready()) OUGHT(std::move(tail).outcome().get().cancelled());

        lost({});
    }
}

// A coroutine that waits for a promise that has already ended on the loop resumes there; when the
// loop ends first, it goes on with `Cancelled`, on the loop's thread.
TRIAL("coroutine: waiting for what ended on a loop that then ends, it resumes with Cancelled there") {
    ManualExecutor before;

    std::atomic<bool> home{false};
    std::atomic<bool> done{false};

    std::optional<Promise<void>> one;

    {
        Loop loop;

        Promise<int> ended = run(before, [] { return 1; }).here();

        SAME(before.turn(), std::size_t{1});
        SAME(loop.turn(), std::size_t{1});
        OUGHT(ended.ready());

        one.emplace(resumed(std::move(ended), std::this_thread::get_id(), home, done));

        OUGHT(!done.load());
    }

    OUGHT(done.load());
    OUGHT(home.load());
}

TRIAL("ManualExecutor: one that ends cancels the bodies it was to run") {
    auto executor = std::make_unique<ManualExecutor>();

    std::atomic<bool> ran{false};

    Promise<int> one = run(*executor, [&ran] {
        ran.store(true);

        return 1;
    });

    executor.reset();

    OUGHT(one.ready());

    if (!one.ready()) return;

    OUGHT(!ran.load());
    OUGHT(std::move(one).outcome().get().cancelled());
}

// --- Long chains ------------------------------------------------------------------------------

// Settling, cancelling and freeing a chain do not descend the stack, link by link.
TRIAL("chain: two hundred thousand links settle and are freed without a deep stack") {
    std::atomic<int> passed{0};

    {
        auto [source, settle] = promised<void>();

        Promise<void> chain = std::move(source);

        for (int at = 0; at < 200000; ++at) chain = std::move(chain).then([&passed] { passed.fetch_add(1); });

        (void)settle.value();

        std::move(chain).get();
    }

    SAME(passed.load(), 200000);
}

#ifndef _WIN32

// Teardown and cancelling on a thread with a small stack, 256 KB, a quarter of the Windows one.
TRIAL("chain: building, settling and cancelling a long chain fit in a small stack") {
    struct Job {
        int built = 0;
    };

    Job job;

    const auto work = [](void* given) -> void* {
        auto* mine = static_cast<Job*>(given);

        {
            auto [source, settle] = promised<void>();

            Promise<void> chain = std::move(source);

            for (int at = 0; at < 50000; ++at) chain = std::move(chain).then([] {});

            (void)settle.value();

            std::move(chain).get();
        }

        {
            auto [source, settle] = promised<void>();

            Promise<void> idle = std::move(source);

            for (int at = 0; at < 50000; ++at) idle = std::move(idle).then([] {});

            idle.cancel();
        }

        mine->built = 50000;

        return nullptr;
    };

    pthread_attr_t shape;
    pthread_attr_init(&shape);
    pthread_attr_setstacksize(&shape, 256 * 1024);

    pthread_t thread;

    OUGHT(pthread_create(&thread, &shape, work, &job) == 0);

    pthread_join(thread, nullptr);
    pthread_attr_destroy(&shape);

    SAME(job.built, 50000);
}

#endif

// --- Pool -------------------------------------------------------------------------------------

namespace {

// The highest number of jobs running at the same time.
struct Highest {
    std::atomic<int> now{0};
    std::atomic<int> most{0};

    // Counts a job in and raises the highest number seen when needed.
    void entered() {
        const int here = ++now;

        int seen = most.load();
        while (here > seen && !most.compare_exchange_weak(seen, here)) {
        }
    }

    // Counts a job out.
    void left() { --now; }
};

// What a closure holds; its destructor comes back to the pool with more work.
class Returning {
public:
    Returning(Pool& into, std::atomic<int>& counted) : into_(into), counted_(counted) {}

    // Posts one more job to the pool that the closure was dropped from.
    ~Returning() {
        std::atomic<int>& counted = counted_;

        (void)into_.post([&counted] { counted += 100; });
    }

    Returning(const Returning&) = delete;
    Returning& operator=(const Returning&) = delete;
    Returning(Returning&&) = delete;
    Returning& operator=(Returning&&) = delete;

private:
    Pool& into_;
    std::atomic<int>& counted_;
};

}

TRIAL("Pool: an own pool takes work and can be drained") {
    Pool mine;

    std::atomic<int> counted{0};

    for (int at = 0; at < 24; ++at) (void)mine.post([&counted] { ++counted; });

    mine.drain();

    SAME(counted.load(), 24);
    OUGHT(mine.staff() >= 1);
}

TRIAL("Pool: workers come back instead of multiplying without end") {
    Pool mine;

    for (int at = 0; at < 50; ++at) {
        (void)mine.post([] {});
        mine.drain();
    }

    OUGHT(mine.staff() <= 2);
}

TRIAL("Pool: computing work does not exceed the ceiling, through promises too") {
    Pool mine{3};

    Highest counted;

    std::vector<Promise<void>> among;

    for (int at = 0; at < 24; ++at)
        among.push_back(run(mine.cpu(), [&counted] {
            counted.entered();
            std::this_thread::sleep_for(20ms);
            counted.left();
        }));

    all(std::move(among)).get();

    OUGHT(counted.most.load() <= 3);
    OUGHT(counted.most.load() >= 2);
}

TRIAL("Pool: ordinary work knows no ceiling") {
    Pool mine{2};

    Highest counted;

    for (int at = 0; at < 12; ++at)
        (void)mine.post([&counted] {
            counted.entered();
            std::this_thread::sleep_for(40ms);
            counted.left();
        });

    mine.drain();

    OUGHT(counted.most.load() > 2);
}

// A burst of blocking work leaves many workers behind. Those above the ceiling leave after their
// rest, each with its thread and its bell, and the pool still takes work afterwards.
TRIAL("Pool: idle workers above the ceiling leave after their rest") {
    Pool mine{1, 30ms};

    for (int at = 0; at < 6; ++at) (void)mine.post([] { std::this_thread::sleep_for(40ms); });

    mine.drain();

    OUGHT(mine.staff() >= 2);

    SOON(mine.staff() <= 1);

    SAME(mine.staff(), std::size_t{1});

    std::atomic<int> counted{0};

    for (int at = 0; at < 4; ++at) (void)mine.post([&counted] { ++counted; });

    mine.drain();

    SAME(counted.load(), 4);
}

// Workers stuck in blocking work do not use up the computing ceiling. The only worker, made for
// computing work, then blocks on ordinary work, and a computing task posted meanwhile must get a
// worker of its own instead of waiting for the blocked one.
TRIAL("Pool: computing work runs while blocking work holds every worker") {
    Pool mine{1};

    (void)mine.post([] {}, Lane::cpu);

    mine.drain();

    Holding blocking;

    (void)mine.post([&blocking] { blocking(); });

    OUGHT(risen(blocking.held));

    std::atomic<bool> done{false};

    (void)mine.post([&done] { done.store(true); }, Lane::cpu);

    const bool timely = risen(done);

    blocking.released.store(true);

    mine.drain();

    OUGHT(timely);
}

// A dropped job's closure is destroyed after the pool lets go of its lock: what it holds may come
// back to the pool in its destructor, here with more work, and under the lock that would deadlock.
// The jobs left keep the order in which they were posted.
TRIAL("Pool: a dropped job is let go outside the lock, and the rest keep their order") {
    Pool mine{1};

    Holding blocking;

    (void)mine.post([&blocking] { blocking(); }, Lane::cpu);

    OUGHT(risen(blocking.held));

    std::mutex guard;
    std::vector<int> order;
    std::vector<Pool::Job> jobs;

    for (int at = 0; at < 6; ++at)
        jobs.push_back(mine.post(
            [&guard, &order, at] {
                const std::lock_guard<std::mutex> kept{guard};

                order.push_back(at);
            },
            Lane::cpu));

    std::atomic<int> counted{0};

    auto returning = std::make_shared<Returning>(mine, counted);

    const Pool::Job doomed = mine.post([returning] {}, Lane::cpu);

    returning.reset();

    OUGHT(mine.drop(doomed));
    OUGHT(mine.drop(jobs[1]));
    OUGHT(mine.drop(jobs[4]));
    OUGHT(!mine.drop(jobs[4]));

    blocking.released.store(true);

    mine.drain();

    SAME(order, (std::vector<int>{0, 2, 3, 5}));
    SAME(counted.load(), 100);
}

// After draining nobody holds a job's closure any more: whatever it captured may go.
TRIAL("Pool: after draining nobody holds the job's closure") {
    Pool mine;

    auto held = std::make_shared<int>(1);

    for (int at = 0; at < 10; ++at) (void)mine.post([held] {});

    mine.drain();

    SAME(held.use_count(), 1L);
}

#ifndef _WIN32

namespace {

// Takes every free descriptor under a lowered limit for the length of a trial and gives them all
// back at its end, so that nothing new (a pipe for a bell) can be opened meanwhile.
class Starving {
public:
    // Lowers the limit and fills it up.
    Starving() {
        held_ = ::getrlimit(RLIMIT_NOFILE, &was_) == 0;

        if (!held_) return;

        rlimit now = was_;

        now.rlim_cur = std::min<rlim_t>(was_.rlim_cur, 512);

        held_ = ::setrlimit(RLIMIT_NOFILE, &now) == 0;

        if (!held_) return;

        for (int again = 0; again < 1024; ++again) {
            const int one = ::open("/dev/null", O_RDONLY | O_CLOEXEC);

            if (one < 0) break;

            taken_.push_back(one);
        }
    }

    // Closes what it took and restores the limit.
    ~Starving() {
        for (const int one : taken_) ::close(one);

        if (held_) (void)::setrlimit(RLIMIT_NOFILE, &was_);
    }

    Starving(const Starving&) = delete;
    Starving& operator=(const Starving&) = delete;
    Starving(Starving&&) = delete;
    Starving& operator=(Starving&&) = delete;

    // Whether the limit could be lowered.
    [[nodiscard]] bool held() const { return held_; }

private:
    rlimit was_{};
    bool held_ = false;
    std::vector<int> taken_;
};

}

// A worker's bell is a pair of descriptors. Without a free one, a pool that has no worker refuses
// the work to whoever posts it, and a pool that has a worker keeps the work until that worker is
// free; neither ends the program from a worker thread. A promise on a refusing pool ends cancelled.
// Under UBSan the check of dynamic types reports "invalid vptr" here, with "memory cannot be
// printed": its test of whether memory can be read goes through a pipe, and the trial has taken
// every descriptor. The objects are whole; without the starving the same calls report nothing.
TRIAL("Pool: without descriptors work is refused or waits, and the program goes on") {
    Pool fresh;
    Pool working{1};

    Holding blocking;

    (void)working.post([&blocking] { blocking(); });

    OUGHT(risen(blocking.held));

    std::atomic<int> counted{0};

    std::optional<Promise<void>> refused;

    {
        const Starving starving;

        OUGHT(starving.held());

        THREW((void)fresh.post([&counted] { ++counted; }));

        refused.emplace(run(fresh.cpu(), [&counted] { ++counted; }));

        (void)working.post([&counted] { counted += 10; });
    }

    blocking.released.store(true);

    working.drain();

    SAME(counted.load(), 10);

    THREW(std::move(*refused).get());

    (void)fresh.post([&counted] { ++counted; });

    fresh.drain();

    SAME(counted.load(), 11);
}

#endif

TRIAL("Pool: work posted from many threads all runs") {
    std::atomic<int> counted{0};

    std::vector<std::thread> posting;

    for (int who = 0; who < 8; ++who)
        posting.emplace_back([&counted] {
            for (int at = 0; at < 100; ++at) run(io(), [&counted] { ++counted; }).detach();
        });

    for (std::thread& one : posting) one.join();

    OUGHT(reached(counted, 800));
}
