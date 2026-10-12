#include "proving.hpp"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <deque>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "talcite/bell.hpp"
#include "talcite/loop.hpp"
#include "talcite/pool.hpp"
#include "talcite/promise.hpp"

#ifndef _WIN32
#include <fcntl.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

using namespace talcite;

namespace {

// A promise that resolves with the given value.
template <typename T>
Promise<T> gave(T given) {
    return Promise<T>{[given](auto resolve, auto) { resolve(given); }};
}

// A promise that sleeps `wait` milliseconds and then resolves with `given`.
Promise<int> slowly(int given, int wait) {
    return Promise<int>{[given, wait](auto resolve, auto) {
        std::this_thread::sleep_for(std::chrono::milliseconds{wait});
        resolve(given);
    }};
}

}

TRIAL("Promise: does not start until someone says so") {
    std::atomic<bool> ran{false};

    Promise<int> one{[&ran](auto resolve, auto) {
        ran.store(true);
        resolve(7);
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    ought(!ran.load());

    same(one.await(), 7);
    ought(ran.load());
}

TRIAL("Promise: await gives the result") {
    Promise<std::string> one = gave<std::string>("gotowe");

    same(one.await(), std::string{"gotowe"});
}

TRIAL("Promise: the result stays, the work happens once") {
    std::atomic<int> counted{0};

    Promise<int> one{[&counted](auto resolve, auto) {
        resolve(++counted);
    }};

    same(one.await(), 1);
    same(one.await(), 1);
    same(counted.load(), 1);
}

TRIAL("Promise: a rejection comes back from await as an exception") {
    Promise<int> one{[](auto, auto reject) { reject("No such file."); }};

    bool caught = false;

    try {
        (void)one.await();
    } catch (const std::exception& why) {
        caught = std::string{why.what()} == "No such file.";
    }

    ought(caught);
}

TRIAL("Promise: an exception thrown in the body becomes a rejection") {
    Promise<int> one{[](auto, auto) -> void { throw std::runtime_error{"fell over"}; }};

    threw((void)one.await());
}

TRIAL("Promise: then passes the result on") {
    Promise<int> chain = gave(20).then([](const int& given) { return given + 2; });

    same(chain.await(), 22);
}

TRIAL("Promise: then changes the type") {
    Promise<std::string> chain =
        gave(7).then([](const int& given) { return std::to_string(given * 6); });

    same(chain.await(), std::string{"42"});
}

TRIAL("Promise: the whole chain starts from the last link") {
    std::atomic<bool> ran{false};

    Promise<int> root{[&ran](auto resolve, auto) {
        ran.store(true);
        resolve(1);
    }};

    Promise<int> chain = root.then([](const int& given) { return given + 1; })
                             .then([](const int& given) { return given + 1; });

    ought(!ran.load());

    same(chain.await(), 3);
}

TRIAL("Promise: a rejection skips then and reaches fail") {
    std::atomic<int> ran{0};
    std::string said;

    Promise<int> chain = Promise<int>{[](auto, auto reject) { reject("zepsute"); }}
                             .then([&ran](const int& given) {
                                 ++ran;
                                 return given;
                             })
                             .fail([&said](const std::exception& why) { said = why.what(); });

    threw((void)chain.await());

    same(ran.load(), 0);
    same(said, std::string{"zepsute"});
}

// A promise returned from `then` is unwrapped in the chain instead of giving
// Promise<Promise<int>>. Without flattening a chain of asynchronous steps does not compose at all:
// every step adds a layer of wrapping, and the caller has to take it off, that is wait, that is
// block a thread.
TRIAL("Promise: a promise returned from then unwraps in the chain") {
    Promise<int> chain = gave(5).then([](const int& given) { return slowly(given * 2, 20); });

    same(chain.await(), 10);
}

TRIAL("Promise: unwrapping works for a void promise too") {
    std::atomic<int> ran{0};

    Promise<void> chain = gave(1).then([&ran](const int&) {
        return Promise<void>{[&ran](auto resolve, auto) {
            ran.fetch_add(1);
            resolve();
        }};
    });

    chain.await();

    same(ran.load(), 1);
}

TRIAL("Promise: a rejection in the middle of the chain comes to the surface") {
    std::string said;

    Promise<int> chain =
        gave(2)
            .then([](const int&) {
                return Promise<int>{[](auto, auto reject) { reject("zepsute w środku"); }};
            })
            .fail([&said](const std::exception& why) { said = why.what(); });

    threw((void)chain.await());

    same(said, std::string{"zepsute w środku"});
}

TRIAL("Promise: two asynchronous steps in a row") {
    Promise<int> chain = gave(3)
                             .then([](const int& given) { return slowly(given + 1, 10); })
                             .then([](const int& given) { return slowly(given * 10, 10); });

    same(chain.await(), 40);
}

TRIAL("Promise: an inner promise starts by itself, without await") {
    std::atomic<int> ran{0};

    Promise<int> chain = gave(1).then([&ran](const int&) {
        return Promise<int>{[&ran](auto resolve, auto) {
            ran.fetch_add(1);
            resolve(7);
        }};
    });

    same(chain.await(), 7);
    same(ran.load(), 1);
}

TRIAL("Promise: void settles without a payload") {
    std::atomic<bool> ran{false};

    Promise<void> one{[&ran](auto resolve, auto) {
        ran.store(true);
        resolve();
    }};

    one.await();

    ought(ran.load());
}

TRIAL("Promise: then after void calls without a parameter") {
    Promise<int> chain = Promise<void>{[](auto resolve, auto) { resolve(); }}.then([] { return 9; });

    same(chain.await(), 9);
}

TRIAL("Promise: registering on a settled one is not lost") {
    Promise<int> one = gave(3);

    same(one.await(), 3);

    Promise<int> late = one.then([](const int& given) { return given * 3; });

    same(late.await(), 9);
}

TRIAL("Promise: a copy of the handle shares the result") {
    Promise<int> one = slowly(4, 20);
    Promise<int> two = one;

    same(one.await(), 4);
    same(two.await(), 4);
}

TRIAL("Promise: a dead branch stays silent") {
    std::atomic<int> ran{0};

    Promise<int> root = slowly(1, 60);

    {
        Promise<int> doomed = root.then([&ran](const int& given) {
            ++ran;
            return given;
        });
        (void)doomed;
    }

    Promise<int> kept = root.then([](const int& given) { return given + 1; });

    same(kept.await(), 2);

    pooling().drain();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    same(ran.load(), 0);
}

TRIAL("Pool: an own pool takes work and can be drained") {
    Pool mine;

    std::atomic<int> counted{0};

    for (int at = 0; at < 24; ++at) mine.post([&counted] { ++counted; });

    mine.drain();

    same(counted.load(), 24);
    ought(mine.staff() >= 1);
}

TRIAL("Pool: workers come back instead of multiplying without end") {
    Pool mine;

    for (int at = 0; at < 50; ++at) {
        mine.post([] {});
        mine.drain();
    }

    ought(mine.staff() <= 2);
}

TRIAL("Promise: cancel before the start does not run the work") {
    std::atomic<bool> ran{false};

    Promise<int> one{[&ran](auto resolve, auto) {
        ran.store(true);
        resolve(1);
    }};

    one.cancel();

    bool caught = false;

    try {
        (void)one.await();
    } catch (const Cancelled&) {
        caught = true;
    }

    ought(caught);

    pooling().drain();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    ought(!ran.load());
}

TRIAL("Promise: cancel midway, the late result is ignored") {
    Promise<int> one = slowly(5, 80);

    one.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

    one.cancel();

    threw((void)one.await());

    pooling().drain();

    bool caught = false;

    try {
        (void)one.await();
    } catch (const Cancelled&) {
        caught = true;
    }

    ought(caught);
}

TRIAL("Promise: cancel skips then and reaches fail") {
    std::atomic<int> ran{0};
    std::atomic<bool> knew{false};

    Promise<int> root = slowly(1, 60);

    Promise<int> chain = root.then([&ran](const int& given) {
                                 ++ran;
                                 return given;
                             })
                             .fail([&knew](const std::exception& why) {
                                 knew.store(dynamic_cast<const Cancelled*>(&why) != nullptr);
                             });

    root.cancel();

    threw((void)chain.await());

    same(ran.load(), 0);
    ought(knew.load());
}

// Something thrown that is not a `std::exception` reaches `fail` too, as a fixed `Rejected` with an
// English sentence, because `fail` takes `const std::exception&` and has nothing else to give.
TRIAL("Promise: fail also gets a rejection that is not an exception") {
    std::string said;

    Promise<int> chain = Promise<int>{[](auto, auto) { throw 7; }}.fail(
        [&said](const std::exception& why) { said = why.what(); });

    // The reason itself goes on untouched: the tail throws whatever the body threw.
    bool rethrown = false;

    try {
        (void)chain.await();
    } catch (int) {
        rethrown = true;
    }

    ought(rethrown);
    same(said, std::string{"Unknown reason for rejection."});
}

TRIAL("Promise: finally runs after success and does not touch the result") {
    std::atomic<int> ran{0};

    Promise<int> chain = gave(11).finally([&ran] { ++ran; });

    same(chain.await(), 11);
    same(ran.load(), 1);
}

TRIAL("Promise: finally runs after a rejection, and the reason goes on") {
    std::atomic<int> ran{0};
    std::string said;

    Promise<int> chain = Promise<int>{[](auto, auto reject) { reject("zepsute"); }}
                             .finally([&ran] { ++ran; })
                             .fail([&said](const std::exception& why) { said = why.what(); });

    threw((void)chain.await());

    same(ran.load(), 1);
    same(said, std::string{"zepsute"});
}

TRIAL("Promise: finally runs after cancelling too") {
    std::atomic<int> ran{0};

    Promise<int> root = slowly(2, 60);
    Promise<int> chain = root.finally([&ran] { ++ran; });

    root.cancel();

    threw((void)chain.await());

    same(ran.load(), 1);
}

// A callback that throws rejects the promise it made, with what it threw, rather than leaving it
// waiting forever: `fail` instead of the reason it got, `finally` instead of the value. `finally`
// still runs after a `fail` that threw, and the first exception wins.
TRIAL("Promise: a fail or finally that throws rejects its promise with what it threw") {
    std::string said;

    Promise<int> failing = Promise<int>{[](auto, auto reject) { reject("zepsute"); }}
                               .fail([](const std::exception&) { throw std::runtime_error{"z fail"}; })
                               .fail([&said](const std::exception& why) { said = why.what(); });

    threw((void)failing.timeout(std::chrono::seconds{5}).await());
    same(said, std::string{"z fail"});

    std::atomic<int> ran{0};
    std::string last;

    Promise<int> after = gave(3)
                             .finally([] { throw std::runtime_error{"z finally"}; })
                             .finally([&ran] { ++ran; })
                             .fail([&last](const std::exception& why) { last = why.what(); });

    threw((void)after.timeout(std::chrono::seconds{5}).await());
    same(ran.load(), 1);
    same(last, std::string{"z finally"});

    // A finally after a fail that threw still runs, and what it throws in turn goes on.
    std::string final;

    Promise<void> twice = Promise<void>{[](auto, auto reject) { reject("zepsute"); }}
                              .fail([](const std::exception&) { throw std::runtime_error{"pierwszy"}; })
                              .finally([] { throw std::runtime_error{"drugi"}; })
                              .fail([&final](const std::exception& why) { final = why.what(); });

    threw(twice.timeout(std::chrono::seconds{5}).await());
    same(final, std::string{"drugi"});
}

// The continuations of one promise are woken together; one that throws does not keep the others
// from settling.
TRIAL("Promise: a throwing continuation does not stop its siblings") {
    Promise<int> root = slowly(4, 30);

    Promise<int> broken = root.finally([] { throw std::runtime_error{"z finally"}; });
    Promise<int> sibling = root.then([](int given) { return given * 2; });

    broken.start();
    sibling.start();

    same(sibling.timeout(std::chrono::seconds{5}).await(), 8);
    threw((void)broken.timeout(std::chrono::seconds{5}).await());
}

// --- Cancelling goes to the source -----------------------------------------------------------

// Cancelling stops the work, not a view of it. The caller usually holds the tail (the last link
// from `then`, `fail` and `on`), and that is what it wants to stop, so cancelling the tail reaches
// the source; the caller does not need to hold the head and the tail separately.
TRIAL("cancel: the tail cancels the source before it starts") {
    std::atomic<bool> ran{false};

    Promise<int> root{[&ran](auto resolve, auto) {
        ran.store(true);
        resolve(1);
    }};

    Promise<int> tail = root.then([](const int& given) { return given + 1; })
                            .fail([](const std::exception&) {})
                            .finally([] {});

    tail.cancel();

    threw((void)tail.await());
    threw((void)root.await());

    pooling().drain();

    ought(!ran.load());
}

TRIAL("cancel: the tail stops a source that is already running") {
    Promise<int> root = slowly(1, 80);
    Promise<int> tail = root.then([](const int& given) { return given; });

    tail.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

    tail.cancel();

    bool caught = false;

    try {
        (void)root.await();
    } catch (const Cancelled&) {
        caught = true;
    }

    ought(caught);
}

// A pool task that waits for other work hears its own cancellation in the wait: `await` stops,
// throws `Cancelled`, and cancels the work it waited for, which nobody wants any more. Without that
// the cancelled task would wait to the end and hold its worker.
// A bell with an ear lent to it wakes that condition variable when it rings, from another thread,
// and says it rang until cleared; `heed` gives back the ear heard before, and without an ear a ring
// touches no condition variable.
TRIAL("Bell: an ear lent to it wakes a condition variable, and rang lasts until clear") {
    Bell bell;

    std::mutex guard;
    std::condition_variable stirred;

    ought(!bell.rang());

    const Bell::Ear before = bell.heed(Bell::Ear{&guard, &stirred});

    ought(before.guard == nullptr && before.stirred == nullptr);

    std::thread ringer{[&bell] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        bell.ring();
    }};

    {
        std::unique_lock<std::mutex> held{guard};

        ought(stirred.wait_for(held, std::chrono::seconds{10}, [&bell] { return bell.rang(); }));
    }

    ringer.join();

    const Bell::Ear lent = bell.heed(Bell::Ear{});

    ought(lent.guard == &guard && lent.stirred == &stirred);
    ought(bell.rang());
    ought(bell.wait(std::chrono::milliseconds{0}));

    bell.clear();

    ought(!bell.rang());
    ought(!bell.wait(std::chrono::milliseconds{0}));

    bell.ring();

    ought(bell.rang());
}

TRIAL("await: a cancelled pool task stops waiting and cancels what it waited for") {
    const auto began = std::make_shared<std::atomic<bool>>(false);
    const auto heard = std::make_shared<std::atomic<bool>>(false);
    const auto dropped = std::make_shared<std::atomic<bool>>(false);

    Promise<void> never{[](auto, auto) {}};
    Promise<void> watched = never.fail([dropped](const std::exception& why) {
        dropped->store(dynamic_cast<const Cancelled*>(&why) != nullptr);
    });

    Promise<void> task{[never, began, heard](auto resolve, auto) mutable {
        began->store(true);

        try {
            never.await();
        } catch (const Cancelled&) {
            heard->store(true);
        }

        resolve();
    }};

    task.start();

    for (int tries = 0; tries < 2000 && !began->load(); ++tries) std::this_thread::sleep_for(std::chrono::milliseconds{1});

    ought(began->load());

    task.cancel();

    for (int tries = 0; tries < 2000 && !heard->load(); ++tries) std::this_thread::sleep_for(std::chrono::milliseconds{1});

    ought(heard->load());
    ought(dropped->load());
}

// A promise returned from `then` is work just like the source, often the only one still running,
// because the source settled long ago. Cancelling the tail must reach it too.
TRIAL("cancel: reaches a promise returned from then") {
    Promise<int> inner = slowly(2, 80);

    Promise<int> tail = gave(1).then([inner](const int&) { return inner; });

    tail.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});

    tail.cancel();

    bool caught = false;

    try {
        (void)inner.await();
    } catch (const Cancelled&) {
        caught = true;
    }

    ought(caught);
}

