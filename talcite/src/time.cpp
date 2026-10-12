#include "talcite/time.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace talcite {

namespace {

using Moment = std::chrono::steady_clock::time_point;

class Timing;

// The ticket of a wait. Its progress is known exactly (so much of so much has passed), so a
// countdown draws a progress bar for free. The start moment is recorded only at start(), because
// the promise is lazy and there is nothing to count before it runs.
class Countdown : public detail::Result<void> {
public:
    explicit Countdown(std::chrono::milliseconds whole) : whole_(whole) {}

    // Records the moment the wait started; until then progress stays at zero.
    void begun(Moment when) {
        began_.store(when.time_since_epoch().count(), std::memory_order_relaxed);
    }

    // The share of the wait that has passed, from 0 to 1; 1 once the ticket is settled.
    [[nodiscard]] double progress() const override {
        if (over(*this)) return 1.0;

        const std::int64_t began = began_.load(std::memory_order_relaxed);
        if (began == 0 || whole_.count() <= 0) return 0.0;

        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - Moment{Moment::duration{began}});

        return std::clamp(static_cast<double>(spent.count()) /
                              static_cast<double>(whole_.count()),
                          0.0, 1.0);
    }

private:
    std::atomic<std::int64_t> began_{0};
    std::chrono::milliseconds whole_;
};

// A promise that reaches for the protected constructor of Promise to supply its own ticket.
class Awaiting : public Promise<void> {
public:
    Awaiting(Promise<void>::Callback given, std::shared_ptr<Countdown> made)
        : Promise<void>(std::move(given), std::move(made)) {}
};

// The watcher hooked on a ticket: when the ticket settles by other means, it removes the entry
// from the timer.
class Dropping : public detail::Waiter {
public:
    Dropping(Timing& whose, std::uint64_t which) : whose_(whose), which_(which) {}

    void woke() override;

private:
    Timing& whose_;
    std::uint64_t which_;
};

// One thread for the whole library. It sleeps until the nearest deadline; a new deadline nearer
// than the current one wakes it through the condition variable. A hundred waits are still one
// thread, not a hundred.
//
// The entries stand in two indexes: by number, for `drop`, and by deadline, for the thread, so
// each operation costs a logarithm instead of a walk through every wait. A screen full of
// countdowns, or ten thousand timeouts dropped as their work ends first, would otherwise pay for
// each one with the whole list, under the lock that every other wait needs.
class Timing {
public:
    Timing() = default;

    ~Timing() = default;

    Timing(const Timing&) = delete;
    Timing& operator=(const Timing&) = delete;
    Timing(Timing&&) = delete;
    Timing& operator=(Timing&&) = delete;

    // Starts the timer thread.
    void begin() { keeper_ = std::thread{[this] { serve(); }}; }

    // Settles `result` at `when`: with success, or with `Overdue` when `bad` is set. If the
    // promise settles earlier by itself, the entry is dropped.
    void after(Moment when, std::shared_ptr<detail::Result<void>> result, bool bad) {
        std::shared_ptr<detail::Waiter> watch;
        std::uint64_t which = 0;

        {
            const std::lock_guard<std::mutex> held{guard_};

            which = ++next_;
            watch = std::make_shared<Dropping>(*this, which);

            const auto placed = order_.emplace(when, which).first;

            try {
                waiting_.emplace(which, Entry{when, result, watch, bad});
            } catch (...) {
                order_.erase(placed);

                throw;
            }
        }

        stirred_.notify_all();

        // Outside the lock, because a promise that is already settled calls the watcher at once,
        // and the watcher comes back here through drop(); under the lock that would deadlock on
        // our own thread.
        if (detail::hooked(*result, watch)) drop(which);
    }

    // Removes the entry `which`, if it is still waiting. The entry is moved out and destroyed
    // only after the lock is released.
    void drop(std::uint64_t which) {
        Entry taken;

        {
            const std::lock_guard<std::mutex> held{guard_};

            const auto at = waiting_.find(which);

            if (at == waiting_.end()) return;

            taken = std::move(at->second);

            order_.erase({taken.when, which});
            waiting_.erase(at);
        }

        stirred_.notify_all();
    }

private:
    struct Entry {
        Moment when;
        std::shared_ptr<detail::Result<void>> result;
        std::shared_ptr<detail::Waiter> watch;
        bool bad = false;
    };

