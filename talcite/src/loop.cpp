#include "talcite/loop.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

#include "batched.hpp"

namespace talcite {

namespace {

// A loop belongs to a thread, not to the process. That makes a second screen or a thread with its
// own cycle possible, and tests share no state with each other.
thread_local Loop* standing = nullptr;

}

// Makes this the loop of the current thread; a second loop on the same thread throws
// std::logic_error.
Loop::Loop() {
    if (standing != nullptr)
        throw std::logic_error("talcite::Loop: a loop already stands on this thread");

    standing = this;
}

// Frees the thread for another loop. The loop stops taking work first (`Executor::close`), so a
// continuation that arrives from the pool while the loop is going away is refused, not queued into
// a dying object. Then the work still queued is thrown away, while the loop still stands on this
// thread: what a promise handed over is abandoned (`posted`), so its chain ends cancelled and only
// its cleanup runs (`finally`, a coroutine going on with `Cancelled`), here, on the loop's thread.
// No `then` or `recover` runs against objects that are being destroyed.
Loop::~Loop() {
    close();

    std::vector<std::function<void()>> left;

    {
        const std::lock_guard<std::mutex> held{guard_};

        left.swap(queued_);
    }

    left.clear();

    if (standing == this) standing = nullptr;
}

// The loop of the current thread, or `nullptr` when none stands on it.
Loop* Loop::current() { return standing; }

// Queues work for the loop's thread at its next turn and rings its bell. Safe to call from any
// thread. The work goes in under the lock and the bell rings after it, so a turn that clears the
// bell first never loses what came in between (see `turn`). Only the work that finds the queue
// empty rings: whatever stood there before rang already, and that ring is cleared only by a turn
// that then takes the whole queue, this work with it. A burst of posts is one system call.
bool Loop::post(std::function<void()> what) {
    bool first = false;

    {
        const std::lock_guard<std::mutex> held{guard_};

        first = queued_.empty();

        queued_.push_back(std::move(what));
    }

    if (first) bell_.ring();

    return true;
}

// The number of pieces of work waiting for the next turn.
std::size_t Loop::waiting() const {
    const std::lock_guard<std::mutex> held{guard_};

    return queued_.size();
}

// Runs the queued work and returns how much ran. The queue is taken whole and the lock released
// before running anything; otherwise a call that queues something itself would deadlock on its
// own loop, and that is the most ordinary case there is.
//
// The bell is cleared before the queue is taken, not after. The reverse order would lose a
// wake-up: `post` appends under the lock and only then rings, so a call squeezed in between taking
// and clearing would leave its work in the queue while its ring went to waste, and the loop would
// stop in poll with no deadline until the next key. In this order the worst that can happen is
// one idle turn.
//
// A piece of work that throws does not take the rest of the batch with it (`detail::ran`): every
// piece runs, and the first exception is passed on after the last one.
std::size_t Loop::turn() {
    bell_.clear();

    std::vector<std::function<void()>> taken;

    {
        const std::lock_guard<std::mutex> held{guard_};

        taken.swap(queued_);
    }

    const std::exception_ptr first = detail::ran(taken);

    const std::size_t ran = taken.size();

    // The emptied vector goes back with its room, when nothing came meanwhile, so a busy loop does
    // not grow its queue anew every turn.
    taken.clear();

    {
        const std::lock_guard<std::mutex> held{guard_};

        if (queued_.empty()) queued_.swap(taken);
    }

    if (first) std::rethrow_exception(first);

    return ran;
}

}