// The tail is cancelled before `then` has returned its promise: the returned one no longer starts.
// Without that, cancelling would fall into the gap between the end of the `then` body and the start
// of what it returned.
TRIAL("cancel: a promise returned from then after cancelling no longer starts") {
    std::atomic<bool> release{false};
    std::atomic<bool> ran{false};

    Promise<int> tail = gave(1).then([&release, &ran](const int&) {
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds{2});

        return Promise<int>{[&ran](auto resolve, auto) {
            ran.store(true);
            resolve(2);
        }};
    });

    tail.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});

    tail.cancel();
    release.store(true);

    threw((void)tail.await());

    pooling().drain();
    std::this_thread::sleep_for(std::chrono::milliseconds{20});

    ought(!ran.load());
}

TRIAL("cancel: a join cancels all its members") {
    Promise<int> one = slowly(1, 80);
    Promise<int> two = slowly(2, 80);

    Promise<void> both = Promise<int>::all(one, two);

    both.start();
    std::this_thread::sleep_for(std::chrono::milliseconds{10});

    both.cancel();

    threw((void)one.await());
    threw((void)two.await());

    std::vector<Promise<int>> many{slowly(3, 80), slowly(4, 80)};

    Promise<void> all = Promise<int>::race(many);

    all.start();
    all.cancel();

    threw((void)many[0].await());
    threw((void)many[1].await());
}

