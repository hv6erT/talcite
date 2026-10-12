#include "proving.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "talcite/promise.hpp"
#include "talcite/time.hpp"

using namespace talcite;
using namespace std::chrono_literals;

TRIAL("Time: the stopwatch measures what has passed") {
    Stopwatch held;

    std::this_thread::sleep_for(60ms);

    ought(held.spent() >= 50ms);
    ought(held.spent() < 400ms);

    held.reset();

    ought(held.spent() < 50ms);
}

TRIAL("Time: later is fulfilled after the time, not before") {
    Stopwatch held;

    Time::later(120ms).await();

    ought(held.spent() >= 100ms);
    ought(held.spent() < 600ms);
}

TRIAL("Time: later is lazy") {
    Promise<void> waiting = Time::later(40ms);

    std::this_thread::sleep_for(120ms);

    same(waiting.progress(), 0.0);

    Stopwatch held;
    waiting.await();

    ought(held.spent() >= 30ms);
}

TRIAL("Time: the countdown's progress grows") {
    Promise<void> waiting = Time::later(200ms);

    waiting.start();

    std::this_thread::sleep_for(100ms);

    const double part = waiting.progress();

    ought(part > 0.2);
    ought(part < 0.9);

    waiting.await();

    same(waiting.progress(), 1.0);
}

TRIAL("Time: limit fails with an Overdue exception") {
    bool caught = false;

    try {
        Time::limit(40ms).await();
    } catch (const Overdue&) {
        caught = true;
    }

    ought(caught);
}

TRIAL("Time: a stopwatch is not a clock") {
    static_assert(!std::is_default_constructible_v<Time>);
    static_assert(std::is_default_constructible_v<Stopwatch>);
}

TRIAL("timeout: work that made it in time gives its value") {
    Promise<int> quick{[](auto resolve, auto) {
        std::this_thread::sleep_for(20ms);
        resolve(7);
    }};

    same(quick.timeout(800ms).await(), 7);
}

TRIAL("timeout: work that did not make it in time fails with an Overdue exception") {
    Promise<int> slow{[](auto resolve, auto) {
        std::this_thread::sleep_for(600ms);
        resolve(1);
    }};

    Stopwatch held;
    bool caught = false;

    try {
        (void)slow.timeout(60ms).await();
    } catch (const Overdue&) {
        caught = true;
    }

    ought(caught);
    ought(held.spent() < 500ms);
}

TRIAL("timeout: once the time is exceeded the work is cancelled") {
    Promise<void> slow = Time::later(60000ms);

    Promise<void> bounded = slow.timeout(40ms);

    threw(bounded.await());

    bool cancelled = false;

    try {
        slow.await();
    } catch (const Cancelled&) {
        cancelled = true;
    }

    ought(cancelled);
}

TRIAL("timeout: the work's rejection passes through as it is") {
    Promise<int> broken{[](auto, auto reject) { reject("zepsute"); }};

    std::string said;

    try {
        (void)broken.timeout(800ms).await();
    } catch (const Overdue&) {
        said = "overdue";
    } catch (const Rejected& why) {
        said = why.what();
    }

    same(said, std::string{"zepsute"});
}

TRIAL("timeout: the deadline counts from starting, not from composing") {
    Promise<int> quick{[](auto resolve, auto) { resolve(3); }};

    Promise<int> bounded = quick.timeout(40ms);

    std::this_thread::sleep_for(120ms);

    same(bounded.await(), 3);
}

TRIAL("timeout: cancelling the result cancels the work") {
    Promise<void> slow = Time::later(60000ms);

    Promise<void> bounded = slow.timeout(60000ms);

    bounded.start();
    bounded.cancel();

    bool cancelled = false;

    try {
        slow.await();
    } catch (const Cancelled&) {
        cancelled = true;
    }

    ought(cancelled);
}

TRIAL("timeout: fulfilled work removes the deadline, which no longer upsets the result") {
    Promise<int> quick{[](auto resolve, auto) { resolve(5); }};

    Promise<int> bounded = quick.timeout(40ms);

    same(bounded.await(), 5);

    std::this_thread::sleep_for(100ms);

    same(bounded.await(), 5);
}

TRIAL("timeout: works on a link of a chain and cancels it all the way to the source") {
    Promise<void> source = Time::later(60000ms);

    Promise<int> chained = source.then([] { return 1; });

    threw(chained.timeout(40ms).await());

    bool cancelled = false;

    try {
        source.await();
    } catch (const Cancelled&) {
        cancelled = true;
    }

    ought(cancelled);
}

TRIAL("Time: a hundred waits do not cost a hundred threads") {
    std::vector<Promise<void>> among;
    among.reserve(100);

    for (int at = 0; at < 100; ++at) among.push_back(Time::later(60ms));

    Stopwatch held;

    Promise<void>::all(among).await();

    // If every wait slept in its own task, a hundred of them on two cores would drag on much longer
    // than one.
    ought(held.spent() < 900ms);
}

TRIAL("Time: a cancelled wait disappears from the clock") {
    Promise<void> waiting = Time::later(60000ms);

    waiting.start();

    std::this_thread::sleep_for(40ms);

    waiting.cancel();

    threw(waiting.await());

    // The clock has to remove the entry at once rather than hold it for a minute; if it does not,
    // ending the process still goes through, but the entry would lie there until its deadline.
    Time::later(30ms).await();
}

// Many waits with deadlines in no order, half of them cancelled on the way: the rest come due, each
// not before its time, and the cancelled ones never do.
TRIAL("Time: of many waits the cancelled ones never come due and the rest do on time") {
    std::vector<Promise<void>> among;
    std::vector<std::chrono::milliseconds> patience;

    for (int at = 0; at < 400; ++at) {
        patience.push_back(std::chrono::milliseconds{20 + (at * 37) % 120});
        among.push_back(Time::later(patience.back()));
    }

    Stopwatch held;

    for (Promise<void>& one : among) one.start();

    for (std::size_t at = 1; at < among.size(); at += 2) among[at].cancel();

    for (std::size_t at = 0; at < among.size(); at += 2) {
        among[at].await();

        ought(held.spent() + 5ms >= patience[at]);
    }

    for (std::size_t at = 1; at < among.size(); at += 2) threw(among[at].await());
}

TRIAL("Time: the calendar stamp moves forward") {
    const std::chrono::sys_seconds one = Time::stamp();

    ought(one.time_since_epoch().count() > 1700000000);
}
