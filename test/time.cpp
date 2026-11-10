#include "tal/testing.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
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

    OUGHT(held.spent() >= 50ms);
    OUGHT(held.spent() < 400ms);

    held.reset();

    OUGHT(held.spent() < 50ms);
}

TRIAL("Time: later is fulfilled after the time, not before") {
    Stopwatch held;

    later(120ms).get();

    OUGHT(held.spent() >= 100ms);
    OUGHT(held.spent() < 600ms);
}

TRIAL("Time: later counts from the call, not from the first wait") {
    Promise<void> waiting = later(40ms);

    std::this_thread::sleep_for(200ms);

    OUGHT(waiting.ready());

    Stopwatch held;

    std::move(waiting).get();

    OUGHT(held.spent() < 100ms);
}

TRIAL("Time: a stopwatch is not a clock") {
    static_assert(!std::is_default_constructible_v<Time>);
    static_assert(std::is_default_constructible_v<Stopwatch>);
}

TRIAL("timeout: work that made it in time gives its value") {
    Promise<int> quick = run(io(), [] {
        std::this_thread::sleep_for(20ms);

        return 7;
    });

    SAME(std::move(quick).timeout(800ms).get(), 7);
}

TRIAL("timeout: work that did not make it in time fails with an Overdue exception and is stopped") {
    auto [made, settle] = promised<int>();

    Stopwatch held;
    bool caught = false;

    try {
        (void)std::move(made).timeout(60ms).get();
    } catch (const Overdue&) {
        caught = true;
    }

    OUGHT(caught);
    OUGHT(held.spent() < 500ms);
    OUGHT(settle.stop().wait(5s));
}

TRIAL("timeout: the work's rejection passes through as it is") {
    Promise<int> broken = run(io(), []() -> int { throw Rejected{"zepsute"}; });

    std::string said;

    try {
        (void)std::move(broken).timeout(800ms).get();
    } catch (const Overdue&) {
        said = "overdue";
    } catch (const Rejected& why) {
        said = why.what();
    }

    SAME(said, std::string{"zepsute"});
}

TRIAL("timeout: a value that came in time stays, however long the result waits") {
    Promise<int> bounded = run(io(), [] { return 5; }).timeout(40ms);

    std::this_thread::sleep_for(120ms);

    SAME(std::move(bounded).get(), 5);
}

TRIAL("timeout: cancelling the result stops the work") {
    auto [made, settle] = promised<void>();

    Promise<void> bounded = std::move(made).timeout(60000ms);

    bounded.cancel();

    OUGHT(settle.stop().wait(5s));
}

TRIAL("timeout: works on a link of a chain and stops it all the way to the source") {
    auto [made, settle] = promised<void>();

    Promise<int> chained = std::move(made).then([] { return 1; });

    THREW(std::move(chained).timeout(40ms).get());

    OUGHT(settle.stop().wait(5s));
}

TRIAL("Time: a hundred waits do not cost a hundred threads") {
    std::vector<Promise<void>> among;
    among.reserve(100);

    Stopwatch held;

    for (int at = 0; at < 100; ++at) among.push_back(later(60ms));

    all(std::move(among)).get();

    // If every wait slept in its own task, a hundred of them on two cores would drag on much longer
    // than one.
    OUGHT(held.spent() < 900ms);
}

TRIAL("Time: a cancelled wait never comes due") {
    auto fired = std::make_shared<std::atomic<bool>>(false);

    Promise<void> waiting = later(80ms).then([fired] { fired->store(true); });

    std::this_thread::sleep_for(20ms);

    waiting.cancel();

    later(200ms).get();

    OUGHT(!fired->load());
}

// Many waits with deadlines in no order, half of them cancelled on the way: the rest come due, each
// not before its time, and the cancelled ones never do. A wait can be cancelled only before it is
// due, so only those whose cancellation was over before their deadline are held to it; on a busy
// machine the loop that cancels may come to a short one too late.
TRIAL("Time: of many waits the cancelled ones never come due and the rest do on time") {
    constexpr std::size_t many = 400;

    auto fired = std::make_shared<std::vector<std::atomic<bool>>>(many);

    std::vector<Promise<void>> among;
    std::vector<std::chrono::milliseconds> patience;

    Stopwatch held;

    for (std::size_t at = 0; at < many; ++at) {
        patience.push_back(std::chrono::milliseconds{20 + (at * 37) % 120});
        among.push_back(later(patience.back()).then([fired, at] { (*fired)[at].store(true); }));
    }

    std::vector<bool> in_time(many, false);

    for (std::size_t at = 1; at < among.size(); at += 2) {
        among[at].cancel();

        in_time[at] = held.spent() < patience[at];
    }

    for (std::size_t at = 0; at < among.size(); at += 2) {
        std::move(among[at]).get();

        OUGHT(held.spent() + 5ms >= patience[at]);
    }

    later(200ms).get();

    std::size_t held_to = 0;

    for (std::size_t at = 1; at < among.size(); at += 2)
        if (in_time[at]) {
            ++held_to;

            OUGHT(!(*fired)[at].load());
        }

    OUGHT(held_to > 0);
}

TRIAL("Time: the calendar stamp moves forward") {
    const std::chrono::sys_seconds one = Time::stamp();

    OUGHT(one.time_since_epoch().count() > 1700000000);
}
