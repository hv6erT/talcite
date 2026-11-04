#include "talcite/modals.hpp"

#include <memory>
#include <utility>
#include <vector>

#include "talcite/modal.hpp"
#include "talcite/root.hpp"

#include "slot.hpp"
#include "stops.hpp"

namespace talcite::ui {

// The curtain container is a `Layers` with content underneath and with a modal focus. The content
// is the first child and the curtains the following ones, the last on top; `Stack` paints them,
// and mouse hits follow its order. Opening, closing and remembering the focus are shared with
// windows in `Layers`; what is left here is how a curtain differs from a window: the focus goes
// into it on opening, a focus request from beneath it waits until it closes, and after closing
// the focus returns where it came from.
//
// It stands outermost of all three frames. In Midnight Commander a window covers both the key bar
// and the menu bar, so it has to be their ancestor. The nesting order is not a matter of taste: it
// follows from what has to lie on top of what, and from the path a key takes, because the curtain
// swallows it before it reaches the key bar.
Modals::Modals() : body_(std::make_shared<Row>()) { children().add(body_); }

// The content underneath, or `nullptr` when there is none. The slot is described in `slot.hpp`.
Node* Modals::content() const { return held(*body_); }

// Puts the given node in the content slot, replacing what was there.
void Modals::content(std::shared_ptr<Node> given) { hold(*body_, std::move(given)); }

// From outside it accepts exactly one child, the content, and every open curtain opens exactly one
// more place, which it takes itself.
int Modals::capacity() const { return 1 + static_cast<int>(many()); }

// The topmost curtain, or `nullptr`. Everything the container takes in besides the content is a
// curtain, so the cast is safe.
Modal* Modals::current() const { return static_cast<Modal*>(top()); }

// Opens a curtain on top and moves the focus into it.
void Modals::open(std::shared_ptr<Modal> given) {
    Modal* one = given.get();

    adopt(std::move(given));

    // The focus from before opening is remembered by the curtain itself, not by the container.
    // With a stack of curtains each remembers its own, so closing the top one gives the focus to
    // the one beneath, and closing the last one to the application. A single field in the
    // container would have to choose which of them to remember.
    if (Root* top = root()) {
        remember(*one, top->focused_node());

        // The focus goes deep into the curtain, to the first focusable stop. When there is
        // nothing focusable inside, it stays on the curtain itself, which then swallows the keys.
        // Only on opening: a focus put on the curtain later (say, by handing it to the nearest
        // focusable ancestor) stays on it, so that Enter does not press something nobody chose.
        one->focus();

        if (const std::vector<Node*> these = stops(*one); !these.empty()) these.front()->focus();
    }
}

// Whether a focus request from the given node is held back. The curtain is modal for the focus
// too. A node beneath the top curtain, in the content or in a lower curtain, may ask for the focus
// at any moment: a preview sheet does so when its file arrives from the pool, even though an error
// message is already hanging over it. If the request were granted, keys would go under a window
// that could then be closed neither with Enter nor with Escape, because they would not reach it.
//
// So the request is not lost but becomes "return there after closing": it is recorded as the
// remembered focus of the top curtain, and `Root` does not move the focus. The covered node gets
// no event in the process; a flicker of `FocusEvent` and `UnfocusEvent` would switch the active
// panel, because panels track it exactly that way.
//
// We ask about the top curtain of this container, not about the requester's curtain ancestor: the
// top one goes first and gives the focus back.
bool Modals::withheld(Node& one) {
    Modal* top = current();

    if (top == nullptr) return false;

    if (one.within(*top)) return false;

    remember(*top, &one);

    return true;
}

// Gives the focus back after a curtain is removed, and only then: before that `Root::losing` would
// clear it anyway, because it clears on every removed node and everything beneath it. When the
// focus still stands somewhere (in a curtain opened from a handler, or in the top one when a lower
// one was removed), it stays there.
void Modals::restore(std::weak_ptr<Node> was) {
    if (Root* top = root(); top != nullptr && top->focused_node() != nullptr) return;

    const std::shared_ptr<Node> back = was.lock();

    if (!back || back->root() == nullptr) return;

    // The remembered node may have stopped taking the focus in the meantime, most simply because
    // the choice whose list is just closing was disabled. The focus then goes to the nearest
    // focusable ancestor, by the same rule as on disabling. Otherwise it would be nowhere in the
    // dialog, and a dialog swallows keys and hands out hotkeys only with the focus inside it.
    for (Node* at = back.get(); at != nullptr; at = at->parent())
        if (at->focusable()) {
            at->focus();

            return;
        }
}

}
