#include "tal/task.hpp"

#include <string>
#include <utility>

// How tasks run. A chain is one root coroutine and everything it awaits, innermost last; only one
// frame of a chain runs at a time, on the chain's executor. A chain is in one of four states:
// running, waiting (suspended at a leaf: a pending result, a timer, a move to another executor),
// doomed (cancelled while waiting: its root is being destroyed) and ended. Leaves wake a chain from
// any thread through `resume`, which posts to the executor and resumes only a chain still waiting;
// cancellation dooms a waiting chain at once and a running one at its next suspension. A doomed
// chain is destroyed from the root without resuming anything, so only destructors of its frames
// run, and the leaf awaiters withdraw themselves from what they waited for (`Leaf::bury`). Each
// leaf has its own lock, held from registering until the chain is marked waiting, so that a result
// arriving on another thread cannot resume a chain that has not suspended yet, and the destruction
// of a doomed chain waits until the awaiter that suspended it has let go of its frame.

namespace tal {

namespace detail {

// Gives the chain its root coroutine, suspended at its start, and the work to run once the chain
// ends or is destroyed.
void Chain::rooted(std::coroutine_handle<> root, Work gone) {
    root_ = root;
    gone_ = std::move(gone);
}

// Starts the root on the chain's executor; a chain cancelled before it ran is destroyed instead.
void Chain::start() { resume(root_); }

// Cancels the chain: a waiting one is doomed at once, a running one at its next suspension, and
// one that blocks in a call of the system is woken out of it (`heed`).
void Chain::cancel() {
    cancelled_.store(true);

    {
        const std::lock_guard<std::mutex> kept{heeding_};

        if (heeded_ != nullptr) heeded_->woken();
    }

    int was = waiting;

    if (state_.compare_exchange_strong(was, doomed)) doom();
}

// The chain has suspended at a leaf. Called with the leaf's lock held, before anyone can wake it.
// A cancellation that came while it ran dooms it now.
void Chain::suspended() {
    state_.store(waiting);

    if (!cancelled_.load()) return;

    int was = waiting;

    if (state_.compare_exchange_strong(was, doomed)) doom();
}

// Wakes the chain at `leaf` on its executor, unless it was doomed meanwhile.
void Chain::resume(std::coroutine_handle<> leaf) {
    where().post([self = shared_from_this(), leaf] {
        int was = waiting;

        if (self->state_.compare_exchange_strong(was, running)) leaf.resume();
    });
}

// The root ended on its own; whatever it held is let go.
void Chain::finished() {
    state_.store(ended);

    if (gone_) std::exchange(gone_, {})();
}

// Whether the chain is over: ended, or doomed and so never to run again.
bool Chain::over() const {
    const int now = state_.load();

    return now == ended || now == doomed;
}

namespace {

// The destruction of a doomed chain, posted to its executor. Work dropped unrun (a loop destroyed
// with it still queued) destroys the chain all the same, so no frame is left behind.
struct Dooming {
    std::shared_ptr<Chain> chain;

    // Holds the chain to destroy.
    explicit Dooming(std::shared_ptr<Chain> given) : chain(std::move(given)) {}

    // Takes the chain over from a moved-from destruction.
    Dooming(Dooming&& other) noexcept : chain(std::exchange(other.chain, {})) {}

    Dooming(const Dooming&) = delete;
    Dooming& operator=(const Dooming&) = delete;
    Dooming& operator=(Dooming&&) = delete;

    // Destroys the chain if the work never ran.
    ~Dooming() {
        if (chain) chain->destroyed();
    }

    // Destroys the chain now.
    void operator()() {
        if (const auto held = std::exchange(chain, {})) held->destroyed();
    }
};

}

// Destroys the root on the chain's executor, which destroys every frame below it from the root
// down, without resuming any.
void Chain::doom() { where().post(Dooming{shared_from_this()}); }

// Destroys the root and lets go of what the chain held; once.
void Chain::destroyed() {
    if (root_) std::exchange(root_, {}).destroy();

    if (gone_) std::exchange(gone_, {})();
}

// Gives what wakes the chain out of a blocking call of the system when it is cancelled, or none
// once the call is over; a cancellation that came before is told at once.
void Chain::heed(Waking* one) {
    const std::lock_guard<std::mutex> kept{heeding_};

    heeded_ = one;

    if (heeded_ != nullptr && cancelled_.load()) heeded_->woken();
}

// Wakes the chain waiting here, once: a leaf whose awaiter is gone, or that already fired, does
// nothing.
void Leaf::fire() {
    const std::lock_guard<std::mutex> kept{guard};

    if (!armed || dead) return;

    armed = false;
    chain->resume(handle);
}

// The awaiter is gone with its frame; a later result must not touch it.
void Leaf::bury() {
    const std::lock_guard<std::mutex> kept{guard};

    dead = true;
    armed = false;
}

}

// A scope whose work starts on `where`, the program's loop unless given.
Scope::Scope(Executor& where) : state_(std::make_shared<State>()), where_(&where) {}

// Cancels all the work of the scope. It does not wait: each piece is destroyed on its own executor
// at its next suspension, so the owner may go at once. A task must not destroy the scope that runs
// it while it runs, since it would go on running in its destroyed owner until it suspends.
Scope::~Scope() { cancel(); }

// Cancels all the work of the scope; the scope can take new work afterwards.
void Scope::cancel() {
    std::vector<std::shared_ptr<detail::Chain>> all;

    {
        const std::lock_guard<std::mutex> kept{state_->guard};

        for (const auto& [id, weak] : state_->chains)
            if (auto one = weak.lock()) all.push_back(std::move(one));
    }

    for (const auto& one : all) one->cancel();
}

// Whether some work of the scope has not ended yet.
bool Scope::busy() const {
    const std::lock_guard<std::mutex> kept{state_->guard};

    return !state_->chains.empty();
}

// A number for the next piece of work.
std::uint64_t Scope::State::reserved() {
    const std::lock_guard<std::mutex> kept{guard};

    return ++numbered;
}

// Keeps a piece of work, so that cancelling the scope reaches it.
void Scope::State::keep(std::uint64_t id, const std::shared_ptr<detail::Chain>& chain) {
    const std::lock_guard<std::mutex> kept{guard};

    chains.emplace(id, chain);
}

// Forgets a piece of work that ended or was destroyed.
void Scope::State::forget(std::uint64_t id) {
    const std::lock_guard<std::mutex> kept{guard};

    chains.erase(id);
}

// A failure that left a piece of work unhandled goes to the log and stops a debug build
// (`detail::unhandled`); cancellation is no failure.
void Scope::failed(const std::exception_ptr& why) {
    if (!why) return;

    try {
        std::rethrow_exception(why);
    } catch (const Cancelled&) {
    } catch (const std::exception& one) {
        detail::unhandled(std::string{"Unhandled failure in a task: "} + one.what());
    } catch (...) {
        detail::unhandled("Unhandled failure in a task.");
    }
}

}
