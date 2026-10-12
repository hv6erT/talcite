#include "proving.hpp"

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
#include <thread>
#include <tuple>
#include <vector>

#include "talcite/bell.hpp"
#include "talcite/clock.hpp"
#include "talcite/loop.hpp"
#include "talcite/place.hpp"
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

// Waits until the flag is up, at most five seconds; returns whether it came up.
bool risen(const std::atomic<bool>& flag) {
    for (int again = 0; again < 500 && !flag.load(); ++again) std::this_thread::sleep_for(10ms);

    return flag.load();
}

// Waits until the counter reaches `want`, at most five seconds; returns whether it did.
bool reached(const std::atomic<int>& counter, int want) {
    for (int again = 0; again < 500 && counter.load() < want; ++again) std::this_thread::sleep_for(10ms);

    return counter.load() >= want;
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

    ought(risen(ran));
    same(std::move(one).get(), 7);
}

TRIAL("run: get gives the value and leaves the handle empty") {
    Promise<std::string> one = run(io(), [] { return std::string{"siedem"}; });

    ought(static_cast<bool>(one));
    same(std::move(one).get(), std::string{"siedem"});
    ought(!static_cast<bool>(one));
}

TRIAL("run: what the body throws comes back from get as it was") {
    Promise<int> one = run(io(), []() -> int { throw std::out_of_range{"za daleko"}; });

    std::string said;

    try {
        (void)std::move(one).get();
    } catch (const std::out_of_range& why) {
        said = why.what();
    }

    same(said, std::string{"za daleko"});
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

    ought(rejected);
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

    ought(cancelled);
    ought(!recovered.load());
}

TRIAL("run: a body may take its work, and its stop is the current one") {
    std::atomic<bool> matched{false};

    Promise<void> one = run(io(), [&matched](Work& work) { matched.store(Stop::current() == &work.stop()); });

    std::move(one).get();

    ought(matched.load());
    ought(Stop::current() == nullptr);
}

TRIAL("run: a body that returns a promise is adopted") {
    Promise<int> one = run(io(), [] { return run(io(), [] { return 41; }).then([](int got) { return got + 1; }); });

    same(std::move(one).get(), 42);
}

TRIAL("ready and failed: already settled") {
    Promise<int> one = ready(5);

    ought(one.ready());
    same(std::move(one).get(), 5);

    Promise<void> none = ready();

    ought(none.ready());
    std::move(none).get();

    Promise<int> bad = failed<int>(std::make_exception_ptr(std::runtime_error{"nie"}));

    threw((void)std::move(bad).get());
}

TRIAL("Promise: an empty handle refuses everything that needs a promise") {
    Promise<int> empty;

    ought(!static_cast<bool>(empty));
    ought(!empty.ready());
    same(empty.progress(), (Progress{}));

    bool refused = false;

    try {
        (void)std::move(empty).then([](int got) { return got; });
    } catch (const std::logic_error&) {
        refused = true;
    }

    ought(refused);

    empty.cancel();
}

// --- The chain --------------------------------------------------------------------------------

TRIAL("then: passes the value on and changes its type") {
    Promise<std::string> one =
        ready(20).then([](int got) { return got + 1; }).then([](int got) { return std::to_string(got); });

    same(std::move(one).get(), std::string{"21"});
}

TRIAL("then: after void the callback takes nothing, and a void callback gives void") {
    std::atomic<int> seen{0};

    Promise<void> one = ready().then([] { return 3; }).then([&seen](int got) { seen.store(got); });

    std::move(one).get();

    same(seen.load(), 3);
}

TRIAL("then: a promise returned from the callback is adopted, a shared one too") {
    Promise<int> one = ready(1).then([](int got) { return run(io(), [got] { return got * 10; }); });

    same(std::move(one).get(), 10);

    SharedPromise<int> shared = ready(4).share();

    Promise<int> two = ready().then([shared] { return shared; });

    same(std::move(two).get(), 4);
}

TRIAL("then: an error skips the callbacks and reaches the end") {
    std::atomic<bool> ran{false};

    Promise<int> one = run(io(), []() -> int { throw std::runtime_error{"zepsute"}; }).then([&ran](int got) {
        ran.store(true);

        return got;
    });

    threw((void)std::move(one).get());

    ought(!ran.load());
}

TRIAL("then: what the callback throws fails the next promise") {
    Promise<int> one = ready(1).then([](int) -> int { throw std::runtime_error{"w środku"}; });

    std::string said;

    try {
        (void)std::move(one).get();
    } catch (const std::exception& why) {
        said = why.what();
    }

    same(said, std::string{"w środku"});
}

