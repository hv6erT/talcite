#include "tal/executor.hpp"

#include <algorithm>
#include <atomic>
#include <utility>

#include "tal/task.hpp"
#include "tal/trouble.hpp"

namespace tal {

namespace {

// The program's loop: the first one constructed that still lives. Trials make one loop each, one
// after another, and each becomes the program's loop for its lifetime.
std::atomic<Loop*> program_loop{nullptr};

// Held while the program's loop changes and while a timer takes the life of that loop, so that a
// loop going away cannot be taken halfway.
std::mutex program_guard;

}

// What outlives a loop of it: whether the loop still is, for the timers that withdraw themselves
// later than the loop went.
struct Loop::Life {
    std::mutex guard;
    Loop* loop = nullptr;
};

// A loop on the given clock; it becomes the program's loop when there is none.
Loop::Loop(Port<Clock> time) : time_(std::move(time)), life_(std::make_shared<Life>()) {
    life_->loop = this;

    Loop* none = nullptr;
    const std::lock_guard<std::mutex> kept{program_guard};

    (void)program_loop.compare_exchange_strong(none, this);
}

// First waits for the chains away from the loop (`drained`). Then stops being the program's loop,
// and tells the timers that outlive it that it is gone. Work still queued and timers still waiting
// are dropped then: dropping them may destroy frames of cancelled tasks, whose timers then withdraw
// nothing. A frame waiting on this loop never resumes.
Loop::~Loop() {
    drained();

    {
        Loop* self = this;
        const std::lock_guard<std::mutex> kept{program_guard};

        (void)program_loop.compare_exchange_strong(self, nullptr);
    }

    {
        const std::lock_guard<std::mutex> kept{life_->guard};

        life_->loop = nullptr;
    }

    for (;;) {
        std::deque<Work> left;
        std::map<std::uint64_t, Work> waiting;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            left.swap(ready_);
            waiting.swap(timers_);
            due_.clear();
        }

        if (left.empty() && waiting.empty()) break;
    }
}

// Queues work from any thread and wakes the loop if it waits.
void Loop::post(Work what) {
    {
        const std::lock_guard<std::mutex> kept{guard_};

        ready_.push_back(std::move(what));
    }

    stirred_.notify_all();
    stirred();
}

// One turn: the work queued so far and the timers due by the loop's clock, run outside the lock,
// so that work may post more work. Whether anything ran.
bool Loop::turn() {
    std::deque<Work> now;
    std::vector<Work> fired;

    {
        const std::lock_guard<std::mutex> kept{guard_};

        now.swap(ready_);

        const auto at = time_->now();

        while (!due_.empty() && due_.begin()->first <= at) {
            const std::uint64_t which = due_.begin()->second;

            due_.erase(due_.begin());

            if (const auto found = timers_.find(which); found != timers_.end()) {
                fired.push_back(std::move(found->second));
                timers_.erase(found);
            }
        }
    }

    for (Work& one : now) one();
    for (Work& one : fired) one();

    return !now.empty() || !fired.empty();
}

// Waits for work at most until `latest`, then turns once as `turn()` does. Work posted, a timer
// already due, a timer falling due (only on a clock that runs by itself; a manual one moves only
// by hand) or a stop ends the wait early. Returns whether any work ran, so a trial can wait for a
// condition by a deadline instead of counting turns.
bool Loop::turn(std::chrono::steady_clock::time_point latest) {
    {
        std::unique_lock<std::mutex> kept{guard_};

        const auto stirred = [this] { return !ready_.empty() || stopping_; };
        const bool due = !due_.empty() && due_.begin()->first <= time_->now();

        if (!stirred() && !due) {
            auto until = latest;

            if (!due_.empty() && !time_->manual()) until = std::min(until, due_.begin()->first);

            (void)stirred_.wait_until(kept, until, stirred);
        }
    }

    return turn();
}

