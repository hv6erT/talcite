#include "talcite/windows.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "talcite/root.hpp"
#include "talcite/window.hpp"

#include "stops.hpp"

namespace talcite::ui {

// Modeless windows in one place, like modals in `Modals`: opening, closing and the current
// window are shared in `Layers`. The differences from modals come from there being no modality
// here:
//
// - the stacking order is not the opening order. Stacking says what lies on what, and every raise
//   changes it; the list of layers keeps the opening order, and `next` and `previous` walk that,
//   otherwise "next window" would jump through the history of looking rather than through the
//   files, and two presses would come back to the same place;
// - the focus is remembered by the window, not by the container. Coming back to a window puts the
//   cursor where it was, not on the first focusable node: a file one was moving through should
//   stay where it was left.

namespace {

// Whether `one` is `under` or lies somewhere below it.
[[nodiscard]] bool inside(const Node* one, const Node& under) {
    for (; one != nullptr; one = one->parent())
        if (one == &under) return true;

    return false;
}

// The neighbor of the current window in opening order, wrapping around. With one window or none
// there is no neighbor.
[[nodiscard]] Window* beside(const std::vector<std::shared_ptr<Layer>>& all, const Window* now, int by) {
    if (all.size() < 2) return nullptr;

    const auto at = std::find_if(all.begin(), all.end(),
                                 [now](const std::shared_ptr<Layer>& mine) { return mine.get() == now; });

    if (at == all.end()) return nullptr;

    const auto much = static_cast<std::ptrdiff_t>(all.size());
    const std::ptrdiff_t which = ((at - all.begin()) + by % much + much) % much;

    return static_cast<Window*>(all[static_cast<std::size_t>(which)].get());
}

}

// Where a window stands in the room: all of it when full, otherwise its own seat. The seat is the
// window's state, which the user changes (Move and Resize), and the container reads it; the window
// does not place itself.
Area Windows::placement(const Node& one, Size room) const {
    const auto* window = dynamic_cast<const Window*>(&one);

    if (window == nullptr || window->full()) return {{0, 0}, room};

    return window->seat();
}

// Adds a window and brings it to the top with the focus.
void Windows::open(std::shared_ptr<Window> given) {
    Window* one = given.get();

    adopt(std::move(given));

    raise(*one);
}

// The window on top. Everything the container holds is a window (only `open` accepts them), so
// the cast is safe.
Window* Windows::current() const { return static_cast<Window*>(top()); }

// Raising is moving the window to the end of the children; the layering is their order and
// nothing more. The focus is remembered in the window it sits in, before it leaves the tree
// together with the node being moved; then it goes to what the raised window remembers, and when
// that remembers nothing (or that node has left), to the first focusable node inside.
//
// The window is found by the focus, not as the current one: on `open` the new window already
// stands on top before it gets here, so "current" would be the new window itself and the memory
// of the previous one would be lost.
void Windows::raise(Window& one) {
    const auto at = std::find_if(opened().begin(), opened().end(),
                                 [&one](const std::shared_ptr<Layer>& mine) { return mine.get() == &one; });

    if (at == opened().end()) throw std::invalid_argument("talcite::ui::Windows: not mine");

    if (Root* top = root(); top != nullptr)
        if (Node* was = top->focused_node(); was != nullptr)
            for (const std::shared_ptr<Layer>& mine : opened())
                if (inside(was, *mine)) remember(*mine, was);

    const std::shared_ptr<Layer> kept = *at;

    if (current() != &one) {
        (void)children().remove(&one);
        children().add(kept);
    }

    if (root() == nullptr) return;

    if (const std::shared_ptr<Node> back = remembered(one); back && back->root() != nullptr) {
        back->focus();

        return;
    }

    const std::vector<Node*> these = stops(one);

    if (!these.empty()) these.front()->focus();
}

// After closing, the focus goes to the new top window, with its own memory, as on every raise.
// The memory of the closed one has no say here: with modeless windows the focus returns to a
// window, not to whoever had it before the closed one opened.
void Windows::restore(std::weak_ptr<Node>) {
    if (Window* now = current(); now != nullptr) raise(*now);
}

// Raises the next window in opening order, wrapping around, like Window > Next in mcedit.
void Windows::next() {
    if (Window* one = beside(opened(), current(), 1)) raise(*one);
}

// Raises the previous window in opening order, wrapping around.
void Windows::previous() {
    if (Window* one = beside(opened(), current(), -1)) raise(*one);
}

}
