#include "talcite/layer.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "talcite/event.hpp"
#include "talcite/root.hpp"

namespace talcite::ui {

// A layer is a node that lies in a layer container, can be closed, and remembers whom to give
// the focus back to. That much is shared by the modal curtain (`Modal`) and the modeless window
// (`Window`), so it lives here once. What sets them apart, modality and therefore where the focus
// returns, is decided by the container in `restore`.

// Closes the layer. Closing goes through the container, because the container removes the layer
// and hands the focus back. A layer outside a container has nothing to close.
void Layer::close() {
    if (auto* up = dynamic_cast<Layers*>(parent())) up->close(*this);
}

// Whether this layer is the topmost one in its container.
bool Layer::current() const {
    const auto* up = dynamic_cast<const Layers*>(parent());

    return up != nullptr && up->top() == this;
}

// Announces the closing with a `CloseEvent`, on the layer itself and before it leaves the tree.
// Once removed it would have no parent, so the event would have nowhere to bubble, and the
// caller's handler is attached to this very layer. Esc has no button, so without this
// announcement "closed without an answer" could not be heard at all.
void Layer::closing() {
    (void)trigger(CloseEvent{});
}

// Takes in a new layer. It goes on the list before it is attached, because the curtain container
// counts from that list how many children it accepts. A null layer throws std::logic_error.
void Layers::adopt(std::shared_ptr<Layer> given) {
    if (!given) throw std::logic_error("talcite::ui::Layers: nothing to open");

    opened_.push_back(given);

    try {
        children().add(std::move(given));
    } catch (...) {
        opened_.pop_back();

        throw;
    }
}

// The topmost layer, or `nullptr` when none is open. It is the last layer in the order of the
// children, that is, of stacking, not of opening. For windows the two differ, because raising a
// window reorders the children but leaves the list of openings alone. The content of the curtain
// container is not a layer and lies underneath.
Layer* Layers::top() const {
    for (std::size_t at = children().many(); at > 0; --at)
        if (auto* one = dynamic_cast<Layer*>(children()[at - 1].get())) return one;

    return nullptr;
}

// Records on the layer the node that had the focus before it opened, without keeping it alive.
void Layers::remember(Layer& one, Node* focus) {
    one.was_ = focus != nullptr ? focus->weak_from_this() : std::weak_ptr<Node>{};
}

// The node remembered by `remember`, or nothing when it is gone.
std::shared_ptr<Node> Layers::remembered(const Layer& one) { return one.was_.lock(); }

// Closes the topmost layer, if there is one.
void Layers::close() {
    if (Layer* one = top()) close(*one);
}

// Closes the given layer: announcement, removal, focus, in that order. We hold our own reference
// throughout, because removal from the tree and from the list may drop the last one, and the
// caller's handler may still touch the layer. The close handler may also open a new layer (an
// error message after OK) or close another one; the list may have shifted, so the position is
// looked up again, and a layer that is already gone is not removed a second time.
void Layers::close(Layer& one) {
    const auto at = std::find_if(opened_.begin(), opened_.end(),
                                 [&one](const std::shared_ptr<Layer>& mine) { return mine.get() == &one; });

    if (at == opened_.end()) return;

    const std::shared_ptr<Layer> kept = *at;

    kept->closing();

    const auto again = std::find(opened_.begin(), opened_.end(), kept);

    if (again == opened_.end()) return;

    const std::weak_ptr<Node> was = kept->was_;

    // A layer opened from the close handler remembered a focus that is about to leave the tree
    // together with us, so it inherits our memory, and the focus has somewhere to return to after
    // it closes.
    const auto inside = [&kept](const Node* node) {
        for (; node != nullptr; node = node->parent())
            if (node == kept.get()) return true;

        return false;
    };

    for (auto above = std::next(again); above != opened_.end(); ++above) {
        const std::shared_ptr<Node> mine = (*above)->was_.lock();

        if (!mine || mine->root() == nullptr || inside(mine.get())) (*above)->was_ = was;
    }

    opened_.erase(again);

    (void)children().remove(kept.get());

    restore(was);
}

}