// A settled promise has nothing to stop: the result stays, and the source gets no cancel, even
// though the tail still points to it.
TRIAL("cancel: on a settled tail it changes nothing") {
    Promise<int> root = gave(5);
    Promise<int> tail = root.then([](const int& given) { return given * 2; });

    same(tail.await(), 10);

    tail.cancel();

    same(tail.await(), 10);
    same(root.await(), 5);

    // A join too: a race settled by its first member does not stop the second when it is cancelled
    // later; what is settled no longer says anything down or up.
    Promise<int> fast = gave(1);
    Promise<int> slow = slowly(2, 60);
    Promise<void> first = Promise<int>::race(fast, slow);

    first.await();
    first.cancel();

    same(slow.await(), 2);
}

// A shared source shares its fate. Two branches from one promise point to the same work, so
// cancelling one stops it for the other as well. That is deliberate: counting interested branches
// would need state and locks in every link for a case nobody has.
TRIAL("cancel: branches from a shared source share the cancel") {
    Promise<int> root = slowly(1, 80);
    Promise<int> left = root.then([](const int& given) { return given; });
    Promise<int> right = root.then([](const int& given) { return given; });

    left.start();
    left.cancel();

    threw((void)right.await());
}

TRIAL("all: waits for all, and the results stay in the promises") {
    Promise<int> one = slowly(1, 60);
    Promise<int> two = slowly(2, 10);
    Promise<int> three = slowly(3, 30);

    Promise<int>::all(one, two, three).await();

    same(one.await(), 1);
    same(two.await(), 2);
    same(three.await(), 3);
}

