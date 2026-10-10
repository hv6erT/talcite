#pragma once

#include <memory>
#include <utility>

#include "talcite/node.hpp"

namespace talcite::ui {

// A slot for the application's content. Modals, the legend and the menu bar place a row beneath
// them, into which the application puts what is to stand under them. The content is a single node
// and is set directly with `content`, so the caller does not reach through someone else's node into
// its children. The row stays inside as the slot, because it is what gives the content the whole
// space. This returns the content held in the slot, or null when it is empty.
[[nodiscard]] inline Node* held(const Node& slot) {
    return slot.children().empty() ? nullptr : slot.children().front().get();
}

// New content replaces the old, and a null pointer empties the slot. The same content a second
// time does nothing: taking it out and putting it back would take away its focus.
inline void hold(Node& slot, std::shared_ptr<Node> given) {
    if (given.get() == held(slot)) return;

    slot.children().clear();

    if (given != nullptr) slot.children().add(std::move(given));
}

}
