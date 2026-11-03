#include "talcite/modal.hpp"

#include <algorithm>
#include <vector>

#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "stops.hpp"

namespace talcite::ui {

namespace {

// The stop the focus is at. The focus may have gone deeper than the traversal reaches: a list
// hands it to its current item. The stop is then the ancestor that is on the list of stops. A node
// outside that list gives nothing, the same as no focus, and a step forward then starts from the
// beginning.
Node* landed(const std::vector<Node*>& these, Node* one) {
    for (Node* at = one; at != nullptr; at = at->parent())
        if (std::find(these.begin(), these.end(), at) != these.end()) return at;

    return nullptr;
}

}

Modal::Modal(ModalShape shape) : shape_(shape) {}

// The curtain is the whole screen, although it draws only the box on it. All the modality comes
// from this: `Root::under` picks by rectangle and layer, so a click beside the box hits the
// curtain, because there is no deeper child there. No lock in the root is needed for it.
Size Modal::wants(Size room) const { return room; }

// One child: the box. The border and the title are drawn by the `Frame` the application puts in;
// drawing a border again in another place of the tree would be a second truth about how the box
// looks.
int Modal::capacity() const { return 1; }

// The curtain takes the focus itself when there is nothing inside to take it, and then swallows
// the keys itself instead of leaving them to the application underneath.
bool Modal::focusable() const { return true; }

// Where the box will stand, and this is the only thing in which curtains differ from one another.
// By default in the middle, because that is where a dialog stands.
//
// The room for the shadow is subtracted before the position is computed, so that the shadow does
// not go off the screen and the box stands in the middle of what is really left. This part is
// shared by all curtains, because it concerns the shadow, not who stands where, so it sits here
// and not in those that compute the position differently.
Area Modal::seat(Size given) const {
    if (children().empty()) return {};

    const int aside = shape_.shade ? 2 : 0;
    const int apart = shape_.shade ? 1 : 0;

    const Size room{std::max(given.wide - aside, 0), std::max(given.high - apart, 0)};

    Size wants = children().front()->wants(room);

    wants.wide = std::min(wants.wide, room.wide);
    wants.high = std::min(wants.high, room.high);

    return {{std::max((room.wide - wants.wide) / 2, 0), std::max((room.high - wants.high) / 2, 0)},
            wants};
}

// Lays the box out at its seat; the rest of the curtain holds nothing. The shadow falls outside
// the box, so a box that moved or changed size changes the curtain, not only the box.
void Modal::layout(Size given) {
    if (children().empty()) return;

    Node& box = *children().front();

    const Area was = box.area();

    box.arrange(seat(given));

    if (shape_.shade && box.area() != was) changed();
}

// Paints the box at its seat with its shadow; the rest of the curtain is transparent, so a
// screen-sized curtain does not paint over what lies beneath it.
//
// The shadow falls to the right and down, offset by one row and two columns, the same rule as
// under a menu, because in Midnight Commander it is the same shadow. The letters underneath stay
// and are only dimmed.
void Modal::paint(Canvas& onto) const {
    if (children().empty()) return;

    Node& box = *children().front();

    box.render(onto);

    const Area& where = box.area();

    if (!shape_.shade || where.size.wide <= 0 || where.size.high <= 0) return;

    const int last_x = where.at.x + where.size.wide - 1;
    const int last_y = where.at.y + where.size.high - 1;

    const Style dark = skin().style("talcite.shade");

    onto.shade({{last_x + 1, where.at.y + 1}, {2, where.size.high - 1}}, dark);
    onto.shade({{where.at.x + 2, last_y + 1}, {where.size.wide, 1}}, dark);
}

// Moves the focus by the given number of stops, wrapping around. The traversal boundary follows
// from modality, not from a separate notion. Tab bubbles like any key and is handled by whoever
// moves the focus among their focusable descendants. Since the curtain swallows everything anyway,
// Tab never escapes it and nothing extra has to guard that.
//
// Without a focus inside, a step forward lands on the first focusable stop, a step back on the
// last. Not on the default button: in a window that starts with a text field, the first letter
// typed may be the button's hotkey, and the window would close with an empty field. The curtain
// needs to know nothing about fields or buttons; the order is set by whoever assembles the window.
void Modal::step(int by) {
    const std::vector<Node*> these = stops(*this);

    if (these.empty()) return;

    Root* top = root();

    if (top == nullptr) return;

    const auto at = std::find(these.begin(), these.end(), landed(these, top->focused_node()));

    if (at == these.end()) {
        if (by >= 0)
            these.front()->focus();
        else
            these.back()->focus();

        return;
    }

    const int much = static_cast<int>(these.size());
    const int which = static_cast<int>(at - these.begin());

    these[static_cast<std::size_t>(((which + by) % much + much) % much)]->focus();
}

// Keyboard modality is here. The focus sits in the curtain, so a key nobody took on the way
// arrives here and goes no further. There is nothing new in this: an open menu behaves exactly
// the same.
//
// Closing removes the curtain from the tree in the middle of delivery, but the node survives it
// thanks to `linger` in the root, so returning true after `close()` is allowed here.
//
// The curtain does not act on Escape. Closing the window on Esc would be an action the caller
// could not revoke, for the same reason the dialog does not press its default button by itself.
// Whoever wants "Esc closes" attaches a key handler with `on` to their curtain and calls `close()`
// themselves.
//
// The rest is swallowed, because swallowing does nothing: it is the curtain's whole job and the
// only way modality can be expressed at all. Tab and Shift+Tab move between the stops, and so do
// the down and up arrows, as in the dialogs of Midnight Commander: a widget that uses them (a list,
// a group of options, a history) takes them first and lets them through only at its ends, so an
// arrow reaches the curtain exactly when nothing lower down wants it.
bool Modal::handle(KeyEvent& what) {
    const Code code = what.key().code();

    if (code == Code::tab) {
        step(what.mods().shift ? -1 : 1);

        return true;
    }

    if ((code == Code::down || code == Code::up) && !what.mods().shift && !what.mods().ctrl && !what.mods().alt) {
        step(code == Code::down ? 1 : -1);

        return true;
    }

    return true;
}

// A click beside the box is swallowed and nothing more. Midnight Commander does not close a window
// on a click in the background; Esc or a button closes it. A click on the box itself does not get
// here, because the child takes it.
bool Modal::handle(ClickEvent&) { return true; }

// The raw mouse event is swallowed too, so that drag selection in the panel underneath does not
// work through the window.
bool Modal::handle(MouseEvent&) { return true; }

// The wheel is swallowed too, so the panel underneath does not scroll through the window.
bool Modal::handle(WheelEvent&) { return true; }

}
