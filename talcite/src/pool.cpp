#include "talcite/pool.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

namespace talcite {

namespace {

// The ceiling for computing work: `most`, or the number of cores when `most` is zero (one when the
// system does not say).
std::size_t roomy(std::size_t most) {
    if (most != 0) return most;

    const unsigned told = std::thread::hardware_concurrency();

    return told == 0 ? 1 : static_cast<std::size_t>(told);
}

// The bell of the task that is running on this thread right now. A blocking operation written in
// talcite waits on its descriptor together with this bell, so drop() wakes it normally instead of
// interrupting its system call. Outside a task it is null.
thread_local Bell* standing = nullptr;

// The pool whose worker this thread is, and whether the task it runs now holds a computing slot;
// `Waiting` gives the slot back while the task waits.
thread_local Pool* serving = nullptr;
thread_local bool computing = false;

// Takes the job `which` out of the queue `from`, if it is there; the closure comes back so that it
// can be destroyed after the lock is released. The caller holds the lock.
std::function<void()> pulled(std::map<Pool::Job, std::function<void()>>& from, Pool::Job which) {
    const auto at = from.find(which);

    if (at == from.end()) return {};

    std::function<void()> made = std::move(at->second);

    from.erase(at);

    return made;
}

}

// The bell of the pool task running on the calling thread, or null outside a task.
Bell* ringing() { return standing; }

// A pool whose computing lane runs at most `most` tasks at once (zero means one per core). A worker
// that has had nothing to do for `rest` leaves, as long as more than `most` workers remain.
Pool::Pool(std::size_t most, std::chrono::milliseconds rest) : most_(roomy(most)), rest_(rest) {}

// Tells the workers to finish and joins them, those that have left on their own too. Queued
// computing work still runs before they leave.
Pool::~Pool() {
    {
        const std::lock_guard<std::mutex> held{guard_};
        closing_ = true;
    }

    stirred_.notify_all();

    // The list is taken under the lock and joined outside it. `handed` may add a worker from
    // another thread (a closure released after a task can post one last piece of work), and then
    // the vector reallocates and a range loop would read freed memory. Joining under the lock is
    // not allowed, because a finishing worker reaches for that lock itself.
    std::vector<std::thread> these;

    {
        const std::lock_guard<std::mutex> held{guard_};
        these.swap(staff_);
    }

    for (std::thread& one : these)
        if (one.joinable()) one.join();
}

// Hands work over for execution and returns its number for `drop`. There is one word for "hand
// over for execution", the same as `Loop::post`: once to a loop, once to the pool. The lane says
// whether the work waits for the system (`io`) or computes (`cpu`).
Pool::Job Pool::post(std::function<void()> what, Lane lane) {
    return handed(std::move(what), lane, nullptr);
}

// Hands work over and stores its number in `into` before the task becomes visible. The version
// without that place returns the number only from `return`, that is, after the task has been
// queued and a worker woken, and then whoever wants to cancel the task has a window in which it
// already runs but there is no number yet. `drop()` then has nothing to take off and no bell to
// ring, so cancelled work goes through the whole job to the end. Measured on 4000 promises: of
// 1523 bodies that started, 467 did not hear the bell.
Pool::Job Pool::post(std::function<void()> what, Lane lane, std::atomic<Job>& into) {
    return handed(std::move(what), lane, &into);
}

// Queues the work and adds a worker when needed (`hired`). Two queues, one staff. Work on the `io`
// lane may wait for a file or for a dead network share, so the pool grows for it without limit;
// otherwise the ninth such task would stop the whole program. Work on the `cpu` lane declares that
// it only computes, so there is no point in running more of it at once than the machine has cores:
// two hundred computing tasks on eight cores finish slower than eight. The queues are keyed by the
// job's number, which grows, so they keep the order of posting and `drop` finds a job at once.
Pool::Job Pool::handed(std::function<void()> what, Lane lane, std::atomic<Job>* into) {
    std::vector<std::thread> gone;
    Job which = 0;

    {
        std::unique_lock<std::mutex> held{guard_};

        which = ++next_;

        // Before queueing, not after. A worker takes the task off under the same lock, so whoever
        // sees the number here is sure that the task is at least in the queue, and whoever sees
        // zero knows that no worker has picked it up yet.
        if (into != nullptr) into->store(which);

        if (lane == Lane::cpu) cpu_.emplace(which, std::move(what));
        else io_.emplace(which, std::move(what));

        try {
            hired(true);
        } catch (...) {
            std::function<void()> dropped = pulled(lane == Lane::cpu ? cpu_ : io_, which);

            if (into != nullptr) into->store(0);

            held.unlock();

            throw;
        }

        gone = reaped();
    }

    stirred_.notify_one();

    for (std::thread& one : gone) one.join();

    return which;
}

// How many queued tasks could start right now: every blocking one, and computing ones as far as
// the ceiling has free slots. The caller holds the lock.
std::size_t Pool::runnable() const {
    const std::size_t free = cpu_busy_ < most_ ? most_ - cpu_busy_ : 0;

    return io_.size() + std::min(cpu_.size(), free);
}

// Adds a worker when more tasks could start than there are idle workers to start them. The count
// compares with the length of the queues, not with zero: a worker woken a moment ago still counts
// as idle until it takes a task off itself, so the rule "add one when nobody is idle" would, for a
// handful of tasks posted at once, add one worker and make the rest wait. Computing work counts
// only as far as the ceiling lets it run, so workers stuck in blocking work never leave computing
// work without anyone to take it.
//
// The worker's bell is made here, before its thread: a bell is a pair of descriptors, and a pool
// that has run out of them must say so to whoever posts, not end the program from a worker thread.
// When nothing can be added, the work waits for a worker that frees itself; only a pool without any
// worker, which nobody would ever serve, gives the error back (`strict`). The caller holds the lock.
void Pool::hired(bool strict) {
    if (closing_ || runnable() <= idle_) return;

    try {
        auto bell = std::make_unique<Bell>();

        staff_.emplace_back([this, mine = std::move(bell)]() mutable { serve(std::move(mine)); });

        ++alive_;
        ++idle_;
    } catch (...) {
        if (strict && alive_ == 0) throw;
    }
}

// The threads of the workers that have left on their own, taken out of the staff so that the caller
// joins them after releasing the lock. The caller holds the lock.
std::vector<std::thread> Pool::reaped() {
    std::vector<std::thread> made;

    for (const std::thread::id one : retired_) {
        const auto at = std::find_if(staff_.begin(), staff_.end(), [one](const std::thread& each) { return each.get_id() == one; });

        if (at == staff_.end()) continue;

        made.push_back(std::move(*at));
        staff_.erase(at);
    }

    retired_.clear();

    return made;
}

// Cancels task `which`. A task that has not started yet simply falls out of the queue and costs
// nothing; its closure is destroyed after the lock is released, since destructors may come back to
// the pool. A task that runs gets its bell rung: its system call is not interrupted, it is only
// woken from waiting, provided it was waiting on the bell together with its descriptor.
void Pool::drop(Job which) {
    std::function<void()> dropped;

    {
        const std::lock_guard<std::mutex> held{guard_};

        dropped = pulled(io_, which);

        if (!dropped) dropped = pulled(cpu_, which);

        if (!dropped) {
            const auto at = running_.find(which);

            if (at != running_.end()) at->second->ring();

            return;
        }
    }

    emptied_.notify_all();
}

// Waits until both queues are empty and no task is running.
void Pool::drain() {
    std::unique_lock<std::mutex> held{guard_};
    emptied_.wait(held, [this] { return io_.empty() && cpu_.empty() && busy_ == 0; });
}

// How many worker threads the pool has now.
std::size_t Pool::staff() const {
    const std::lock_guard<std::mutex> held{guard_};

    return alive_;
}

// The loop of one worker: takes `io` work first, then `cpu` work while the computing ceiling
// allows, and returns once the pool is closing and nothing is left for it, or after `rest_` without
// work while the staff is larger than the ceiling: a burst of blocking work on a dead share leaves
// many workers behind, and each of them holds a thread and two descriptors.
void Pool::serve(std::unique_ptr<Bell> mine) {
    Bell& bell = *mine;

    standing = &bell;
    serving = this;

    const auto ready = [this] { return closing_ || !io_.empty() || (!cpu_.empty() && cpu_busy_ < most_); };

    while (true) {
        std::function<void()> what;
        Job which = 0;
        bool cpu = false;

        {
            std::unique_lock<std::mutex> held{guard_};

            if (!stirred_.wait_for(held, rest_, ready)) {
                if (alive_ <= most_) continue;

                --idle_;
                --alive_;
                retired_.push_back(std::this_thread::get_id());

                standing = nullptr;
                serving = nullptr;

                return;
            }

            if (!io_.empty()) {
                which = io_.begin()->first;
                what = std::move(io_.begin()->second);
                io_.erase(io_.begin());
            } else if (!cpu_.empty() && (cpu_busy_ < most_ || closing_)) {
                which = cpu_.begin()->first;
                what = std::move(cpu_.begin()->second);
                cpu_.erase(cpu_.begin());

                cpu = true;
                ++cpu_busy_;
            } else {
                if (closing_) {
                    --idle_;
                    --alive_;

                    standing = nullptr;
                    serving = nullptr;

                    return;
                }

                continue;
            }

            // The bell is cleared under the same lock under which the task enters the running set.
            // Otherwise a ring from drop() just before the previous task ended would leave the bell
            // raised, and the next task would wake for no reason.
            bell.clear();
            running_.emplace(which, &bell);

            --idle_;
            ++busy_;
        }

        computing = cpu;

        // An exception from here has nowhere to go: the promise has already recorded it as a
        // rejection, and a worker thread has no one to hand it to. It is swallowed deliberately.
        try {
            what();
        } catch (...) {
        }

        computing = false;

        // The closure is released before we announce that we are idle. It holds what the caller
        // captured (a promise, its result, other handles), and `drain()` waits for `busy_ == 0` and
        // after returning may assume that nobody touches those any more. Left until the end of the
        // loop round, it would be destroyed after that announcement, on another thread and without
        // any agreement with whoever just moved on.
        //
        // It is released without the lock: destroying the closure runs other people's destructors,
        // and those may come back to the pool.
        what = nullptr;

        {
            const std::lock_guard<std::mutex> held{guard_};

            running_.erase(which);

            --busy_;
            ++idle_;
            if (cpu) --cpu_busy_;

            // A freed computing slot may let queued computing work start while this worker turns to
            // blocking work, so the staff is checked again.
            hired(false);
        }

        // A freed computing slot can take one waiting worker, so one is woken, not all. Blocking
        // work takes no slot, so nobody has to be woken after it: this worker checks the queue
        // itself in its next round.
        emptied_.notify_all();

        if (cpu) stirred_.notify_one();
    }
}

// A computing task that is about to wait for other work (`Promise::await`) gives its slot back for
// the length of the wait: otherwise tasks that each wait for another computing task could fill
// every slot with waiting and leave the work they wait for without a slot, for good. Outside a
// computing task it does nothing.
Waiting::Waiting() {
    if (serving == nullptr || !computing) return;

    pool_ = serving;
    computing = false;

    {
        const std::lock_guard<std::mutex> held{pool_->guard_};

        --pool_->cpu_busy_;

        pool_->hired(false);
    }

    pool_->stirred_.notify_one();
}

// Takes the slot again after the wait. It does not wait for a free one: the task is already running,
// and for a moment the ceiling is exceeded by the tasks that came back from waiting.
Waiting::~Waiting() {
    if (pool_ == nullptr) return;

    {
        const std::lock_guard<std::mutex> held{pool_->guard_};

        ++pool_->cpu_busy_;
    }

    computing = true;
}

// The program's shared pool, created on first use.
Pool& pooling() {
    static Pool only;

    return only;
}

}