// A callback for something that has already ended does not run inside the call that registers it:
// the caller may still be setting up what the callback touches.
TRIAL("then: on a settled promise the callback never runs inside then") {
    const std::thread::id mine = std::this_thread::get_id();

    std::atomic<bool> inside{true};

    Promise<void> one = ready(1).then([&inside, mine](int) { inside.store(std::this_thread::get_id() == mine); });

    std::move(one).get();

    ought(!inside.load());
}

// A callback may hold what can only be moved, a promise or a settle, since the node keeps it.
TRIAL("then: the callback may hold a promise") {
    Promise<int> inner = ready(5);

    Promise<int> one = ready(1).then([inner = std::move(inner)](int got) mutable { return std::move(inner).get() + got; });

    same(std::move(one).get(), 6);
}

// A value that can only be moved goes through a promise that is not shared: through `then`, out of
// `get`, from a settle and inside an outcome.
TRIAL("then: a value that can only be moved passes through") {
    Promise<int> one = ready(std::make_unique<int>(7)).then([](std::unique_ptr<int> got) { return *got + 1; });

    same(std::move(one).get(), 8);

    auto [two, settle] = promised<std::unique_ptr<int>>();

    ought(settle.value(std::make_unique<int>(3)));
    same(*std::move(two).get(), 3);

    Outcome<std::unique_ptr<int>> three = ready(std::make_unique<int>(5)).outcome().get();

    ought(three.ok());
    same(*std::move(three).value(), 5);
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

    ought(!kept->value(std::shared_ptr<int>{watched}));
    same(watched.use_count(), 1L);
}

TRIAL("recover: turns an error into a value or into a promise") {
    Promise<int> one = failed<int>(std::make_exception_ptr(std::runtime_error{"nie"})).recover([](const std::exception& why) {
        return std::string{why.what()} == "nie" ? 9 : 0;
    });

    same(std::move(one).get(), 9);

    Promise<int> two = failed<int>(std::make_exception_ptr(std::runtime_error{"nie"})).recover([](const std::exception&) {
        return run(io(), [] { return 8; });
    });

    same(std::move(two).get(), 8);
}

TRIAL("recover: a value passes untouched, and what recover throws is the new error") {
    std::atomic<bool> ran{false};

    Promise<int> one = ready(3).recover([&ran](const std::exception&) {
        ran.store(true);

        return 0;
    });

    same(std::move(one).get(), 3);
    ought(!ran.load());

    Promise<int> two = failed<int>(std::make_exception_ptr(std::runtime_error{"pierwszy"})).recover([](const std::exception&) -> int {
        throw std::runtime_error{"drugi"};
    });

    std::string said;

    try {
        (void)std::move(two).get();
    } catch (const std::exception& why) {
        said = why.what();
    }

    same(said, std::string{"drugi"});
}

TRIAL("finally: runs after a value and after an error, and the ending passes through") {
    std::atomic<int> ran{0};

    Promise<int> one = ready(4).finally([&ran] { ++ran; });

    same(std::move(one).get(), 4);

    Promise<int> two = failed<int>(std::make_exception_ptr(std::runtime_error{"nie"})).finally([&ran] { ++ran; });

    threw((void)std::move(two).get());

    same(ran.load(), 2);
}

TRIAL("finally: what it throws becomes the error") {
    Promise<int> one = ready(4).finally([] { throw std::runtime_error{"sprzątanie"}; });

    std::string said;

    try {
        (void)std::move(one).get();
    } catch (const std::exception& why) {
        said = why.what();
    }

    same(said, std::string{"sprzątanie"});
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

    ought(settle.stop().stopped());

    std::this_thread::sleep_for(50ms);

    ought(!cleaned.load());

    (void)settle.value(1);

    ought(risen(cleaned));
    ought(other.load());
}

// A chain moved to a place (a loop) cleans up there after a cancellation too, so that code bound to
// that place's thread never runs on another one.
TRIAL("finally: after a cancellation it runs on the place the chain moved to") {
    ManualPlace place;

    auto [source, settle] = promised<int>();

    std::atomic<bool> cleaned{false};

    Promise<int> one = std::move(source).on(place).finally([&cleaned] { cleaned.store(true); });

    one.cancel();

    (void)settle.value(1);

    for (int again = 0; again < 500 && place.waiting() == 0; ++again) std::this_thread::sleep_for(10ms);

    ought(!cleaned.load());

    (void)place.turn();

    ought(cleaned.load());
}

