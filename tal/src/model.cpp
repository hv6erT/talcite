#include "tal/model.hpp"

#include <algorithm>

namespace tal {

// A model is told about by the hooks of its views; nothing to let go of but them.
Model::~Model() = default;

// Tells every view what changed, in the order they started listening: all of them hear it, since
// each shows the data in its own way. A copy of the handlers, so that a handler may add or remove
// handlers meanwhile; one whose hook went meanwhile is skipped.
void Model::told(Changes what) {
    const std::vector<std::shared_ptr<detail::Slot>> called = slots_;

    for (const std::shared_ptr<detail::Slot>& slot : called)
        if (slot->live) slot->called(what);
}

// Keeps the handler `slot`, as an action keeps its handlers; the hook returned removes it.
Hook Model::hooked(std::shared_ptr<detail::Slot> slot) {
    std::erase_if(slots_, [](const std::shared_ptr<detail::Slot>& one) { return !one->live; });

    std::weak_ptr<detail::Slot> held = slot;

    slots_.push_back(std::move(slot));

    return Hook{std::move(held)};
}

}