TRIAL("all: then after it gets nothing") {
    std::atomic<bool> ran{false};

    Promise<int> one = gave(1);
    Promise<int> two = gave(2);

    Promise<void> joined = Promise<int>::all(one, two).then([&ran] { ran.store(true); });

    joined.await();

    ought(ran.load());
}

TRIAL("all: nothing starts until someone says so") {
    std::atomic<int> ran{0};

    std::vector<Promise<int>> among;
    for (int at = 0; at < 3; ++at)
        among.push_back(Promise<int>{[&ran](auto resolve, auto) {
            ++ran;
            resolve(1);
        }});

    Promise<void> joined = Promise<int>::all(among);

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    same(ran.load(), 0);

    joined.await();

    same(ran.load(), 3);
}

TRIAL("all: they run side by side, not one after another") {
    const auto began = std::chrono::steady_clock::now();

    std::vector<Promise<int>> among;
    for (int at = 0; at < 4; ++at) among.push_back(slowly(at, 80));

    Promise<int>::all(among).await();

    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - began)
                           .count();

    ought(spent < 240);
}

TRIAL("all: an empty set settles at once") {
    Promise<int>::all(std::vector<Promise<int>>{}).await();
    Promise<int>::all().await();
}

TRIAL("all: one rejection brings the whole down") {
    std::vector<Promise<int>> among;
    among.push_back(gave(1));
    among.push_back(Promise<int>{[](auto, auto reject) { reject("zepsute"); }});
    among.push_back(gave(3));

    std::string said;

    Promise<void> joined =
        Promise<int>::all(among).fail([&said](const std::exception& why) { said = why.what(); });

    threw(joined.await());

    same(said, std::string{"zepsute"});
}

TRIAL("all: joins void promises too") {
    std::atomic<int> ran{0};

    std::vector<Promise<void>> among;
    for (int at = 0; at < 5; ++at)
        among.push_back(Promise<void>{[&ran](auto resolve, auto) {
            ++ran;
            resolve();
        }});

    Promise<void>::all(among).await();

    same(ran.load(), 5);
}

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

}

TRIAL("Pool: computing work does not exceed the ceiling") {
    Pool mine{3};

    Highest counted;

    for (int at = 0; at < 24; ++at)
        mine.post([&counted] {
            counted.entered();
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            counted.left();
        }, Lane::cpu);

    mine.drain();

    same(counted.most.load() <= 3, true);
    ought(counted.most.load() >= 2);
}

TRIAL("Pool: ordinary work knows no ceiling") {
    Pool mine{2};

    Highest counted;

    for (int at = 0; at < 12; ++at)
        mine.post([&counted] {
            counted.entered();
            std::this_thread::sleep_for(std::chrono::milliseconds{40});
            counted.left();
        });

    mine.drain();

    ought(counted.most.load() > 2);
}

TRIAL("Pool: computing work is not starved by a busy team") {
    Pool mine{2};

    std::atomic<bool> done{false};

    for (int at = 0; at < 4; ++at)
        mine.post([] { std::this_thread::sleep_for(std::chrono::milliseconds{60}); });

    mine.post([&done] { done.store(true); }, Lane::cpu);

    mine.drain();

    ought(done.load());
}

