#include "talcite/focusholder.hpp"

#include "talcite/root.hpp"

namespace talcite::ui {

// A node that holds the focus and nothing else. A non-focusable container already exists: it is a
// plain `Node`, which passes its only child through, returns that child's wish and paints nothing
// of its own. `FocusHolder` adds only the focus, which is why it is focusable by default; off by
// default, it would not differ from what we already have. It takes the room its child wants, as
// any node with one child; a holder that is to cover a part of the window however small its content
// is stands in a `Box`, whose job sizing is.
//
// The reason it has to exist at all is single and not obvious: a key bubbles up from the focus.
// The key bar and the menu bar are ancestors of the content, so as long as there is no focus
// anywhere, `Root::dispatch` hands the key to the root and the journey ends there, so the F-keys
// stay silent. A window without any button and without a text field must therefore have something
// that holds the focus. Demos share this one widget for that purpose.

// Takes the focus on a click that no child wanted. A container does not take clicks from its
// children: the mouse travels from the deepest node under the cursor upward, so a child comes
// first, and whoever takes the click ends the journey; only what nobody below wanted arrives here.
// Then absorbing the click is part of taking the focus: since we take it, we are the answer to
// this click.
//
// A holder that is not focusable (disabled) returns false and is transparent for the mouse too:
// the click goes further up, as if it were not there.
//
// The guard on the root is the same as in `Field`: `Node::focus()` throws on a node outside a
// tree, and a widget is sometimes rendered in a test without a root.
bool FocusHolder::handle(ClickEvent&) {
    if (!enabled()) return false;

    if (root() != nullptr) focus();

    return true;
}

}
