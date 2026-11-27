#include "tal/event.hpp"

#include <algorithm>
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

// Keeps the handler `slot` for events of the kind `type`, the handlers whose hooks went dropped
// first; the hook returned removes it.
Hook Bell::hooked(std::type_index type, std::shared_ptr<detail::Slot> slot) {
    std::erase_if(slots_, [](const auto& one) { return !one.second->live; });

    std::weak_ptr<detail::Slot> held = slot;

    slots_.emplace_back(type, std::move(slot));

    return Hook{std::move(held)};
}

// Calls the live handlers for an event of the kind of `one`, in the order they came, until one
// takes it. A copy of them, so that a handler may add or remove handlers meanwhile; one whose hook
// went meanwhile is skipped.
void Bell::ring(Event& one) const {
    const std::type_index type{typeid(one)};
    std::vector<std::shared_ptr<detail::Slot>> called;

    for (const auto& [kind, slot] : slots_)
        if (kind == type) called.push_back(slot);

    for (const std::shared_ptr<detail::Slot>& slot : called) {
        if (!slot->live) continue;

        slot->called(one);

        if (one.taken()) return;
    }
}

// Hears that the carrying of this load ended, `taken` when a node took it (`Dropped`), else when it
// was dropped where nothing takes it or given up (Esc, a press). Nothing by default: for a load
// whose maker wants to know.
void Carried::landed(bool taken) { (void)taken; }

}
