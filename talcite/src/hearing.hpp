#pragma once

#include <cstdint>
#include <memory>
#include <mutex>

#include "talcite/promise.hpp"

namespace talcite::net {

// Listens for the stop of the work running on this thread for the length of a wait, waking the
// condition variable `stirring` of `held` when it comes, so that cancelling the work ends the wait
// at once instead of after its whole patience. Outside a work there is no stop and it does
// nothing. The listener runs inside the cancelling call and takes `guard` only to wake the waiter
// without losing the wakeup; it holds `held`, because it may still be running when the wait is
// over. It must be made before `guard` is taken and outlive it, since withdrawing the listener
// waits for the stop's own lock.
class Hearing {
public:
    // Starts listening for the stop of the current work, if there is one.
    template <typename H>
    explicit Hearing(const std::shared_ptr<H>& held) : stop_(Stop::current()) {
        if (stop_ == nullptr) return;

        which_ = detail::stopping(*stop_, [held] {
            {
                const std::lock_guard<std::mutex> kept{held->guard};
            }

            held->stirring.notify_all();
        });
    }

    // Stops listening.
    ~Hearing() {
        if (stop_ != nullptr) detail::Access::core(*stop_)->off_stop(which_);
    }

    Hearing(const Hearing&) = delete;
    Hearing& operator=(const Hearing&) = delete;
    Hearing(Hearing&&) = delete;
    Hearing& operator=(Hearing&&) = delete;

    // Whether the work has been stopped.
    [[nodiscard]] bool rang() const { return stop_ != nullptr && stop_->stopped(); }

private:
    const Stop* stop_ = nullptr;
    std::uint64_t which_ = 0;
};

}
