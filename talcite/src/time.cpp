#include "talcite/time.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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
// The entries stand in a `detail::Deadlines`, the table of numbered deadlines this clock, the
// manual one and the schedule of a root (timer.cpp) all keep. Every entry stands in two indexes: by
// number, for withdrawing it, and by deadline and then number, so that for one deadline the earlier
// added comes first, for finding what is due. So each operation costs a logarithm instead of a walk
// through every wait. A screen full of countdowns, or ten thousand timeouts withdrawn as their work
// ends first, would otherwise pay for each one with the whole list, under the lock that every other
// wait needs. The table has no lock of its own, its owner holds one, and adding changes nothing
// when it throws.
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

            which = waiting_.added(when, std::move(then));
            nearest = waiting_.earliest(which);
        }

        if (nearest) stirred_.notify_all();

        return which;
    }

    // Withdraws the entry `which`, if it is still waiting; whether it was. The entry is destroyed only
    // after the lock is released. The thread is not woken: if it sleeps until this entry, it wakes,
    // finds nothing due and sleeps until the next one.
    [[nodiscard]] bool unscheduled(std::uint64_t which) override {
        std::optional<Waiting::Entry> taken;

        {
            const std::lock_guard<std::mutex> held{guard_};

            taken = waiting_.taken(which);
        }

        return taken.has_value();
    }

private:
    using Waiting = detail::Deadlines<std::function<void()>>;

    // The loop of the timer thread: sleeps until the nearest deadline, then runs every entry that is
    // due, outside the lock. The callbacks are the library's own and only hand the ending over to
    // the io lane; anything they throw is swallowed so that the rest still run.
    void serve() {
        while (true) {
            std::vector<Waiting::Entry> due;

            {
                std::unique_lock<std::mutex> held{guard_};

                const std::optional<Moment> nearest = waiting_.nearest();

                if (!nearest) {
                    stirred_.wait(held);
                    continue;
                }

                const Moment now = std::chrono::steady_clock::now();

                if (now < *nearest) {
                    stirred_.wait_until(held, *nearest);
                    continue;
                }

                while (std::optional<Waiting::Entry> one = waiting_.due(now)) due.push_back(std::move(*one));
            }

            for (Waiting::Entry& one : due) {
                try {
                    one.what();
                } catch (...) {
                }
            }
        }
    }

    std::mutex guard_;
    std::condition_variable stirred_;
    Waiting waiting_;
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

    return waiting_.added(when, std::move(then));
}

// Withdraws the entry `which`; whether it was still waiting. The entry goes after the lock.
bool ManualClock::unscheduled(std::uint64_t which) {
    std::optional<detail::Deadlines<std::function<void()>>::Entry> taken;

    {
        const std::lock_guard<std::mutex> held{guard_};

        taken = waiting_.taken(which);
    }

    return taken.has_value();
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

            std::optional<detail::Deadlines<std::function<void()>>::Entry> one = waiting_.due(until);

            if (!one) return;

            then = std::move(one->what);
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
