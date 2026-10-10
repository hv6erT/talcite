#include "talcite/pool.hpp"

#include <algorithm>
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

}

// The bell of the pool task running on the calling thread, or null outside a task.
Bell* ringing() { return standing; }

// A pool whose computing lane runs at most `most` tasks at once (zero means one per core).
Pool::Pool(std::size_t most) : most_(roomy(most)) {}

// Tells the workers to finish and joins them. Queued computing work still runs before they leave.
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

// Queues the work and adds a worker when needed. Two queues, one staff. Work on the `io` lane may
// wait for a file or for a dead network share, so the pool grows for it without limit; otherwise
// the ninth such task would stop the whole program. Work on the `cpu` lane declares that it only
// computes, so there is no point in running more of it at once than the machine has cores: two
// hundred computing tasks on eight cores finish slower than eight.
Pool::Job Pool::handed(std::function<void()> what, Lane lane, std::atomic<Job>* into) {
    std::unique_lock<std::mutex> held{guard_};

    const Job which = ++next_;

    // Before queueing, not after. A worker takes the task off under the same lock, so whoever sees
    // the number here is sure that the task is at least in the queue, and whoever sees zero knows
    // that no worker has picked it up yet.
    if (into != nullptr) into->store(which);

    if (lane == Lane::cpu) cpu_.push_back(Doing{which, std::move(what)});
    else io_.push_back(Doing{which, std::move(what)});

    // We compare with the length of the queue, not with zero. A worker woken a moment ago still
    // counts as idle until it takes a task off itself, so the rule "add one when nobody is idle"
    // would, for a handful of tasks posted at once, add one worker and make the rest wait in the
    // queue.
    const bool hasty = io_.size() > idle_;
    const bool slack = !hasty && cpu_.size() > idle_ && spare_ < most_;

    if (hasty || slack) {
        staff_.emplace_back([this] { serve(); });
        ++idle_;

        // Workers added for computing work are counted separately: otherwise blocking work that
        // occupied the whole staff would starve the computing work, with the ceiling used up by
        // other threads.
        if (slack) ++spare_;
    }

    held.unlock();
    stirred_.notify_one();

    return which;
}

// Removes task `which` from the queue `from`; returns whether it was there. The caller holds the
// lock.
bool Pool::pulled(std::deque<Doing>& from, Job which) {
    const auto at = std::find_if(from.begin(), from.end(),
                                 [which](const Doing& one) { return one.which == which; });

    if (at == from.end()) return false;

    from.erase(at);

    return true;
}

// Cancels task `which`. A task that has not started yet simply falls out of the queue and costs
// nothing. A task that runs gets its bell rung: its system call is not interrupted, it is only
// woken from waiting, provided it was waiting on the bell together with its descriptor.
void Pool::drop(Job which) {
    const std::lock_guard<std::mutex> held{guard_};

    if (pulled(io_, which) || pulled(cpu_, which)) {
        emptied_.notify_all();
        return;
    }

    const auto at = running_.find(which);
    if (at != running_.end()) at->second->ring();
}

// Waits until both queues are empty and no task is running.
void Pool::drain() {
    std::unique_lock<std::mutex> held{guard_};
    emptied_.wait(held, [this] { return io_.empty() && cpu_.empty() && busy_ == 0; });
}

// How many worker threads the pool has started.
std::size_t Pool::staff() const {
    const std::lock_guard<std::mutex> held{guard_};

    return staff_.size();
}

// The loop of one worker: takes `io` work first, then `cpu` work while the computing ceiling
// allows, and returns once the pool is closing and nothing is left for it.
void Pool::serve() {
    Bell bell;

    standing = &bell;

    while (true) {
        Doing one;
        bool cpu = false;

        {
            std::unique_lock<std::mutex> held{guard_};

            stirred_.wait(held, [this] {
                return closing_ || !io_.empty() || (!cpu_.empty() && cpu_busy_ < most_);
            });

            if (!io_.empty()) {
                one = std::move(io_.front());
                io_.pop_front();
            } else if (!cpu_.empty() && (cpu_busy_ < most_ || closing_)) {
                one = std::move(cpu_.front());
                cpu_.pop_front();

                cpu = true;
                ++cpu_busy_;
            } else {
                if (closing_) {
                    standing = nullptr;
                    return;
                }

                continue;
            }

            // The bell is cleared under the same lock under which the task enters the running set.
            // Otherwise a ring from drop() just before the previous task ended would leave the bell
            // raised, and the next task would wake for no reason.
            bell.clear();
            running_.emplace(one.which, &bell);

            --idle_;
            ++busy_;
        }

        // An exception from here has nowhere to go: the promise has already recorded it as a
        // rejection, and a worker thread has no one to hand it to. It is swallowed deliberately.
        try {
            one.what();
        } catch (...) {
        }

        // The closure is released before we announce that we are idle. It holds what the caller
        // captured (a promise, its result, other handles), and `drain()` waits for `busy_ == 0` and
        // after returning may assume that nobody touches those any more. Left until the end of the
        // loop round, it would be destroyed after that announcement, on another thread and without
        // any agreement with whoever just moved on.
        //
        // It is released without the lock: destroying the closure runs other people's destructors,
        // and those may come back to the pool.
        one.what = nullptr;

        {
            const std::lock_guard<std::mutex> held{guard_};

            running_.erase(one.which);

            --busy_;
            ++idle_;
            if (cpu) --cpu_busy_;
        }

        // A freed computing slot can take one waiting worker, so one is woken, not all. Blocking
        // work takes no slot, so nobody has to be woken after it: this worker checks the queue
        // itself in its next round.
        emptied_.notify_all();

        if (cpu) stirred_.notify_one();
    }
}

// The program's shared pool, created on first use.
Pool& pooling() {
    static Pool only;

    return only;
}

}
