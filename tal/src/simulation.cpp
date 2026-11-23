#include "tal/simulation.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <string>
#include <thread>

// The clockwork of a simulation (E7e): steps of a fixed length on a thread of their own, owed by
// the real time the program says has passed (`advance`) times the pace, so that a step is the same
// however often the screen is drawn. Time is counted in nanoseconds, so that a second at steps of
// 0.01 s is a hundred steps, not ninety-nine. What is owed while paused is not kept; a single step
// is taken paused or not. A batch of steps that takes more than a quarter of a second of real work
// lets go of what is still owed: a simulation slower than real time slows down rather than falls
// further and further behind. After every batch, and whenever the program changed the state, the
// state is handed over (`settle`); the program's loop never waits for a step. A step's Trouble (or
// any other exception, as Trouble other) stops the simulation and is kept.
namespace tal {

// What the thread and the program share, under one lock.
struct Ticking::Held {
    std::function<void()> step;
    std::function<void()> settle;
    std::chrono::nanoseconds length{1};
    double length_seconds = 0;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::chrono::nanoseconds owed{0};
    double scale = 1;
    bool running = true;
    bool single = false;
    bool changed = false;
    bool working = false;
    bool stopping = false;
    std::optional<Trouble> trouble;
    std::atomic<std::uint64_t> steps{0};
    std::thread thread;

    // Whether there is anything for the thread to do; under the lock.
    [[nodiscard]] bool due() const { return single || changed || (running && owed >= length); }

    // The thread: waits for work, takes the changes, steps while steps are owed, hands the state
    // over.
    void worked() {
        std::unique_lock<std::mutex> lock{mutex};

        for (;;) {
            wake.wait(lock, [this] { return stopping || due(); });

            if (stopping) return;

            bool once = single;

            single = false;
            changed = false;
            working = true;
            lock.unlock();

            try {
                settle();

                const auto begun = std::chrono::steady_clock::now();

                for (;;) {
                    lock.lock();

                    const bool go = !stopping && (once || (running && owed >= length));

                    if (go && !once) owed -= length;

                    lock.unlock();

                    if (!go) break;

                    once = false;
                    step();
                    ++steps;

                    if (std::chrono::steady_clock::now() - begun > std::chrono::milliseconds{250}) {
                        lock.lock();
                        owed = std::chrono::nanoseconds{0};
                        lock.unlock();

                        break;
                    }
                }

                settle();
            } catch (const Trouble& why) {
                lock.lock();
                trouble = why;
                running = false;
                owed = std::chrono::nanoseconds{0};
                lock.unlock();
            } catch (const std::exception& why) {
                lock.lock();
                trouble = Trouble{Kind::other, why.what()};
                running = false;
                owed = std::chrono::nanoseconds{0};
                lock.unlock();
            }

            lock.lock();
            working = false;
        }
    }
};

// Steps of `dt` seconds by `step`, the state handed over by `settle`, running at once. Throws
// Trouble malformed for a step that is not positive or not finite.
Ticking::Ticking(double dt, std::function<void()> step, std::function<void()> settle) : held_(std::make_unique<Held>()) {
    if (!std::isfinite(dt) || dt <= 0 || dt > 3600) throw Trouble{Kind::malformed, "A step of a simulation that is not between 0 and an hour."};

    held_->step = std::move(step);
    held_->settle = std::move(settle);
    held_->length = std::chrono::nanoseconds{std::max<long long>(1, std::llround(dt * 1e9))};
    held_->length_seconds = dt;
    held_->thread = std::thread{[held = held_.get()] { held->worked(); }};
}

// Stops the thread and waits for it.
Ticking::~Ticking() {
    {
        const std::lock_guard<std::mutex> lock{held_->mutex};

        held_->stopping = true;
    }

    held_->wake.notify_all();
    held_->thread.join();
}

// `real` time passed, owed as `real` times the pace while running.
void Ticking::advance(std::chrono::nanoseconds real) {
    if (real.count() <= 0) return;

    {
        const std::lock_guard<std::mutex> lock{held_->mutex};

        if (!held_->running) return;

        held_->owed += std::chrono::nanoseconds{std::llround(static_cast<double>(real.count()) * held_->scale)};
    }

    held_->wake.notify_all();
}

// One step, paused or not.
void Ticking::step() {
    {
        const std::lock_guard<std::mutex> lock{held_->mutex};

        held_->single = true;
    }

    held_->wake.notify_all();
}

// Running or paused; pausing forgets what is owed.
void Ticking::run(bool on) {
    {
        const std::lock_guard<std::mutex> lock{held_->mutex};

        held_->running = on;

        if (!on) held_->owed = std::chrono::nanoseconds{0};

        if (on) held_->trouble.reset();
    }

    held_->wake.notify_all();
}

// Whether it runs.
bool Ticking::running() const {
    const std::lock_guard<std::mutex> lock{held_->mutex};

    return held_->running;
}

// The pace: simulated seconds to a second of real time. Throws Trouble malformed for a pace that
// is negative or not finite.
void Ticking::pace(double scale) {
    if (!std::isfinite(scale) || scale < 0 || scale > 1e6) throw Trouble{Kind::malformed, "A pace that is negative, too large or not a finite number."};

    const std::lock_guard<std::mutex> lock{held_->mutex};

    held_->scale = scale;
}

// The pace.
double Ticking::pace() const {
    const std::lock_guard<std::mutex> lock{held_->mutex};

    return held_->scale;
}

// The program changed the state: the thread takes the change in, between steps.
void Ticking::woken() {
    {
        const std::lock_guard<std::mutex> lock{held_->mutex};

        held_->changed = true;
    }

    held_->wake.notify_all();
}

// Whether the thread works or has work owed.
bool Ticking::busy() const {
    const std::lock_guard<std::mutex> lock{held_->mutex};

    return held_->working || held_->due();
}

// The steps taken.
std::uint64_t Ticking::steps() const { return held_->steps.load(); }

// The length of a step, in seconds.
double Ticking::dt() const { return held_->length_seconds; }

// The trouble a step threw, which stopped the simulation; none while it runs well.
std::optional<Trouble> Ticking::trouble() const {
    const std::lock_guard<std::mutex> lock{held_->mutex};

    return held_->trouble;
}

}