TRIAL("outcome: gives the ending as a value") {
    Outcome<int> good = ready(2).outcome().get();

    ought(good.ok());
    same(good.value(), 2);

    Outcome<int> bad = failed<int>(std::make_exception_ptr(std::runtime_error{"nie"})).outcome().get();

    ought(!bad.ok());
    ought(!bad.cancelled());
    ought(bad.error() != nullptr);
    threw((void)bad.value());

    Outcome<int> gone = run(io(), []() -> int { throw Cancelled{}; }).outcome().get();

    ought(gone.cancelled());
}

// --- Owners -----------------------------------------------------------------------------------

TRIAL("owners: destroying the tail stops the source") {
    auto [source, settle] = promised<int>();

    {
        Promise<int> tail = std::move(source).then([](int got) { return got; }).then([](int got) { return got + 1; });

        ought(!settle.stop().stopped());
    }

    ought(settle.stop().stopped());
    ought(!settle.value(3));
}

TRIAL("owners: cancelling the tail stops a body that waits") {
    std::atomic<bool> heard{false};
    std::atomic<bool> began{false};

    Promise<void> tail = run(io(), [&heard, &began](Work& work) {
                             began.store(true);

                             heard.store(work.stop().wait(5s));
                         }).then([] {});

    ought(risen(began));

    tail.cancel();

    ought(risen(heard));
}

TRIAL("owners: a body cancelled before it starts never runs") {
    ManualPlace place;

    std::atomic<bool> ran{false};

    Promise<void> one = run(place, [&ran] { ran.store(true); });

    one.cancel();

    same(place.turn(), std::size_t{1});

    ought(!ran.load());
}

// A pool lane takes a job back when its owners leave before it starts: it does not wait in the queue
// for a worker to find it abandoned.
TRIAL("owners: a job abandoned before it starts leaves the pool queue at once") {
    Pool mine{1};

    Holding blocking;

    (void)mine.post([&blocking] { blocking(); }, Lane::cpu);

    ought(risen(blocking.held));

    std::atomic<bool> ran{false};

    Promise<void> one = run(mine.cpu(), [&ran] { ran.store(true); });

    one.cancel();

    blocking.released.store(true);

    mine.drain();

    ought(!ran.load());
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

    ought(!settle.stop().stopped());

    (void)settle.fail("zgubione");

    {
        std::unique_lock<std::mutex> held{guard};

        (void)stirred.wait_for(held, 5s, [&] { return !heard.empty(); });
    }

    lost({});

    same(heard, std::string{"zgubione"});
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

    ought(risen(ran));

    std::this_thread::sleep_for(20ms);

    ought(!withdrawn.load());
}

#ifndef _WIN32

TRIAL("owners: the stop's descriptor becomes readable when the work is stopped") {
    auto [source, settle] = promised<int>();

    const Stop::Handle heard = settle.stop().heard();

    pollfd one{heard, POLLIN, 0};

    same(::poll(&one, 1, 0), 0);

    source.cancel();

    same(::poll(&one, 1, 1000), 1);
}

#endif

// --- Producers --------------------------------------------------------------------------------

TRIAL("Settle: the first settling wins, the rest say no") {
    auto [made, settle] = promised<int>();

    ought(settle.value(1));
    ought(!settle.value(2));
    ought(!settle.fail("późno"));

    same(std::move(made).get(), 1);
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

    ought(broken);
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

    same(std::move(after).get(), 2);
}

TRIAL("Settle: void settles without a value") {
    auto [made, settle] = promised<void>();

    ought(settle.value());

    std::move(made).get();
}

// --- Places -----------------------------------------------------------------------------------

TRIAL("on: the rest of the chain runs on the given place, at its turn") {
    ManualPlace place;

    std::atomic<bool> ran{false};

    Promise<void> one = ready(1).on(place).then([&ran](int) { ran.store(true); });

    for (int again = 0; again < 500 && place.waiting() == 0; ++again) std::this_thread::sleep_for(10ms);

    ought(!ran.load());
    same(place.turn(), std::size_t{1});
    ought(ran.load());
    ought(one.ready());
}