    // The loop of the timer thread: sleeps until the nearest deadline, then settles every entry
    // that is due.
    void serve() {
        while (true) {
            std::vector<Entry> due;

            {
                std::unique_lock<std::mutex> held{guard_};

                if (order_.empty()) {
                    stirred_.wait(held);
                    continue;
                }

                const Moment now = std::chrono::steady_clock::now();
                const Moment nearest = order_.begin()->first;

                if (now < nearest) {
                    stirred_.wait_until(held, nearest);
                    continue;
                }

                while (!order_.empty() && order_.begin()->first <= now) {
                    const auto at = waiting_.find(order_.begin()->second);

                    due.push_back(std::move(at->second));

                    waiting_.erase(at);
                    order_.erase(order_.begin());
                }
            }

            // Settling happens outside the lock: it wakes the watchers, among them our own
            // watcher that removes the entry and comes back here for the lock. Settling also runs
            // whatever was chained to the promise, and an exception from there would end the
            // program from this thread; the promise is settled before the watchers run, so there
            // is nobody left to tell, and the exception is swallowed so the rest still settle.
            for (Entry& one : due) {
                try {
                    if (one.bad) detail::spoiled(*one.result, std::make_exception_ptr(Overdue{}));
                    else detail::settled(*one.result);
                } catch (...) {
                }
            }
        }
    }

    std::mutex guard_;
    std::condition_variable stirred_;
    std::map<std::uint64_t, Entry> waiting_;
    std::set<std::pair<Moment, std::uint64_t>> order_;
    std::uint64_t next_ = 0;
    std::thread keeper_;
};

// The timer is deliberately never destroyed. Static objects are destroyed in the reverse order of
// their creation, and `Timing` is created later than `Pool`, so it would go first, while the
// pool's workers are still running and may call it. Instead of arranging a dependency between
// two singletons, the timer simply outlives the program.
Timing& timing() {
    static Timing* only = [] {
        auto* made = new Timing;
        made->begin();

        return made;
    }();

    return *only;
}

// The ticket has settled: the timer forgets the entry.
void Dropping::woke() { whose_.drop(which_); }

// A lazy promise that settles `patience` after it is started: with success, or with `Overdue`
// when `bad` is set.
Promise<void> counted(std::chrono::milliseconds patience, bool bad) {
    auto held = std::make_shared<Countdown>(patience);

    return Awaiting{[patience, held, bad](auto, auto) {
                        const Moment now = std::chrono::steady_clock::now();

                        held->begun(now);
                        timing().after(now + patience, held, bad);
                    },
                    held};
}

}

Overdue::Overdue() : Trouble("Timed out.") {}

Stopwatch::Stopwatch() : began_(std::chrono::steady_clock::now()) {}

// The time since the stopwatch was made or last reset.
std::chrono::milliseconds Stopwatch::spent() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - began_);
}

// Starts counting again from now.
void Stopwatch::reset() { began_ = std::chrono::steady_clock::now(); }

// The current moment of the monotonic clock.
std::chrono::steady_clock::time_point Time::now() { return std::chrono::steady_clock::now(); }

// The current wall-clock time, truncated to whole seconds.
std::chrono::sys_seconds Time::stamp() {
    return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
}

// A promise that succeeds after `patience`.
Promise<void> Time::later(std::chrono::milliseconds patience) { return counted(patience, false); }

// A promise that fails with `Overdue` after `patience`.
Promise<void> Time::limit(std::chrono::milliseconds patience) { return counted(patience, true); }

// The deadline for `Promise::timeout`. The promise header cannot include `time.hpp`, because
// that one includes it, so the declaration stands in `detail` and the body here, next to the timer.
Promise<void> detail::deadline(std::chrono::milliseconds patience) {
    return counted(patience, true);
}

}