// Waits until work is queued, the next timer falls due or the loop is stopped. On a manual clock
// time moves only by hand, so the wait is for work alone: such a loop is driven by `turn`.
void Loop::pause() {
    std::unique_lock<std::mutex> kept{guard_};

    if (!ready_.empty() || stopping_) return;

    if (timers_.empty() || time_->manual()) {
        stirred_.wait(kept, [this] { return !ready_.empty() || stopping_; });

        return;
    }

    const auto next = due_.begin()->first;

    stirred_.wait_until(kept, next, [this] { return !ready_.empty() || stopping_; });
}

// Turns until stopped and returns the code given to `stop`; the loop can run again afterwards.
int Loop::run() {
    for (;;) {
        {
            const std::lock_guard<std::mutex> kept{guard_};

            if (stopping_) {
                stopping_ = false;

                return code_;
            }
        }

        if (!turn()) pause();
    }
}

// Asks `run` to return with `code` after the current turn; from any thread.
void Loop::stop(int code) {
    {
        const std::lock_guard<std::mutex> kept{guard_};

        stopping_ = true;
        code_ = code;
    }

    stirred_.notify_all();
    stirred();
}

// Runs `what` on the loop once its clock reaches `when`; the number withdraws it (`unwake`).
std::uint64_t Loop::wake(std::chrono::steady_clock::time_point when, Work what) {
    std::uint64_t which = 0;

    {
        const std::lock_guard<std::mutex> kept{guard_};

        which = ++numbered_;
        due_.emplace(when, which);
        timers_.emplace(which, std::move(what));
    }

    stirred_.notify_all();
    stirred();

    return which;
}

// Withdraws a timer that has not fired; its place in the order of deadlines goes when it is due.
void Loop::unwake(std::uint64_t which) { const Work dropped = withdrawn(which); }

// The work of the timer `which`, taken out unless it fired; the caller drops it outside the lock.
Work Loop::withdrawn(std::uint64_t which) {
    const std::lock_guard<std::mutex> kept{guard_};

    if (const auto found = timers_.find(which); found != timers_.end()) {
        Work taken = std::move(found->second);

        timers_.erase(found);

        return taken;
    }

    return {};
}

// When the earliest timer that has not been withdrawn falls due, or nothing when none waits.
std::optional<std::chrono::steady_clock::time_point> Loop::next() const {
    const std::lock_guard<std::mutex> kept{guard_};

    for (const auto& [when, which] : due_)
        if (timers_.contains(which)) return when;

    return std::nullopt;
}

// Told after work is posted, a timer set or a stop asked for, from whatever thread did it, once the
// loop's own waiting has been woken. A loop that waits elsewhere (an application waiting on its
// console) overrides it to end that wait; the loop itself needs nothing more.
void Loop::stirred() {}

// A chain left the loop for another executor; the loop waits for it before it goes.
void Loop::left() {
    const std::lock_guard<std::mutex> kept{guard_};

    ++away_;
}

// A chain away from the loop came back, ended or was destroyed. The loop is woken under its lock,
// so that a loop going away waits until this call has let go of it.
void Loop::returned() {
    const std::lock_guard<std::mutex> kept{guard_};

    --away_;
    stirred_.notify_all();
}

// Turns until no chain is away from the loop and nothing is queued: work away on a pool comes back
// here, or ends there, or (cancelled) is destroyed there, so that none of it reaches a loop that is
// gone. The loop works meanwhile as ever: what came back runs here until it ends or waits, what was
// doomed is destroyed here, timers fire as their clock says.
void Loop::drained() {
    for (;;) {
        {
            std::unique_lock<std::mutex> kept{guard_};

            const auto woken = [this] { return !ready_.empty() || away_ == 0; };
            const bool due = !due_.empty() && due_.begin()->first <= time_->now();

            if (!woken() && !due) {
                if (due_.empty() || time_->manual()) stirred_.wait(kept, woken);
                else (void)stirred_.wait_until(kept, due_.begin()->first, woken);
            }

            if (away_ == 0 && ready_.empty()) return;
        }

        (void)turn();
    }
}

