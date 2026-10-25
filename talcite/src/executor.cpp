#include "talcite/executor.hpp"

#include <atomic>
#include <exception>
#include <memory>
#include <utility>

#include "talcite/detail/promise.hpp"

namespace talcite {

namespace {

// A piece of work a promise hands to an executor, with what to do when the executor throws it away
// unrun: a loop or a manual executor that ends with work in its queue. Only work the executor took
// is abandoned (`arm`), and only when it never ran: the last copy of the task going away decides,
// on the thread that drops it, which is the executor's own as it ends. What the abandonment throws
// has nobody to go to (`lose`).
class Errand {
public:
    // The work and what to do instead when it is thrown away unrun.
    Errand(std::function<void()> what, std::function<void()> abandoned) : what_(std::move(what)), abandoned_(std::move(abandoned)) {}

    Errand(const Errand&) = delete;
    Errand& operator=(const Errand&) = delete;
    Errand(Errand&&) = delete;
    Errand& operator=(Errand&&) = delete;

    // Abandons the work when the executor took it and dropped it without running it.
    ~Errand() {
        if (!armed_.load() || ran_.load() || !abandoned_) return;

        try {
            abandoned_();
        } catch (...) {
            detail::lose(std::current_exception());
        }
    }

    // Runs the work; from now on it is not abandoned, whatever it throws.
    void run() {
        ran_.store(true);

        what_();
    }

    // The executor has taken the work: dropping it unrun now abandons it.
    void arm() { armed_.store(true); }

private:
    std::function<void()> what_;
    std::function<void()> abandoned_;
    std::atomic<bool> armed_{false};
    std::atomic<bool> ran_{false};
};

}

// Every executor has a gate through which promises reach it later (`on`). A promise holds the gate,
// not the executor, so an executor that has gone away refuses what arrives instead of being called
// dead.
Executor::Executor() : gate_(std::make_shared<detail::Gate>()) { gate_->executor = this; }

// Closes the gate, if the derived executor has not done so already.
Executor::~Executor() { close(); }

// Closes the gate: from now on nothing reaches the executor through it. A derived executor calls
// this first thing in its destructor, before its own members go, so that no post arrives at a
// half-destroyed object; a post in flight is waited for.
void Executor::close() {
    const std::lock_guard<std::mutex> held{gate_->guard};

    gate_->executor = nullptr;
}

// Stops taking work, then throws the queue away: what a promise handed over is abandoned
// (`posted`), so its chain ends cancelled instead of waiting for a turn that never comes. What the
// abandonment posts meets the closed gate.
ManualExecutor::~ManualExecutor() {
    close();

    std::vector<std::function<void()>> left;

    {
        const std::lock_guard<std::mutex> held{guard_};

        left.swap(queued_);
    }

    left.clear();
}

// Queues the work until `turn`.
bool ManualExecutor::post(std::function<void()> what) {
    const std::lock_guard<std::mutex> held{guard_};

    queued_.push_back(std::move(what));

    return true;
}

// Runs what was queued when it was called and returns how much that was; work queued meanwhile
// waits for the next turn. A piece of work that throws does not take the rest of the batch with it,
// as in `Loop::turn`: every piece runs, and the first exception is passed on after the batch.
// Otherwise the rest would be thrown away unrun while the executor is alive.
std::size_t ManualExecutor::turn() {
    std::vector<std::function<void()>> taken;

    {
        const std::lock_guard<std::mutex> held{guard_};

        taken.swap(queued_);
    }

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

// How much work waits for the next turn.
std::size_t ManualExecutor::waiting() const {
    const std::lock_guard<std::mutex> held{guard_};

    return queued_.size();
}

// Posts through a gate: false when the executor behind it is gone or refuses, and then nothing is
// abandoned, since the caller goes its own way. Work the executor took and later throws away unrun
// runs `abandoned` instead (`Errand`). The task is armed under the gate's lock, so an executor
// cannot close and drop it before that.
bool detail::posted(const std::shared_ptr<Gate>& where, std::function<void()> what, std::function<void()> abandoned) {
    const std::lock_guard<std::mutex> held{where->guard};

    if (where->executor == nullptr) return false;

    // Made under the lock, so that this reference goes before the lock does: an executor drops its
    // queue only after closing the gate, so while the gate is held the last copy can only be one
    // that ran or one that was refused, and no abandonment runs on the posting thread.
    const auto errand = std::make_shared<Errand>(std::move(what), std::move(abandoned));

    if (!where->executor->post([errand] { errand->run(); })) return false;

    errand->arm();

    return true;
}

}
