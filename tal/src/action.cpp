#include "tal/action.hpp"

#include <algorithm>

namespace tal {

// An action shown as `text`, with `letter` marked in a menu and `key` as its shortcut; enabled.
Action::Action(std::string text, std::optional<char32_t> letter, std::optional<Key> key)
    : text_(std::move(text)), letter_(letter), key_(key) {}

// Sets the text the views show; they hear `Altered` when it changes.
void Action::text(std::string said) {
    if (said == text_) return;

    text_ = std::move(said);

    altered();
}

// Sets the letter marked in a menu or on a button, or none.
void Action::letter(std::optional<char32_t> one) {
    if (one == letter_) return;

    letter_ = one;

    altered();
}

// Sets the shortcut, or none.
void Action::key(std::optional<Key> one) {
    if (one == key_) return;

    key_ = one;

    altered();
}

// Enables or disables the action; a disabled one does nothing when run, and its views are disabled.
void Action::enable(bool on) {
    if (on == enabled_) return;

    enabled_ = on;

    altered();
}

// Runs the action: `Run` goes to its handlers in the order they were added, until one takes it.
// A disabled action does nothing.
void Action::run() {
    if (!enabled_) return;

    Run one{*this};

    told(one);
}

// Keeps the handler `slot` for events of the kind `type`, as a node keeps its handlers; the hook
// returned removes it.
Hook Action::hooked(std::type_index type, std::shared_ptr<detail::Slot> slot) {
    std::erase_if(slots_, [](const auto& one) { return !one.second->live; });

    std::weak_ptr<detail::Slot> held = slot;

    slots_.emplace_back(type, std::move(slot));

    return Hook{std::move(held)};
}

// Calls the live handlers for an event of the kind of `one`, in order, until one takes it. A copy
// of them, so that a handler may add or remove handlers meanwhile; one whose hook went meanwhile is
// skipped.
void Action::told(Event& one) {
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

// Tells the views that something they show changed.
void Action::altered() {
    Altered one{*this};

    told(one);
}

}