// Whether nothing is queued and no timer waits.
bool Loop::idle() const {
    const std::lock_guard<std::mutex> kept{guard_};

    return ready_.empty() && timers_.empty();
}

// A pool of the given number of threads, started at once.
Pool::Pool(unsigned threads) {
    for (unsigned at = 0; at < std::max(threads, 1U); ++at) threads_.emplace_back([this] { worked(); });
}

// Stops taking work, lets every thread finish the work it holds and joins them. Work still queued
// is dropped, round after round, since dropping it may post more (a doomed chain destroyed unrun
// wakes the chains it held) until none is left.
Pool::~Pool() {
    {
        const std::lock_guard<std::mutex> kept{guard_};

        stopping_ = true;
    }

    stirred_.notify_all();

    for (std::thread& one : threads_) one.join();

    for (;;) {
        std::deque<Work> left;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            left.swap(ready_);
        }

        if (left.empty()) break;
    }
}

// Queues work for the first free thread; from any thread.
void Pool::post(Work what) {
    {
        const std::lock_guard<std::mutex> kept{guard_};

        ready_.push_back(std::move(what));
    }

    stirred_.notify_one();
}

// A thread of the pool: takes work while there is any, until the pool stops.
void Pool::worked() {
    for (;;) {
        Work next;

        {
            std::unique_lock<std::mutex> kept{guard_};

            stirred_.wait(kept, [this] { return stopping_ || !ready_.empty(); });

            if (stopping_) return;

            next = std::move(ready_.front());
            ready_.pop_front();
        }

        next();
    }
}

// The program's loop; there is none before the first `Loop` is made.
Loop& loop() {
    Loop* const found = program_loop.load();

    if (found == nullptr) throw Trouble{Kind::missing, "No loop is running."};

    return *found;
}

// The pool for work, one thread per core and at least two.
Executor& pool() {
    static Pool made{std::max(2U, std::thread::hardware_concurrency())};

    return made;
}

// The pool for calls that block on the system (a slow disk, a sleeping network share), so that
// they do not hold up the work of the other pool.
Executor& io() {
    static Pool made{16};

    return made;
}

namespace detail {

// A timer of a wait, set on a loop that may go before it: the loop's life and the timer's number.
struct Alarm {
    std::shared_ptr<Loop::Life> life;
    std::uint64_t which = 0;
};

// Runs `what` after `wait` on the loop of `where`: the executor itself when it is a loop, the
// program's loop otherwise, which may go first, even while the timer is set; then it never fires.
// The timer is set under the loop's life, so that the loop waits for it before it goes.
std::shared_ptr<Alarm> alarmed(Executor& where, std::chrono::milliseconds wait, Work what) {
    auto made = std::make_shared<Alarm>();

    if (auto* const own = dynamic_cast<Loop*>(&where); own != nullptr) {
        made->life = own->life_;
    } else {
        const std::lock_guard<std::mutex> kept{program_guard};
        Loop* const found = program_loop.load();

        if (found == nullptr) throw Trouble{Kind::missing, "No loop is running."};

        made->life = found->life_;
    }

    const std::lock_guard<std::mutex> kept{made->life->guard};

    if (Loop* const on = made->life->loop; on != nullptr) made->which = on->wake(on->clock().now() + wait, std::move(what));

    return made;
}

// Withdraws the timer `which` from its loop, unless the loop is gone; its work is dropped outside
// both locks, since dropping it may destroy frames whose own timers withdraw themselves.
void unalarmed(Alarm& which) {
    Work dropped;
    const std::lock_guard<std::mutex> kept{which.life->guard};

    if (which.life->loop != nullptr) dropped = which.life->loop->withdrawn(which.which);
}

}

}
