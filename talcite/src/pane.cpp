#include "talcite/pane.hpp"

#include "talcite/root.hpp"

namespace talcite::ui {

// A non-focusable container already exists: it is a plain `Node`, which passes its only child
// through, returns that child's wish and paints nothing of its own. All that `Pane` adds is focus
// and filling the space, which is why both are on by default. If they were off by default, the
// element would not differ from what we already have.
//
// The reason it has to exist at all is single and not obvious: a key bubbles up from the focus.
// The legend and the menu bar are ancestors of the content, so as long as there is no focus
// anywhere, `Root::dispatch` hands the key to the root and the journey ends there, so the F-keys
// stay silent. A window without any button and without a text field must therefore have something
// that holds the focus. Demos share this one widget for that purpose.

// Takes the whole room and only that. A panel or a preview has to occupy its part of the window
// however large its content is, and that is why `Pane` overrides this; without it `Node::wants`
// would descend to the child's wish. This is a constant, not a setting: setting a size is the job
// of `Box`, and `Pane` stays with what it exists for, the focus.
Size Pane::wants(Size room) const { return room; }

// Takes the focus on a click that no child wanted. A container does not take clicks from its
// children: the mouse travels from the deepest node under the cursor upward, so a child comes
// first, and whoever takes the click ends the journey; only what nobody below wanted arrives here.
// Then absorbing the click is part of taking the focus: since we take it, we are the answer to
// this click.
//
// A container that is not focusable (disabled) returns false and is transparent for the mouse too:
// the click goes further up, as if the container were not there.
//
// The guard on the root is the same as in `Field`: `Node::focus()` throws on a node outside a
// tree, and a widget is sometimes rendered in a test without a root.
bool Pane::handle(ClickEvent&) {
    if (!enabled()) return false;

    if (root() != nullptr) focus();

    return true;
}

}
