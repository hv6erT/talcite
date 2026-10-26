#include "talcite/time.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include "talcite/promise.hpp"

namespace talcite {

namespace {

// The real clock: one thread for the whole library. It sleeps until the nearest deadline; a new
// deadline nearer than the current one wakes it through the condition variable. A hundred waits are
// still one thread, not a hundred.
//
// The entries stand in two indexes: by number, for `unscheduled`, and by deadline, for the thread,
// so each operation costs a logarithm instead of a walk through every wait. A screen full of
// countdowns, or ten thousand timeouts withdrawn as their work ends first, would otherwise pay for
// each one with the whole list, under the lock that every other wait needs.
class Real final : public Clock {
public:
    Real() = default;

    // Starts the timer thread.
    void begin() { keeper_ = std::thread{[this] { serve(); }}; }

    // The monotonic clock.
    [[nodiscard]] Moment now() const override { return std::chrono::steady_clock::now(); }

    // Runs `then` on the timer thread at `when`; returns the number to withdraw it with. Only a
    // deadline that becomes the nearest wakes the thread: a later one is found when it wakes
    // anyway.
    [[nodiscard]] std::uint64_t scheduled(Moment when, std::function<void()> then) override {
        std::uint64_t which = 0;
        bool nearest = false;

        {
            const std::lock_guard<std::mutex> held{guard_};

            which = ++next_;

            const auto placed = order_.emplace(when, which).first;

            try {
                waiting_.emplace(which, Entry{when, std::move(then)});
            } catch (...) {
                order_.erase(placed);

                throw;
            }

            nearest = placed == order_.begin();
        }

        if (nearest) stirred_.notify_all();

        return which;
    }

    // Withdraws the entry `which`, if it is still waiting; whether it was. The entry is destroyed only
    // after the lock is released. The thread is not woken: if it sleeps until this entry, it wakes,
    // finds nothing due and sleeps until the next one.
    [[nodiscard]] bool unscheduled(std::uint64_t which) override {
        Entry taken;

        {
            const std::lock_guard<std::mutex> held{guard_};

            const auto at = waiting_.find(which);

            if (at == waiting_.end()) return false;

            taken = std::move(at->second);

            order_.erase({taken.when, which});
            waiting_.erase(at);
        }

        return true;
    }

private:
    struct Entry {
        Moment when;
        std::function<void()> then;
    };

    // The loop of the timer thread: sleeps until the nearest deadline, then runs every entry that is
    // due, outside the lock. The callbacks are the library's own and only hand the ending over to
    // the io lane; anything they throw is swallowed so that the rest still run.
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

            for (Entry& one : due) {
                try {
                    one.then();
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

}

// The real clock, created on first use. It is deliberately never destroyed. Static objects are
// destroyed in the reverse order of their creation, and the clock is created later than the pool,
// so it would go first, while the pool's workers are still running and may call it. Instead of
// arranging a dependency between two singletons, the clock simply outlives the program.
Clock& Clock::real() {
    static Real* only = [] {
        auto* made = new Real;
        made->begin();

        return made;
    }();

    return *only;
}

// The time of the manual clock: it stands until `advance` moves it.
Clock::Moment ManualClock::now() const {
    const std::lock_guard<std::mutex> held{guard_};

    return now_;
}

// Keeps `then` until `advance` reaches `when`; returns the number to withdraw it with.
std::uint64_t ManualClock::scheduled(Moment when, std::function<void()> then) {
    const std::lock_guard<std::mutex> held{guard_};

    const std::uint64_t which = ++next_;

    waiting_.emplace(which, std::make_pair(when, std::move(then)));
    order_.emplace(when, which);

    return which;
}

// Withdraws the entry `which`; whether it was still waiting.
bool ManualClock::unscheduled(std::uint64_t which) {
    std::function<void()> taken;

    {
        const std::lock_guard<std::mutex> held{guard_};

        const auto at = waiting_.find(which);

        if (at == waiting_.end()) return false;

        order_.erase({at->second.first, which});

        taken = std::move(at->second.second);

        waiting_.erase(at);
    }

    return true;
}

// Moves the time by `by` and runs, on this thread and in the order of their deadlines, everything
// that became due; entries scheduled by those callbacks for the time already reached run as well.
void ManualClock::advance(std::chrono::milliseconds by) {
    Moment until;

    {
        const std::lock_guard<std::mutex> held{guard_};

        now_ += by;
        until = now_;
    }

    for (;;) {
        std::function<void()> then;

        {
            const std::lock_guard<std::mutex> held{guard_};

            if (order_.empty() || order_.begin()->first > until) return;

            const auto at = waiting_.find(order_.begin()->second);

            then = std::move(at->second.second);

            waiting_.erase(at);
            order_.erase(order_.begin());
        }

        then();
    }
}

// How many entries wait for their time.
std::size_t ManualClock::waiting() const {
    const std::lock_guard<std::mutex> held{guard_};

    return waiting_.size();
}

// A promise fulfilled `patience` after this call, by `clock`. The ending comes on the io lane;
// abandoned, it withdraws its entry from the clock at once.
Promise<void> later(std::chrono::milliseconds patience, Clock& clock) {
    auto [made, settle] = promised<void>();

    auto shared = std::make_shared<Settle<void>>(std::move(settle));

    const std::uint64_t which = clock.scheduled(clock.now() + patience, [shared] { (void)shared->value(); });

    Clock* const whose = &clock;

    detail::Core* const core = detail::Access::core(shared->stop());

    (void)core->on_stop([whose, which] { (void)whose->unscheduled(which); });

    return std::move(made);
}

Stopwatch::Stopwatch() : began_(std::chrono::steady_clock::now()) {}

// The time since the stopwatch was made or last reset.
std::chrono::milliseconds Stopwatch::spent() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began_);
}

// Starts counting again from now.
void Stopwatch::reset() { began_ = std::chrono::steady_clock::now(); }

// The current moment of the monotonic clock.
std::chrono::steady_clock::time_point Time::now() { return std::chrono::steady_clock::now(); }

// The current wall-clock time, truncated to whole seconds.
std::chrono::sys_seconds Time::stamp() {
    return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
}

}
