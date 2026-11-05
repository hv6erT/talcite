#include "talcite/loop.hpp"

#include <stdexcept>
#include <utility>

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

    queued_.dropped();

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
    if (queued_.queued(std::move(what))) bell_.ring();

    return true;
}

// The number of pieces of work waiting for the next turn.
std::size_t Loop::waiting() const { return queued_.size(); }

// Runs the queued work and returns how much ran (`detail::Queue::turned`: the queue is taken whole
// and the lock released before running anything, and a piece that throws does not take the rest of
// the batch with it).
//
// The bell is cleared before the queue is taken, not after. The reverse order would lose a
// wake-up: `post` appends under the lock and only then rings, so a call squeezed in between taking
// and clearing would leave its work in the queue while its ring went to waste, and the loop would
// stop in poll with no deadline until the next key. In this order the worst that can happen is
// one idle turn.
std::size_t Loop::turn() {
    bell_.clear();

    return queued_.turned();
}

}