TRIAL("on: a place that is gone cancels the rest") {
    auto place = std::make_unique<ManualPlace>();

    auto [made, settle] = promised<int>();

    Promise<int> one = std::move(made).on(*place);

    place.reset();

    (void)settle.value(1);

    threw((void)std::move(one).get());
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

        for (int again = 0; again < 500 && !ran.load(); ++again) {
            (void)loop.turn();

            std::this_thread::sleep_for(5ms);
        }

        ought(ran.load());
        ought(home.load());
    }

    Promise<int> kept = ready(1);

    bool refused = false;

    try {
        (void)std::move(kept).here();
    } catch (const std::logic_error&) {
        refused = true;
    }

    ought(refused);
    ought(static_cast<bool>(kept));
}

// --- Time -------------------------------------------------------------------------------------

TRIAL("later: ends when the clock gets there, not before") {
    ManualClock clock;

    Promise<void> one = later(100ms, clock);

    clock.advance(99ms);

    std::this_thread::sleep_for(20ms);

    ought(!one.ready());

    clock.advance(1ms);

    std::move(one).get();
}

TRIAL("later: abandoned, it leaves the clock at once") {
    ManualClock clock;

    Promise<void> one = later(1h, clock);

    same(clock.waiting(), std::size_t{1});

    one.cancel();

    same(clock.waiting(), std::size_t{0});
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

    ought(overdue);
    ought(settle.stop().wait(5s));
}

TRIAL("timeout: one in time gives its value and leaves the clock") {
    ManualClock clock;

    auto [made, settle] = promised<int>();

    Promise<int> one = std::move(made).timeout(50ms, clock);

    (void)settle.value(6);

    same(std::move(one).get(), 6);

    for (int again = 0; again < 500 && clock.waiting() != 0; ++again) std::this_thread::sleep_for(10ms);

    same(clock.waiting(), std::size_t{0});
}

TRIAL("timeout: on the real clock too") {
    Promise<void> one = run(io(), [](Work& work) { (void)work.stop().wait(5s); }).timeout(20ms);

    threw(std::move(one).get());
}

// --- Shared -----------------------------------------------------------------------------------

TRIAL("SharedPromise: every view gets the value, and get gives it by reference") {
    SharedPromise<std::string> shared = run(io(), [] { return std::string{"wspólne"}; }).share();

    Promise<std::string> one = shared.view();
    Promise<std::string> two = shared.view();

    same(std::move(one).get(), std::string{"wspólne"});
    same(std::move(two).get(), std::string{"wspólne"});
    same(shared.get(), std::string{"wspólne"});
    same(shared.view().get(), std::string{"wspólne"});
}

TRIAL("SharedPromise: a view that gives up takes nothing from the others") {
    auto [made, settle] = promised<int>();

    SharedPromise<int> shared = std::move(made).share();

    Promise<int> impatient = shared.view();
    Promise<int> patient = shared.view();

    impatient.cancel();

    ought(!settle.stop().stopped());

    (void)settle.value(5);

    same(std::move(patient).get(), 5);
}

TRIAL("SharedPromise: when every owner leaves, the work stops") {
    auto [made, settle] = promised<int>();

    {
        SharedPromise<int> shared = std::move(made).share();
        SharedPromise<int> copy = shared;

        Promise<int> one = shared.view();

        shared.cancel();
        one.cancel();

        ought(!settle.stop().stopped());
    }

    ought(settle.stop().stopped());
}

// --- Joins ------------------------------------------------------------------------------------

TRIAL("all: the values in the order given, and an empty set at once") {
    std::vector<Promise<int>> among;

    for (int at = 0; at < 5; ++at) among.push_back(run(io(), [at] {
        std::this_thread::sleep_for(std::chrono::milliseconds{(5 - at) * 5});

        return at;
    }));

    same(all(std::move(among)).get(), (std::vector<int>{0, 1, 2, 3, 4}));

    same(all(std::vector<Promise<int>>{}).get(), (std::vector<int>{}));

    all(std::vector<Promise<void>>{}).get();
}

TRIAL("all: the first error fails it and stops the rest") {
    auto [made, settle] = promised<int>();

    std::vector<Promise<int>> among;

    among.push_back(std::move(made));
    among.push_back(failed<int>(std::make_exception_ptr(std::runtime_error{"zepsute"})));

    threw((void)all(std::move(among)).get());

    ought(settle.stop().wait(5s));
}

TRIAL("all: a list of different types, void among them") {
    std::tuple<int, std::monostate, std::string> got = all(ready(1), ready(), ready(std::string{"trzy"})).get();

    same(std::get<0>(got), 1);
    same(std::get<2>(got), std::string{"trzy"});
}