namespace {

// Waits until the flag is up, at most five seconds; returns whether it came up.
bool risen(const std::atomic<bool>& flag) {
    for (int again = 0; again < 500 && !flag.load(); ++again)
        std::this_thread::sleep_for(std::chrono::milliseconds{10});

    return flag.load();
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

// A burst of blocking work leaves many workers behind. Those above the ceiling leave after their
// rest, each with its thread and its bell, and the pool still takes work afterwards.
TRIAL("Pool: idle workers above the ceiling leave after their rest") {
    Pool mine{1, std::chrono::milliseconds{30}};

    for (int at = 0; at < 6; ++at)
        (void)mine.post([] { std::this_thread::sleep_for(std::chrono::milliseconds{40}); });

    mine.drain();

    ought(mine.staff() >= 2);

    for (int again = 0; again < 300 && mine.staff() > 1; ++again)
        std::this_thread::sleep_for(std::chrono::milliseconds{10});

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

// A computing task that waits for other computing work gives its slot back for the wait. Twice as
// many such tasks as slots would otherwise fill every slot with waiting, and the work they wait
// for would never get one. The timeout only keeps a broken pool from hanging the trials.
TRIAL("Pool: a computing task waiting for computing work does not hold its slot") {
    const int many = static_cast<int>(pooling().most()) * 2;

    std::vector<Promise<int>> among;

    for (int at = 0; at < many; ++at)
        among.push_back(Promise<int>{[at](auto resolve, auto) {
                                         Promise<int> inner{[at](auto give, auto) { give(at); }, Lane::cpu};

                                         resolve(inner.await() + 1);
                                     },
                                     Lane::cpu});

    Promise<void>::all(among).timeout(std::chrono::seconds{5}).await();

    for (int at = 0; at < many; ++at) same(among[static_cast<std::size_t>(at)].await(), at + 1);
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

    mine.drop(doomed);
    mine.drop(jobs[1]);
    mine.drop(jobs[4]);

    blocking.released.store(true);

    mine.drain();

    same(order, (std::vector<int>{0, 2, 3, 5}));
    same(counted.load(), 100);
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
// free; neither ends the program from a worker thread.
TRIAL("Pool: without descriptors work is refused or waits, and the program goes on") {
    Pool fresh;
    Pool working{1};

    Holding blocking;

    (void)working.post([&blocking] { blocking(); });

    ought(risen(blocking.held));

    std::atomic<int> counted{0};

    {
        const Starving starving;

        ought(starving.held());

        threw((void)fresh.post([&counted] { ++counted; }));

        (void)working.post([&counted] { counted += 10; });
    }

    blocking.released.store(true);

    working.drain();

    same(counted.load(), 10);

    (void)fresh.post([&counted] { ++counted; });

    fresh.drain();

    same(counted.load(), 11);
}

#endif

// A shielded promise is a view of another: cancelling the view, or a timeout over it, leaves the
// promise it looks at running for everyone else.
TRIAL("Promise: a shielded view cancels only itself") {
    Promise<int> shared = slowly(5, 120);

    shared.start();

    threw((void)shared.shielded().timeout(std::chrono::milliseconds{10}).await());

    same(shared.shielded().await(), 5);
    same(shared.await(), 5);

    Promise<int> fresh = slowly(7, 80);
    Promise<int> look = fresh.shielded();

    look.start();
    look.cancel();

    threw((void)look.await());

    same(fresh.await(), 7);
}

// --- Lanes -----------------------------------------------------------------------------------

// The lane says where the source's body runs. It is given in the constructor and applies exactly
// to the body.
TRIAL("Lane: a body on the cpu lane does not exceed the ceiling, in a join too") {
    Highest counted;

    std::vector<Promise<int>> among;

    for (int at = 0; at < 16; ++at)
        among.push_back(Promise<int>{[&counted](auto resolve, auto) {
                                         counted.entered();
                                         std::this_thread::sleep_for(std::chrono::milliseconds{20});
                                         counted.left();
                                         resolve(1);
                                     },
                                     Lane::cpu});

    Promise<int>::all(among).await();

    ought(counted.most.load() <= static_cast<int>(pooling().most()));
}

TRIAL("Lane: the lane does not hasten the start, the promise is still lazy") {
    std::atomic<bool> ran{false};

    Promise<int> one{[&ran](auto resolve, auto) {
                         ran.store(true);
                         resolve(1);
                     },
                     Lane::cpu};

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    ought(!ran.load());

    same(one.then([](const int& given) { return given * 2; }).await(), 2);
    ought(ran.load());
}

// A continuation runs on the thread of whoever settled the promise, so computing in `then` after a
// read would run on the lane without a ceiling. `on(Lane::cpu)` moves the rest of the chain to the
// computing lane, just as `on(loop)` moves it to a loop.
TRIAL("Lane: on(cpu) moves computing after an ordinary source under the ceiling") {
    Highest counted;

    const int many = static_cast<int>(pooling().most()) * 4;

    std::vector<Promise<void>> among;

    for (int at = 0; at < many; ++at)
        among.push_back(Promise<void>{[](auto resolve, auto) { resolve(); }}
                            .on(Lane::cpu)
                            .then([&counted] {
                                counted.entered();
                                std::this_thread::sleep_for(std::chrono::milliseconds{20});
                                counted.left();
                            }));

    Promise<void>::all(among).await();

    ought(counted.most.load() <= static_cast<int>(pooling().most()));
}

// The other way round: waiting after a computing source would take a place under the ceiling, that
// is a core that should be computing. `on(Lane::io)` frees it; the waiters run side by side, however
// many there are.
TRIAL("Lane: on(io) takes waiting out from under the ceiling") {
    Highest counted;

    const int many = static_cast<int>(pooling().most()) * 4;

    std::vector<Promise<void>> among;

    for (int at = 0; at < many; ++at)
        among.push_back(Promise<void>{[](auto resolve, auto) { resolve(); }, Lane::cpu}
                            .on(Lane::io)
                            .then([&counted] {
                                counted.entered();
                                std::this_thread::sleep_for(std::chrono::milliseconds{40});
                                counted.left();
                            }));

    Promise<void>::all(among).await();

    ought(counted.most.load() > static_cast<int>(pooling().most()));
}

TRIAL("all_settled: waits for all and does not fall over on a rejection") {
    Promise<int> good = slowly(7, 20);
    Promise<int> bad{[](auto, auto reject) { reject("zepsute"); }};
    Promise<int> other = slowly(9, 40);

    Promise<int>::all_settled(good, bad, other).await();

    same(good.await(), 7);
    same(other.await(), 9);
    threw((void)bad.await());
}

TRIAL("all_settled: the reasons stay in the promises, not in the join") {
    std::atomic<bool> ran{false};

    Promise<int> bad{[](auto, auto reject) { reject("padło"); }};

    Promise<void> joined = Promise<int>::all_settled(bad).then([&ran] { ran.store(true); });

    joined.await();

    ought(ran.load());

    std::string said;

    try {
        (void)bad.await();
    } catch (const std::exception& why) {
        said = why.what();
    }

    same(said, std::string{"padło"});
}

TRIAL("all: a list of arguments instead of a vector") {
    Promise<int> one = slowly(1, 30);
    Promise<int> two = slowly(2, 10);

    Promise<int>::all(one, two).await();

    same(one.await(), 1);
    same(two.await(), 2);
}

TRIAL("all: a list of arguments joins promises of different types") {
    Promise<int> counted = slowly(5, 20);
    Promise<std::string> said{[](auto resolve, auto) { resolve(std::string{"gotowe"}); }};
    Promise<void> done{[](auto resolve, auto) { resolve(); }};

    Promise<void>::all(counted, said, done).await();

    same(counted.await(), 5);
    same(said.await(), std::string{"gotowe"});
}

TRIAL("all: a list of arguments falls over on a rejection") {
    Promise<int> good = gave(1);
    Promise<std::string> bad{[](auto, auto reject) { reject("zepsute"); }};

    std::string said;

    Promise<void> joined = Promise<void>::all(good, bad).fail(
        [&said](const std::exception& why) { said = why.what(); });

    threw(joined.await());

    same(said, std::string{"zepsute"});
}

TRIAL("all_settled: a list of different types does not fall over") {
    Promise<int> good = slowly(3, 20);
    Promise<std::string> bad{[](auto, auto reject) { reject("padło"); }};

    Promise<void>::all_settled(good, bad).await();

    same(good.await(), 3);
    threw((void)bad.await());
}

// A dropped join does not hold its inputs. The join keeps the links that wait on it, and those
// links hold the join, a cycle that only the settling of the last input would break. An input that
// never settles (in twa: a continuation on a loop that died earlier) would keep the whole in memory
// together with the results of the inputs that did settle. Checked without a clock: the result of a
// settled input must disappear together with the join.
TRIAL("all and all_settled: a dropped join does not hold inputs that will never come") {
    const auto dropped = [](auto joining) {
        std::weak_ptr<int> seen;

        {
            auto kept = std::make_shared<int>(7);

            seen = kept;

            Promise<std::shared_ptr<int>> came{[kept](auto resolve, auto) { resolve(kept); }};
            Promise<std::shared_ptr<int>> never{[](auto, auto) {}};

            kept.reset();

            Promise<void> joined = joining(came, never);

            joined.start();

            pooling().drain();

            ought(!seen.expired());
        }

        pooling().drain();

        ought(seen.expired());
    };

    dropped([](Promise<std::shared_ptr<int>>& came, Promise<std::shared_ptr<int>>& never) {
        return Promise<void>::all_settled(came, never);
    });

    dropped([](Promise<std::shared_ptr<int>>& came, Promise<std::shared_ptr<int>>& never) {
        return Promise<void>::all(std::vector<Promise<std::shared_ptr<int>>>{came, never});
    });
}

TRIAL("race: settles with the first, does not wait for the rest") {
    const auto began = std::chrono::steady_clock::now();

    Promise<int> quick = slowly(1, 20);
    Promise<int> slow = slowly(2, 400);

    Promise<void>::race(quick, slow).await();

    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - began)
                           .count();

    ought(spent < 200);

    same(quick.await(), 1);
}

TRIAL("race: the first rejection settles it too") {
    Promise<int> slow = slowly(1, 400);
    Promise<std::string> bad{[](auto, auto reject) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        reject("zepsute");
    }};

    std::string said;

    Promise<void> joined = Promise<void>::race(slow, bad).fail(
        [&said](const std::exception& why) { said = why.what(); });

    threw(joined.await());

    same(said, std::string{"zepsute"});
}

TRIAL("race: an already settled promise wins at once") {
    Promise<int> done = gave(5);
    (void)done.await();

    Promise<int> slow = slowly(2, 400);

    const auto began = std::chrono::steady_clock::now();

    Promise<int>::race(done, slow).await();

    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - began)
                           .count();

    ought(spent < 200);
}

TRIAL("race: also takes a set computed at run time") {
    std::vector<Promise<int>> among;
    among.push_back(slowly(1, 300));
    among.push_back(slowly(2, 20));
    among.push_back(slowly(3, 300));

    const auto began = std::chrono::steady_clock::now();

    Promise<int>::race(among).await();

    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - began)
                           .count();

