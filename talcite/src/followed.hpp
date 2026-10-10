#pragma once

#include <algorithm>

#include "talcite/area.hpp"
#include "talcite/node.hpp"
#include "talcite/root.hpp"

namespace talcite::ui {

// A scroll following the focus. `Scroll` does this on every open axis: after laying out its
// children, it asks the focused node for its rectangle within the content (`Node::area_in`) and
// shifts the scroll to it. An internal header, like `spool.hpp`: the library API does not need it.

// The most times one frame may shift and redraw: each repeat lays out one level deeper, and no
// tree here is built deeper than this.
inline constexpr int passes = 8;

// The node focused in the root, or null when the scroll is not in a tree at all.
[[nodiscard]] inline const Node* beheld(const Node& scroll) {
    const Root* top = scroll.root();

    return top != nullptr ? top->focused_node() : nullptr;
}

// The scroll offset at which the segment [at, at + much) of the content fits in a window of
// length `window`, moved to the nearer edge; when the segment is longer than the window, its start
// wins. One axis at a time. The clamp at the end is a guard, not a rule: for a node lying inside
// the content the result fits in [0, whole - window] anyway, but the rectangle of a node in a
// container that this frame did not lay out comes from the previous frame and may stick out past
// the content, and scrolling past the end would show emptiness until the next frame.
[[nodiscard]] inline int followed(int shift, int at, int much, int window, int whole) {
    int want = shift;

    if (at + much > want + window) want = at + much - window;
    if (at < want) want = at;

    return std::clamp(want, 0, std::max(whole - window, 0));
}

}
