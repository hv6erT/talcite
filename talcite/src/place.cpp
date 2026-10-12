#include "talcite/place.hpp"

#include <utility>

namespace talcite {

// Every place has a gate through which promises reach it later (`on`). A promise holds the gate, not
// the place, so a place that has gone away refuses what arrives instead of being called dead.
Place::Place() : gate_(std::make_shared<detail::Gate>()) { gate_->place = this; }

// Closes the gate, if the derived place has not done so already.
Place::~Place() { close(); }

// Closes the gate: from now on nothing reaches the place through it. A derived place calls this
// first thing in its destructor, before its own members go, so that no post arrives at a
// half-destroyed object; a post in flight is waited for.
void Place::close() {
    const std::lock_guard<std::mutex> held{gate_->guard};

    gate_->place = nullptr;
}

// Stops taking work before the queue goes.
ManualPlace::~ManualPlace() { close(); }

// Queues the work until `turn`.
bool ManualPlace::post(std::function<void()> what) {
    const std::lock_guard<std::mutex> held{guard_};

    queued_.push_back(std::move(what));

    return true;
}

// Runs what was queued when it was called and returns how much that was; work queued meanwhile waits
// for the next turn.
std::size_t ManualPlace::turn() {
    std::vector<std::function<void()>> taken;

    {
        const std::lock_guard<std::mutex> held{guard_};

        taken.swap(queued_);
    }

    for (const std::function<void()>& one : taken) one();

    return taken.size();
}

// How much work waits for the next turn.
std::size_t ManualPlace::waiting() const {
    const std::lock_guard<std::mutex> held{guard_};

    return queued_.size();
}

// Posts through a gate: false when the place behind it is gone or refuses.
bool detail::posted(const std::shared_ptr<Gate>& where, std::function<void()> what) {
    const std::lock_guard<std::mutex> held{where->guard};

    if (where->place == nullptr) return false;

    return where->place->post(std::move(what));
}

}