    ought(spent < 200);
    same(among[1].await(), 2);
}

TRIAL("all: takes any range, not only a vector") {
    std::deque<Promise<int>> among;
    among.push_back(gave(1));
    among.push_back(gave(2));

    Promise<int>::all(among).await();

    same(among[0].await(), 1);
    same(among[1].await(), 2);
}

TRIAL("all: falls over at once, does not wait for the earlier ones") {
    const auto began = std::chrono::steady_clock::now();

    Promise<int> slow = slowly(1, 400);
    Promise<int> bad{[](auto, auto reject) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        reject("zepsute");
    }};

    std::string said;

    Promise<void> joined =
        Promise<int>::all(slow, bad).fail([&said](const std::exception& why) { said = why.what(); });

    threw(joined.await());

    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - began)
                           .count();

    ought(spent < 200);
    same(said, std::string{"zepsute"});
}

TRIAL("all: does not occupy a pool thread while waiting") {
    Pool mine{2};

    std::atomic<int> ran{0};

    // Four long jobs in a pool that grows anyway; the point is that the waiting for all itself is
    // not one of them.
    std::vector<Promise<int>> among;
    for (int at = 0; at < 4; ++at) among.push_back(slowly(at, 60));

    Promise<void> joined = Promise<int>::all(among).then([&ran] { ++ran; });

    joined.start();

    pooling().drain();

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    same(ran.load(), 1);
    (void)mine.staff();
}

TRIAL("any: the first success wins, even if another failed earlier") {
    Promise<int> bad{[](auto, auto reject) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        reject("zepsute");
    }};
    Promise<int> good = slowly(8, 40);

    Promise<int>::any(bad, good).await();

    same(good.await(), 8);
}

TRIAL("any: falls over only when all have failed") {
    Promise<int> one{[](auto, auto reject) { reject("pierwsze"); }};
    Promise<int> two{[](auto, auto reject) {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        reject("drugie");
    }};

    std::string said;

    Promise<void> joined = Promise<int>::any(one, two).fail(
        [&said](const std::exception& why) { said = why.what(); });

    threw(joined.await());

    ought(!said.empty());
}

TRIAL("any: takes a range too") {
    std::vector<Promise<int>> among;
    among.push_back(Promise<int>{[](auto, auto reject) { reject("zepsute"); }});
    among.push_back(slowly(4, 30));

    Promise<int>::any(among).await();

    same(among[1].await(), 4);
}

TRIAL("then: the call never runs on the registering thread") {
    Promise<int> one = gave(3);

    same(one.await(), 3);

    const std::thread::id mine = std::this_thread::get_id();
    std::atomic<bool> aside{false};

    Promise<int> late = one.then([&aside, mine](const int& given) {
        aside.store(std::this_thread::get_id() != mine);
        return given;
    });

    same(late.await(), 3);

    ought(aside.load());
}

TRIAL("Pool: drop takes a job off the queue before it starts") {
    Pool mine{1};

    std::atomic<int> ran{0};
    std::atomic<bool> late{false};

    // The ceiling is one, so the second and the third stand in the queue waiting for their turn.
    for (int at = 0; at < 2; ++at)
        mine.post([&ran] {
            ++ran;
            std::this_thread::sleep_for(std::chrono::milliseconds{60});
        }, Lane::cpu);

    const Pool::Job doomed = mine.post([&late] { late.store(true); }, Lane::cpu);

    mine.drop(doomed);
    mine.drain();

    same(ran.load(), 2);
    ought(!late.load());
}

TRIAL("Pool: drop on a running one wakes it with the bell") {
    Pool mine;

    std::atomic<bool> woke{false};
    std::atomic<bool> ready{false};

    const Pool::Job one = mine.post([&woke, &ready] {
        ready.store(true);

        Bell* bell = ringing();
        if (bell == nullptr) return;

        woke.store(bell->wait(std::chrono::milliseconds{3000}));
    });

    for (int at = 0; at < 200 && !ready.load(); ++at)
        std::this_thread::sleep_for(std::chrono::milliseconds{5});

    std::this_thread::sleep_for(std::chrono::milliseconds{20});

    mine.drop(one);
    mine.drain();

    ought(woke.load());
}