TRIAL("all: void promises") {
    std::atomic<int> ran{0};

    std::vector<Promise<void>> among;

    for (int at = 0; at < 5; ++at) among.push_back(run(io(), [&ran] { ++ran; }));

    all(std::move(among)).get();

    same(ran.load(), 5);
}

TRIAL("any: the first value wins and the rest stop; all failing gives the last error") {
    auto [slow, settle] = promised<int>();

    std::vector<Promise<int>> among;

    among.push_back(failed<int>(std::make_exception_ptr(std::runtime_error{"pierwszy"})));
    among.push_back(std::move(slow));
    among.push_back(ready(3));

    same(any(std::move(among)).get(), 3);

    ought(settle.stop().wait(5s));

    std::vector<Promise<int>> bad;

    bad.push_back(failed<int>(std::make_exception_ptr(std::runtime_error{"jeden"})));
    bad.push_back(failed<int>(std::make_exception_ptr(std::runtime_error{"dwa"})));

    threw((void)any(std::move(bad)).get());

    threw((void)any(std::vector<Promise<int>>{}).get());
}

TRIAL("race: the first ending of any kind wins") {
    auto [slow, settle] = promised<int>();

    std::vector<Promise<int>> among;

    among.push_back(std::move(slow));
    among.push_back(failed<int>(std::make_exception_ptr(std::runtime_error{"szybki"})));

    threw((void)race(std::move(among)).get());

    ought(settle.stop().wait(5s));
}

TRIAL("settled: every ending in the order given, never failing") {
    std::vector<Promise<int>> among;

    among.push_back(ready(1));
    among.push_back(failed<int>(std::make_exception_ptr(std::runtime_error{"nie"})));
    among.push_back(run(io(), []() -> int { throw Cancelled{}; }));

    const std::vector<Outcome<int>> got = settled(std::move(among)).get();

    same(got.size(), std::size_t{3});
    ought(got[0].ok());
    ought(!got[1].ok() && !got[1].cancelled());
    ought(got[2].cancelled());
}

// --- Progress ---------------------------------------------------------------------------------

TRIAL("progress: what the producer reports, through the chain and summed in a join") {
    auto [one, first] = promised<int>();
    auto [two, second] = promised<int>();

    first.progress(Progress{1, 4});
    second.progress(Progress{3, 4});

    Promise<int> chained = std::move(one).then([](int got) { return got; });

    same(chained.progress(), (Progress{1, 4}));

    std::vector<Promise<int>> among;

    among.push_back(std::move(chained));
    among.push_back(std::move(two));

    Promise<std::vector<int>> joined = all(std::move(among));

    same(joined.progress(), (Progress{4, 8}));

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

    ought(risen(told));

    same(one.progress(), (Progress{5, 10}));

    go.store(true);

    for (int again = 0; again < 500 && !one.ready(); ++again) std::this_thread::sleep_for(10ms);

    same(one.progress(), (Progress{10, 10}));
    same(one.progress().fraction(), 1.0);
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

    ought(risen(waiting));

    std::this_thread::sleep_for(20ms);

    outer.cancel();

    ought(risen(left));
    ought(settle.stop().stopped());
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

    for (int at = 0; at < many; ++at) same(got[static_cast<std::size_t>(at)], at + 1);
}

// A callback that blocks in `get` on something whose continuation waits in the same thread's queue
// does not wait for itself.
TRIAL("get: inside a callback, waiting for a chain that ends on this thread does not deadlock") {
    Promise<int> one = ready(1).then([](int got) { return ready(got).then([](int more) { return more + 1; }).get(); });

    same(std::move(one).get(), 2);
}

// --- Coroutines -------------------------------------------------------------------------------

