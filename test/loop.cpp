#include "proving.hpp"

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

#include "talcite/loop.hpp"
#include "talcite/pool.hpp"
#include "talcite/promise.hpp"

using namespace talcite;

TRIAL("Loop: turn runs the posted work and returns how much") {
    Loop loop;

    int counted = 0;

    loop.post([&counted] { ++counted; });
    loop.post([&counted] { counted += 10; });

    same(loop.waiting(), std::size_t{2});
    same(loop.turn(), std::size_t{2});
    same(counted, 11);
    same(loop.turn(), std::size_t{0});
}

TRIAL("Loop: current sees only its own thread") {
    Loop loop;

    ought(Loop::current() == &loop);

    std::atomic<bool> seen{true};

    std::thread aside{[&seen] { seen.store(Loop::current() != nullptr); }};
    aside.join();

    ought(!seen.load());
}

TRIAL("Loop: the bell wakes the waiter") {
    Loop loop;

    ought(!loop.bell().wait(std::chrono::milliseconds{0}));

    loop.post([] {});

    ought(loop.bell().wait(std::chrono::milliseconds{0}));

    (void)loop.turn();

    ought(!loop.bell().wait(std::chrono::milliseconds{0}));
}

TRIAL("Promise: here moves the rest of the chain to the loop's thread") {
    Loop loop;

    const std::thread::id mine = std::this_thread::get_id();

    std::atomic<bool> off{false};
    std::atomic<bool> home{false};
    std::atomic<bool> done{false};

    Promise<int> chain =
        Promise<int>{[](auto resolve, auto) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            resolve(1);
        }}
            .then([&off, mine](const int& given) {
                off.store(std::this_thread::get_id() != mine);
                return given;
            })
            .here()
            .then([&home, &done, mine](const int& given) {
                home.store(std::this_thread::get_id() == mine);
                done.store(true);
                return given;
            });

    chain.start();

    for (int at = 0; at < 200 && !done.load(); ++at) {
        (void)loop.turn();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    ought(off.load());
    ought(home.load());
    ought(done.load());
}

// `on(loop)` names the loop directly, so a chain built on one thread can finish on another
// thread's loop, for example the network loop or the preview loop once there are more of them. The
// building thread has no loop at all here, and the continuation still lands where it was told to.
TRIAL("Promise: on moves the rest of the chain to the given loop") {
    std::atomic<Loop*> other{nullptr};
    std::atomic<bool> stop{false};
    std::thread::id theirs;

    std::thread aside{[&other, &stop, &theirs] {
        Loop loop;

        theirs = std::this_thread::get_id();
        other.store(&loop);

        while (!stop.load()) {
            (void)loop.turn();
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }

        other.store(nullptr);
    }};

    while (other.load() == nullptr) std::this_thread::sleep_for(std::chrono::milliseconds{1});

    ought(Loop::current() == nullptr);

    std::thread::id ran;

    Promise<int> chain = Promise<int>{[](auto resolve, auto) { resolve(3); }}
                             .on(*other.load())
                             .then([&ran](const int& given) {
                                 ran = std::this_thread::get_id();
                                 return given;
                             });

    same(chain.await(), 3);
    ought(ran == theirs);

    stop.store(true);
    aside.join();
}

// `here()` without a loop is the caller's error, reported at once, while the chain is built.
// Silently passing through would run the continuation on the thread that delivered the result,
// which is exactly the race on the node tree that `here()` is meant to prevent. Code that may run
// without a loop checks for one itself before building the chain (the progress bar does so).
TRIAL("Promise: without a loop here throws before anything starts") {
    std::atomic<bool> ran{false};

    Promise<int> source{[&ran](auto resolve, auto) {
        ran.store(true);
        resolve(2);
    }};

    threw((void)source.here());
    ought(!ran.load());
}

TRIAL("then: on a settled promise with here, waits for turn instead of running at once") {
    Loop loop;

    Promise<int> done{[](auto resolve, auto) { resolve(4); }};

    same(done.await(), 4);

    std::atomic<bool> ran{false};

    Promise<int> late = done.here().then([&ran](const int& given) {
        ran.store(true);
        return given;
    });

    late.start();

    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    ought(!ran.load());

    for (int at = 0; at < 100 && !ran.load(); ++at) {
        (void)loop.turn();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    ought(ran.load());
}

// Pool work that finishes after the loop is gone does not reach it. In the program the loop lives
// to the end, but in trials it dies with the trial, so the `here` link must not keep a bare pointer
// to it. This shows without a sanitizer: the new loop takes the same memory as the dead one, so a
// stale pointer would reach it and the continuation promised to the dead loop would run on another.
TRIAL("Loop: a continuation promised to a dead loop is dropped, not sent to the next one") {
    std::atomic<bool> go{false};
    std::atomic<bool> ran{false};

    std::optional<Loop> slot;
    std::optional<Promise<void>> held;

    slot.emplace();

    held.emplace(Promise<void>{[&go](auto resolve, auto) {
                     while (!go.load()) std::this_thread::sleep_for(std::chrono::milliseconds{1});

                     resolve();
                 }}
                     .here()
                     .then([&ran] { ran.store(true); }));

    held->start();

    slot.reset();
    slot.emplace();

    go.store(true);

    pooling().drain();

    (void)slot->turn();

    ought(!ran.load());
    same(slot->waiting(), std::size_t{0});
}