TRIAL("Pool: the bell does not carry over to the next job") {
    Pool mine{1};

    std::atomic<bool> woke{true};

    const Pool::Job one =
        mine.post([] { std::this_thread::sleep_for(std::chrono::milliseconds{40}); }, Lane::cpu);

    mine.post([&woke] {
        Bell* bell = ringing();
        if (bell != nullptr) woke.store(bell->wait(std::chrono::milliseconds{0}));
    }, Lane::cpu);

    mine.drop(one);
    mine.drain();

    ought(!woke.load());
}

TRIAL("Promise: cancel does not run a job standing in the queue") {
    std::atomic<int> ran{0};

    std::vector<Promise<int>> among;

    for (int at = 0; at < 40; ++at)
        among.push_back(Promise<int>{[&ran](auto resolve, auto) {
                                         ++ran;
                                         std::this_thread::sleep_for(std::chrono::milliseconds{20});
                                         resolve(1);
                                     },
                                     Lane::cpu});

    for (auto& one : among) one.start();

    for (std::size_t at = among.size() / 2; at < among.size(); ++at) among[at].cancel();

    pooling().drain();

    ought(ran.load() < 40);
}

namespace {

// An example of what the virtual progress is about: a ticket that knows how many installments out
// of how many are done.
struct Counted : detail::Result<int> {
    std::atomic<int> done{0};
    std::atomic<int> whole{0};

    // The share of installments done, from 0 to 1; zero while the total is unknown.
    [[nodiscard]] double progress() const override {
        const int all = whole.load();
        if (all == 0) return 0.0;

        return std::min(1.0, static_cast<double>(done.load()) / static_cast<double>(all));
    }
};

// A promise that sleeps through `steps` installments and counts them in its ticket.
class Stepping : public Promise<int> {
public:
    explicit Stepping(int steps) : Stepping(steps, std::make_shared<Counted>()) {}

private:
    Stepping(int steps, std::shared_ptr<Counted> made)
        : Promise<int>(
              [steps, made](auto resolve, auto) {
                  made->whole.store(steps);

                  for (int at = 0; at < steps; ++at) {
                      std::this_thread::sleep_for(std::chrono::milliseconds{20});
                      ++made->done;
                  }

                  resolve(steps);
              },
              made) {}
};

}

TRIAL("progress: an ordinary promise knows only zero and one") {
    Promise<int> one = slowly(1, 60);

    same(one.progress(), 0.0);

    same(one.await(), 1);
    same(one.progress(), 1.0);
}

TRIAL("progress: a rejected one is finished too") {
    Promise<int> one{[](auto, auto reject) { reject("zepsute"); }};

    threw((void)one.await());

    same(one.progress(), 1.0);
}

TRIAL("progress: a derived ticket counts its own way") {
    Stepping one{5};

    same(one.progress(), 0.0);

    one.start();

    bool grew = false;

    for (int at = 0; at < 200; ++at) {
        const double part = one.progress();
        if (part > 0.0 && part < 1.0) grew = true;

        std::this_thread::sleep_for(std::chrono::milliseconds{5});

        if (part >= 1.0) break;
    }

    same(one.await(), 5);

    ought(grew);
    same(one.progress(), 1.0);
}

