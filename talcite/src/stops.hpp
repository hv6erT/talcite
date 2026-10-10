#pragma once

#include <memory>
#include <vector>

#include "talcite/node.hpp"

namespace talcite::ui {

// The stops of focus traversal: every focusable node in depth, in layout order, appended to
// `into`. Tab in a modal walks exactly over them, and a non-modal window takes the first one when
// it remembers nothing. One concept, shared by the modal and the window container.
//
// A focusable widget is a leaf: it decides about its own inside. Without this rule, Tab in a
// window with a list would walk over each of its rows separately, because a row is focusable too.
// The rule does not apply to `from` itself, because collecting starts at its children and nobody
// here asks whether it is focusable: a modal is focusable, and its stops lie inside it anyway.
inline void stops(const Node& from, std::vector<Node*>& into) {
    for (const std::shared_ptr<Node>& one : from.children()) {
        if (one->focusable()) {
            into.push_back(one.get());

            continue;
        }

        stops(*one, into);
    }
}

// The same stops, returned as a new list.
[[nodiscard]] inline std::vector<Node*> stops(const Node& from) {
    std::vector<Node*> made;

    stops(from, made);

    return made;
}

}
