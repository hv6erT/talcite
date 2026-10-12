#include "talcite/loop.hpp"

#include <exception>
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

// Frees the thread's place for another loop.
Loop::~Loop() {
    if (standing == this) standing = nullptr;
}

// The loop of the current thread, or `nullptr` when none stands on it.
Loop* Loop::current() { return standing; }

// Queues work for the loop and rings its bell. The queue and the bell live in the door
// (`detail::Door`), not in the loop itself. The loop holds the door strongly, and promise
// continuations (`here`, `on`) hold it weakly. With a raw pointer to the loop, pool work finishing
// after the loop was destroyed would call `post` on a dead object, and if a new loop stood at the
// same address, the continuation would run on someone else's loop. The door dies with the loop,
// and anything that arrives after it is dropped, with nowhere to go.
void detail::Door::post(std::function<void()> what) {
    {
        const std::lock_guard<std::mutex> held{guard_};
        queued_.push_back(std::move(what));
    }

    bell_.ring();
}

// Takes the whole queue at once, leaving it empty.
std::vector<std::function<void()>> detail::Door::taken() {
    std::vector<std::function<void()>> made;

    const std::lock_guard<std::mutex> held{guard_};

    made.swap(queued_);

    return made;
}

// The number of queued pieces of work.
std::size_t detail::Door::waiting() const {
    const std::lock_guard<std::mutex> held{guard_};

    return queued_.size();
}

// Queues work to run on this loop's thread at its next turn. Safe to call from any thread.
void Loop::post(std::function<void()> what) { door_->post(std::move(what)); }

// The number of pieces of work waiting for the next turn.
std::size_t Loop::waiting() const { return door_->waiting(); }

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
// A piece of work that throws does not take the rest of the batch with it: the batch has already
// left the queue, so whatever stood after the thrower would be lost for good, and with it the
// continuation somebody waits for. Every piece runs, and the first exception is passed on after
// the last one.
std::size_t Loop::turn() {
    door_->bell().clear();

    const std::vector<std::function<void()>> taken = door_->taken();

    std::exception_ptr first;

    for (const std::function<void()>& one : taken) {
        try {
            one();
        } catch (...) {
            if (!first) first = std::current_exception();
        }
    }

    if (first) std::rethrow_exception(first);

    return taken.size();
}

}