TRIAL("progress: cutting down to Promise does not lose the way of counting") {
    Stepping made{4};
    Promise<int> one = made;

    one.start();

    bool grew = false;

    for (int at = 0; at < 200 && !grew; ++at) {
        const double part = one.progress();
        if (part > 0.0 && part < 1.0) grew = true;

        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    same(one.await(), 4);

    ought(grew);
}

namespace {

// A promise that never starts; it serves only to set the progress by hand.
class Sitting : public Promise<int> {
public:
    Sitting() : Sitting(std::make_shared<Counted>()) {}

    // Sets the progress to `done` out of `whole`.
    void set(int done, int whole) const {
        held_->whole.store(whole);
        held_->done.store(done);
    }

private:
    explicit Sitting(std::shared_ptr<Counted> made)
        : Promise<int>([](auto, auto) {}, made), held_(std::move(made)) {}

    std::shared_ptr<Counted> held_;
};

}

TRIAL("progress: all averages its parts") {
    const Sitting one;
    const Sitting two;
    const Sitting three;

    one.set(10, 10);
    two.set(5, 10);
    three.set(0, 10);

    const Promise<void> joined = Promise<int>::all(one, two, three);

    ought(std::abs(joined.progress() - 0.5) < 0.001);
}

TRIAL("progress: race and any take the largest") {
    const Sitting one;
    const Sitting two;

    one.set(2, 10);
    two.set(7, 10);

    ought(std::abs(Promise<int>::race(one, two).progress() - 0.7) < 0.001);
    ought(std::abs(Promise<int>::any(one, two).progress() - 0.7) < 0.001);
}

TRIAL("progress: all_settled averages too") {
    const Sitting one;
    const Sitting two;

    one.set(1, 4);
    two.set(3, 4);

    ought(std::abs(Promise<int>::all_settled(one, two).progress() - 0.5) < 0.001);
}

TRIAL("progress: a settled join says one") {
    Promise<int> one = gave(1);
    Promise<int> two = gave(2);

    Promise<void> joined = Promise<int>::all(one, two);

    same(joined.progress(), 0.0);

    joined.await();

    same(joined.progress(), 1.0);
}

TRIAL("progress: the range form counts too") {
    std::vector<Sitting> among(4);

    among[0].set(4, 4);
    among[1].set(2, 4);
    among[2].set(0, 4);
    among[3].set(2, 4);

    std::vector<Promise<int>> plain{among.begin(), among.end()};

    ought(std::abs(Promise<int>::all(plain).progress() - 0.5) < 0.001);
}

// A job lives shorter than the announcement of its end.
//
// A job's closure holds what the caller captured: a promise, its result, other handles. The worker
// loop destroys it before it lowers `busy_` under the lock and wakes `drain()`. Otherwise the waiter
// would move on while another thread still runs the destructors of what the waiter handed it, the
// race that TSan reports when a promise's result is freed.
//
// Checked without a clock and without randomness: the closure's destructor sleeps, so if `drain()`
// returned before it, the flag would still be down.
TRIAL("Pool: after draining nobody holds the job's closure anymore") {
    struct Watch {
        std::shared_ptr<std::atomic<bool>> gone;

        // A watch that raises `gone` when the closure holding it is destroyed.
        Watch(std::shared_ptr<std::atomic<bool>> given) : gone(std::move(given)) {}

        Watch(Watch&&) noexcept = default;
        Watch(const Watch&) = default;
        Watch& operator=(Watch&&) noexcept = default;
        Watch& operator=(const Watch&) = default;

        // Sleeps a while and then raises the flag, unless the watch was moved from.
        ~Watch() {
            if (!gone) return;

            std::this_thread::sleep_for(std::chrono::milliseconds{50});

            gone->store(true);
        }
    };

    Pool pool;

    const auto gone = std::make_shared<std::atomic<bool>>(false);
    const auto ran = std::make_shared<std::atomic<bool>>(false);

    (void)pool.post([watch = Watch{gone}, ran] { ran->store(true); });

    pool.drain();

    ought(ran->load());
    ought(gone->load());
}

// Cancelling from another thread, in parallel with `start()`. The pool writes the job number under
// its lock before the job becomes visible to the workers; otherwise `cancel()` could see zero, not
// ring the bell, and the work would run all the way to the end.
//
// The check is one-sided, so the trial cannot be flaky: no body may reach the end after
// cancelling. How many of them start at all depends on the interleaving and is not examined.
TRIAL("Promise: cancelling in parallel with start() always reaches the body") {
    std::atomic<int> ran{0};
    std::atomic<int> heard{0};
    std::atomic<int> whole{0};

    for (int at = 0; at < 200; ++at) {
        Promise<int> one{[&ran, &heard, &whole](auto resolve, auto) {
            ran.fetch_add(1);

            // Work in installments, as walking a tree and copying do: between installments we ask
            // the bell whether we are still needed. The installments last up to two seconds in
            // total, not two milliseconds: cancelling runs in a separate thread, and on a loaded
            // machine that thread can stall for longer than the whole work takes. The work would
            // then honestly finish before the cancel and the trial would fail with good code.
            Bell* bell = ringing();

            for (int step = 0; step < 400; ++step) {
                if (bell != nullptr && bell->wait(std::chrono::milliseconds{5})) {
                    heard.fetch_add(1);

                    return;
                }
            }

            whole.fetch_add(1);

            resolve(1);
        }};

        std::thread other{[&one] { one.cancel(); }};

        one.start();

        other.join();

        try {
            (void)one.await();
        } catch (...) {
        }
    }

    // We wait until the pool calms down, and only then count. `await()` returns as soon as the
    // promise is rejected, and the worker's body may still be running and hear the bell only a
    // moment later. Without this the trial would fail once in a few runs, through no fault of the
    // code.
    pooling().drain();

    same(whole.load(), 0);
    same(heard.load(), ran.load());
}

// A long chain costs as much as it has links. Links share their parent; if each link held two
// copies of its parent, and each copy its own two, building would cost 2^n: six seconds at
// twenty-two links, and two thousand would never finish.
TRIAL("Promise: two thousand links build and settle at once") {
    Promise<int> chain = gave(0);

    for (int at = 0; at < 2000; ++at) chain = chain.then([](int one) { return one + 1; });

    same(chain.await(), 2000);
}

// The waking trampoline does not change the order: a continuation's descendant runs before its
// siblings, just as when waking goes depth-first down the stack.
TRIAL("Promise: continuations wake depth-first, the descendant before its siblings") {
    std::mutex guard;
    std::vector<std::string> order;

    const auto noted = [&guard, &order](std::string one) {
        const std::lock_guard<std::mutex> held{guard};

        order.push_back(std::move(one));
    };

    Promise<void> first{[](auto resolve, auto) { resolve(); }};

    Promise<void> left = first.then([&noted] { noted("b"); });
    Promise<void> deeper = left.then([&noted] { noted("d"); });
    Promise<void> right = first.then([&noted] { noted("c"); });

    Promise<void>::all(deeper, right).await();

    same(order, (std::vector<std::string>{"b", "d", "c"}));
}

// Teardown leaves the link empty, on every library. A moved-from `std::function` need not be empty
// (libc++ copies a small object), so `buried` takes the contents out by swapping: after it the link
// holds nothing, and the last reference is released in the trampoline, not in a member's
// destructor, which would bring teardown back down the stack.
TRIAL("Promise: teardown leaves the link empty, whatever the library") {
    const auto held = std::make_shared<int>(7);

    detail::Upward one{[held] {}, [held] {}};

    same(held.use_count(), 3L);

    detail::buried(one);

    ought(!one.start);
    ought(!one.cancel);
    same(held.use_count(), 1L);
}

// Settling and teardown do not descend the stack. Waking the next continuations on the same stack,
// and freeing the parent from inside the tail's destructor, would cost a stack frame per link: with
// an 8 MB stack settling would fail at a hundred thousand links, with 1 MB (Windows) at a few
// thousand. Two hundred thousand pass here on any stack.
TRIAL("Promise: two hundred thousand links settle and are freed without a deep stack") {
    std::atomic<int> passed{0};

    {
        std::optional<Promise<void>> chain;
        chain.emplace([](auto resolve, auto) { resolve(); });

        for (int at = 0; at < 200000; ++at) chain = chain->then([&passed] { passed.fetch_add(1); });

        chain->await();

        chain.reset();
    }

    same(passed.load(), 200000);
}

#ifndef _WIN32

// Teardown and cancelling on a thread with a small stack, 256 KB, a quarter of the Windows one;
// here with fifty thousand links.
TRIAL("Promise: teardown and cancelling of a long chain fit in a small stack") {
    struct Job {
        int built = 0;
    };

    Job job;

    const auto work = [](void* given) -> void* {
        auto* mine = static_cast<Job*>(given);

        std::optional<Promise<void>> chain;
        chain.emplace([](auto resolve, auto) { resolve(); });

        for (int at = 0; at < 50000; ++at) chain = chain->then([] {});

        chain->await();
        chain.reset();

        std::optional<Promise<void>> idle;
        idle.emplace([](auto resolve, auto) { resolve(); });

        for (int at = 0; at < 50000; ++at) idle = idle->then([] {});

        idle->cancel();
        idle.reset();

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