namespace {

// Adds two values that come from the pool.
Promise<int> added(int one, int two) {
    const int first = co_await run(io(), [one] { return one; });
    const int second = co_await run(io(), [two] { return two; });

    co_return first + second;
}

// Fails after one step.
Promise<int> failing() {
    co_await ready();

    throw std::runtime_error{"w korutynie"};
}

// Counts up `many` times through promises that are already settled.
Promise<int> counted(int many) {
    int sum = 0;

    for (int at = 0; at < many; ++at) sum += co_await ready(1);

    co_return sum;
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
    same(added(2, 3).get(), 5);
}

TRIAL("coroutine: an error thrown inside fails the promise") {
    std::string said;

    try {
        (void)failing().get();
    } catch (const std::exception& why) {
        said = why.what();
    }

    same(said, std::string{"w korutynie"});
}

// A loop in a coroutine is a loop, not a chain: a hundred thousand turns in constant memory and on
// a constant stack.
TRIAL("coroutine: a long loop of awaits") {
    same(counted(100000).get(), 100000);
}

// When the owner leaves, the coroutine resumes with `Cancelled` where it waited, and what it waited
// for is stopped.
TRIAL("coroutine: abandoned while waiting, it resumes with Cancelled and stops what it waited for") {
    auto [inner, settle] = promised<int>();

    std::atomic<bool> cancelled{false};
    std::atomic<bool> done{false};

    Promise<void> one = waited(std::move(inner), cancelled, done);

    one.cancel();

    ought(risen(done));
    ought(cancelled.load());
    ought(settle.stop().stopped());
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

    same(passed.load(), 200000);
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

    ought(pthread_create(&thread, &shape, work, &job) == 0);

    pthread_join(thread, nullptr);
    pthread_attr_destroy(&shape);

    same(job.built, 50000);
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

    same(counted.load(), 24);
    ought(mine.staff() >= 1);
}

TRIAL("Pool: workers come back instead of multiplying without end") {
    Pool mine;

    for (int at = 0; at < 50; ++at) {
        (void)mine.post([] {});
        mine.drain();
    }

    ought(mine.staff() <= 2);
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

    ought(counted.most.load() <= 3);
    ought(counted.most.load() >= 2);
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

    ought(counted.most.load() > 2);
}

// A burst of blocking work leaves many workers behind. Those above the ceiling leave after their
// rest, each with its thread and its bell, and the pool still takes work afterwards.
TRIAL("Pool: idle workers above the ceiling leave after their rest") {
    Pool mine{1, 30ms};

    for (int at = 0; at < 6; ++at) (void)mine.post([] { std::this_thread::sleep_for(40ms); });

    mine.drain();

    ought(mine.staff() >= 2);

    for (int again = 0; again < 300 && mine.staff() > 1; ++again) std::this_thread::sleep_for(10ms);

    same(mine.staff(), std::size_t{1});

    std::atomic<int> counted{0};

    for (int at = 0; at < 4; ++at) (void)mine.post([&counted] { ++counted; });

    mine.drain();

    same(counted.load(), 4);
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

    ought(risen(blocking.held));

    std::atomic<bool> done{false};

    (void)mine.post([&done] { done.store(true); }, Lane::cpu);

    const bool timely = risen(done);

    blocking.released.store(true);

    mine.drain();

    ought(timely);
}

// A dropped job's closure is destroyed after the pool lets go of its lock: what it holds may come
// back to the pool in its destructor, here with more work, and under the lock that would deadlock.
// The jobs left keep the order in which they were posted.
TRIAL("Pool: a dropped job is let go outside the lock, and the rest keep their order") {
    Pool mine{1};

    Holding blocking;

    (void)mine.post([&blocking] { blocking(); }, Lane::cpu);

    ought(risen(blocking.held));

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

    ought(mine.drop(doomed));
    ought(mine.drop(jobs[1]));
    ought(mine.drop(jobs[4]));
    ought(!mine.drop(jobs[4]));

    blocking.released.store(true);

    mine.drain();

    same(order, (std::vector<int>{0, 2, 3, 5}));
    same(counted.load(), 100);
}

// After draining nobody holds a job's closure any more: whatever it captured may go.
TRIAL("Pool: after draining nobody holds the job's closure") {
    Pool mine;

    auto held = std::make_shared<int>(1);

    for (int at = 0; at < 10; ++at) (void)mine.post([held] {});

    mine.drain();

    same(held.use_count(), 1L);
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
TRIAL("Pool: without descriptors work is refused or waits, and the program goes on") {
    Pool fresh;
    Pool working{1};

    Holding blocking;

    (void)working.post([&blocking] { blocking(); });

    ought(risen(blocking.held));

    std::atomic<int> counted{0};

    std::optional<Promise<void>> refused;

    {
        const Starving starving;

        ought(starving.held());

        threw((void)fresh.post([&counted] { ++counted; }));

        refused.emplace(run(fresh.cpu(), [&counted] { ++counted; }));

        (void)working.post([&counted] { counted += 10; });
    }

    blocking.released.store(true);

    working.drain();

    same(counted.load(), 10);

    threw(std::move(*refused).get());

    (void)fresh.post([&counted] { ++counted; });

    fresh.drain();

    same(counted.load(), 11);
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

    ought(reached(counted, 800));
}
