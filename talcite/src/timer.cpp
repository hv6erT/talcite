#include "talcite/timer.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace talcite::ui {

// The appointments of a root's clock: each under its number, with the moment it is due, its period
// (zero for a single one) and what it calls; the order of moments and numbers says which comes
// first, and for one moment the earlier made. The lock guards both, because a timer may be dropped
// on another thread than the one the root runs on, together with whatever held it.
struct detail::Timers {
    struct Appointment {
        talcite::Clock::Moment when;
        std::chrono::milliseconds period{0};
        std::shared_ptr<std::function<void()>> then;
    };

    mutable std::mutex guard;
    std::map<std::uint64_t, Appointment> all;
    std::set<std::pair<talcite::Clock::Moment, std::uint64_t>> order;
    std::uint64_t next = 0;
};

namespace {

// Takes appointment `number` out of the clock, if it is still there; what it would have called goes
// after the lock, since its captures may hold timers of their own.
void dropped(detail::Timers& timers, std::uint64_t number) {
    std::shared_ptr<std::function<void()>> gone;

    {
        const std::lock_guard<std::mutex> held{timers.guard};

        const auto at = timers.all.find(number);

        if (at == timers.all.end()) return;

        timers.order.erase({at->second.when, number});
        gone = std::move(at->second.then);
        timers.all.erase(at);
    }
}

}

// A timer is the appointment it stands for: the appointment lasts exactly as long as whoever keeps
// the timer, and dropping the timer takes it back. The timer reaches the clock only weakly, so one
// that outlives its root has nothing to take back. Moving hands the appointment over; the timer
// moved from is empty.
//
// Only the root's clock makes a timer with an appointment (`friend class Clock` in the header). The
// friend declaration names `talcite::ui::Clock`, declared after the timer: the standard looks for a
// first-declared friend only in the innermost namespace. MSVC looks further out, finds
// `talcite::Clock` and befriends that one, so under MSVC the header declares `Clock` beforehand.
Timer::Timer(Timer&& other) noexcept : timers_(std::move(other.timers_)), number_(std::exchange(other.number_, 0)) {}

// Takes back what this timer held, then takes over the other one's appointment.
Timer& Timer::operator=(Timer&& other) noexcept {
    if (this != &other) {
        stop();

        timers_ = std::move(other.timers_);
        number_ = std::exchange(other.number_, 0);
    }

    return *this;
}

// Takes back the appointment, if it still waits.
Timer::~Timer() { stop(); }

// Whether the appointment still waits: made, not taken back, and, for a single one, not yet kept.
bool Timer::active() const {
    if (number_ == 0) return false;

    const std::shared_ptr<detail::Timers> timers = timers_.lock();

    if (!timers) return false;

    const std::lock_guard<std::mutex> held{timers->guard};

    return timers->all.contains(number_);
}

// Takes back the appointment before its time; the timer is empty afterwards.
void Timer::stop() {
    const std::uint64_t gone = std::exchange(number_, 0);

    const std::shared_ptr<detail::Timers> timers = timers_.lock();

    timers_.reset();

    if (gone != 0 && timers) dropped(*timers, gone);
}

// The clock of a root reads the time from `time`, the real one in a program and a manual one in
// trials, so that a trial moves time on by itself instead of waiting for it.
Clock::Clock(talcite::Clock& time) : time_(&time), timers_(std::make_shared<detail::Timers>()) {}

// The appointments go together with the clock: a timer that outlives it has nothing left to take
// back.
Clock::~Clock() = default;

// An appointment to call `then` once, `delay` from now. A negative delay counts as none: the call
// comes at the next turn of the loop. An empty `then` only wakes the loop, for a frame of a node
// that computes its picture from the time. The timer given back keeps the appointment.
Timer Clock::after(std::chrono::milliseconds delay, std::function<void()> then) {
    return kept(now() + std::max(delay, std::chrono::milliseconds{0}), std::chrono::milliseconds{0}, std::move(then));
}

// An appointment to call `then` every `period`, the first time a period from now. A period shorter
// than a millisecond counts as one, so an appointment cannot hold the loop in one turn. The timer
// given back keeps the appointment.
Timer Clock::every(std::chrono::milliseconds period, std::function<void()> then) {
    const std::chrono::milliseconds step = std::max(period, std::chrono::milliseconds{1});

    return kept(now() + step, step, std::move(then));
}

// The time as the clock reads it.
talcite::Clock::Moment Clock::now() const { return time_->now(); }

// The moment of the nearest appointment, or nothing when none waits: the loop sleeps until then, or
// until something else wakes it.
std::optional<talcite::Clock::Moment> Clock::next() const {
    const std::lock_guard<std::mutex> held{timers_->guard};

    if (timers_->order.empty()) return std::nullopt;

    return timers_->order.begin()->first;
}

// Makes an appointment and gives back the timer that keeps it.
Timer Clock::kept(talcite::Clock::Moment when, std::chrono::milliseconds period, std::function<void()> then) {
    const std::lock_guard<std::mutex> held{timers_->guard};

    const std::uint64_t number = ++timers_->next;

    timers_->all.emplace(number, detail::Timers::Appointment{when, period, std::make_shared<std::function<void()>>(std::move(then))});
    timers_->order.emplace(when, number);

    return Timer{timers_, number};
}

// Calls what is due by now, in the order of the moments and, for one moment, in the order the
// appointments were made; says whether anything was called. Only appointments made before this
// call count: one made by a call now waits for the next turn, even without a delay, so a call that
// makes its successor at once cannot hold the loop. Every call may make, take back or drop any
// appointment, its own included, and one taken back meanwhile is not called. A repeating
// appointment moves on along its own grid of periods, not from now, so it does not drift; one late
// by several periods (a sleeping computer, a long block) is called once and moves to the first of
// its moments still ahead, rather than catching up on the missed ones.
bool Clock::elapse() {
    const std::shared_ptr<detail::Timers> timers = timers_;
    const talcite::Clock::Moment at = now();

    std::vector<std::uint64_t> due;

    {
        const std::lock_guard<std::mutex> held{timers->guard};

        for (const auto& [when, number] : timers->order) {
            if (when > at) break;

            due.push_back(number);
        }
    }

    bool called = false;

    for (const std::uint64_t number : due) {
        std::shared_ptr<std::function<void()>> then;

        {
            const std::lock_guard<std::mutex> held{timers->guard};

            const auto found = timers->all.find(number);

            if (found == timers->all.end()) continue;

            detail::Timers::Appointment& one = found->second;

            then = one.then;

            timers->order.erase({one.when, number});

            if (one.period > std::chrono::milliseconds{0}) {
                const auto behind = std::chrono::duration_cast<std::chrono::milliseconds>(at - one.when);

                one.when += one.period * (behind / one.period + 1);
                timers->order.emplace(one.when, number);
            } else {
                timers->all.erase(found);
            }
        }

        called = true;

        if (*then) (*then)();
    }

    return called;
}

}
