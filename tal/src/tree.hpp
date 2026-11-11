#pragma once

#include "tal/node.hpp"

namespace tal::tree {

[[nodiscard]] inline bool under(const Node& outer, const Node& one) {
    for (const Node* at = &one; at != nullptr; at = at->parent())
        if (at == &outer) return true;

    return false;
}

[[nodiscard]] inline Node* taking(Node& root) {
    if (root.focusable()) return &root;

    for (Node& one : root.children())
        if (Node* found = taking(one)) return found;

    return nullptr;
}

}
