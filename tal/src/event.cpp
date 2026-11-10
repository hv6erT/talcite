#include "tal/event.hpp"

#include <utility>

namespace tal {

// The handler goes with its hook.
Hook::~Hook() { released(); }

// Takes over the handler of `other`, which is left empty.
Hook::Hook(Hook&& other) noexcept : slot_(std::move(other.slot_)) { other.slot_.reset(); }

// Lets go of the handler held and takes over the one of `other`.
Hook& Hook::operator=(Hook&& other) noexcept {
    if (this == &other) return *this;

    released();

    slot_ = std::move(other.slot_);
    other.slot_.reset();

    return *this;
}

// Stops the handler from being called, also in the middle of a delivery; its node drops it later.
// A handler whose node is gone is gone already.
void Hook::released() {
    if (const std::shared_ptr<detail::Slot> slot = slot_.lock()) slot->live = false;

    slot_.reset();
}

// Keeps `one` until the hooks go.
void Hooks::add(Hook one) { held_.push_back(std::move(one)); }

}
