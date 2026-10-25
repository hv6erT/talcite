#include "talcite/window.hpp"

#include <algorithm>
#include <optional>

#include "talcite/event.hpp"
#include "talcite/root.hpp"
#include "talcite/windows.hpp"

namespace talcite::ui {

// A modeless window: what mcedit keeps open files in. It is a layer, like a modal
// (`Layer`: closing, the current one, focus memory), but in what matters it is the opposite: there
// is one modal, it swallows keys and the mouse and covers everything below it; there are several
// windows at once, none swallows anything, and the one on top is only the current one, the one
// holding the focus. Hence two pairs of classes on a common base rather than one with a switch:
// a modal is all modality, and there is no modality here.
//
// A window draws no frame and no title, just like a modal: the frame is `Frame`, which the caller
// puts inside. A full window (`full`) is simply one whose frame the caller switches off; `Frame`
// has separate edges for that. A second frame drawing in the library would be a second truth
// about how a box looks.

Window::Window() = default;

// One child: the content. The same as a modal.
int Window::capacity() const { return 1; }

// A full window takes the whole room; a placed one takes what it was given, not what the content
// wants, because the size of a window is the window's state, which the user changes (Resize in
// the menu), not a wish of the content at every frame.
Size Window::wants(Size room) const { return full_ ? room : seat_.size; }

// Switches the window between full and placed; the seat is kept for when it is placed again.
void Window::full(bool given) { full_ = given; }

// Places the window at `where` with `size` (negative dimensions become zero), which makes it not
// full.
void Window::seat(Spot where, Size size) {
    seat_ = {where, {std::max(size.wide, 0), std::max(size.high, 0)}};
    full_ = false;
}

namespace {

// The window's room is the area of its container from the last frame. A window without a parent,
// or not drawn yet, has no room, and then there is nothing to compute the limits against.
[[nodiscard]] std::optional<Size> roomed(const Window& one) {
    const Node* above = one.parent();

    if (above == nullptr) return std::nullopt;

    const Size room = above->area().size;

    if (room.wide <= 0 || room.high <= 0) return std::nullopt;

    return room;
}

// One step along one axis. The limit must not be crossed, but a window that already stands beyond
// it does not jump to it; it can only come back. This is how mcedit computes it: the condition asks
// about where the window stands now and does not clamp the result. After the terminal narrows, a
// window beyond the edge stays where it was and can be brought back step by step.
[[nodiscard]] int stepped(int from, int by, int low, int high) {
    const int to = from + by;

    if (by < 0) return std::max(to, std::min(from, low));

    return std::min(to, std::max(from, high));
}

}

// Moves a placed window, like WindowMove in mcedit: the top row stays in the room, and at least one
// column stays visible on the right. In mcedit the leftward condition compares the position with
// the width of the room instead of the window, so a narrow window can vanish past the left edge;
// we take the obvious intent: one column is always visible, whichever side the window leaves by.
// A full window has nothing to move.
void Window::move(int across, int down) {
    const std::optional<Size> room = roomed(*this);

    if (full_ || !room) return;

    seat_.at.x = stepped(seat_.at.x, across, 1 - seat_.size.wide, room->wide - 1);
    seat_.at.y = stepped(seat_.at.y, down, 0, room->high - 1);
}

// Resizes the window, like WindowResize: no smaller than `least`, and it grows only up to the
// right and bottom edges of the room. The top left corner stays put.
void Window::resize(int across, int down) {
    const std::optional<Size> room = roomed(*this);

    if (full_ || !room) return;

    seat_.size.wide = stepped(seat_.size.wide, across, least_.wide, room->wide - seat_.at.x);
    seat_.size.high = stepped(seat_.size.high, down, least_.high, room->high - seat_.at.y);
}

// Sets the smallest size. It has at least one cell; a smaller window would leave nothing to grab.
void Window::least(Size given) { least_ = {std::max(given.wide, 1), std::max(given.high, 1)}; }

// Brings the window to the top, together with the focus it remembers. Outside a `Windows`
// container it means nothing, because "on top" is a property of the list, not of the node.
void Window::raise() {
    if (auto* window = dynamic_cast<Windows*>(parent())) window->raise(*this);
}

// Draws the content over a transparent background; the window itself draws nothing.
View Window::paint(Size given, Area seen) {
    View made{seen.size};

    made.clear({{0, 0}, seen.size});

    if (children().empty()) return made;

    made.draw(children().front()->render({{0, 0}, given}, seen), {0, 0});

    return made;
}

// A click on the window raises it but does not consume it: a modeless window must not eat the
// mouse, and someone who clicked on the text below wants both to raise the window and to put the
// cursor there. Only what the content did not take arrives here; a click on the content itself
// will need revisiting once overlapping windows arrive.
bool Window::handle(ClickEvent&) {
    raise();

    return false;
}

// Dragging with the mouse, as in mcedit: a press on the top row of a placed window grabs the
// position, a press in the bottom right corner grabs the size. A press raises the window, also
// outside those places, because in mcedit the press already selects the window. Only what the
// content did not take arrives here; with a frame, the top row and the corner belong to the frame,
// and it does not take the mouse.
//
// After the grab the window asks the root to capture the mouse, so that motion reaches it
// wherever the cursor is. Motion is computed from the window's state, not from the last motion:
// the position is the cursor minus the grab point, the size is the cursor minus the top left
// corner, plus one. So `move` and `resize` apply the same limits as with the arrow keys, and when
// the cursor comes back from beyond a limit, the window follows exactly to where the cursor holds
// it. Release ends the drag.
bool Window::handle(MouseEvent& what) {
    if (what.press() == Press::up) {
        const bool was = drag_ != Drag::none;

        drag_ = Drag::none;

        return was;
    }

    if (what.press() == Press::move) {
        if (drag_ == Drag::none) return false;

        // A grabbed window stands in a drawn container: without one it would not have received
        // the press, and once it leaves the container, the capture skips it and motion does not
        // arrive here.
        const Area room = parent()->screen_area().value_or(Area{});
        const Spot at{what.where().x - room.at.x, what.where().y - room.at.y};

        if (drag_ == Drag::move)
            move(at.x - grip_.x - seat_.at.x, at.y - grip_.y - seat_.at.y);
        else
            resize(at.x - seat_.at.x + 1 - seat_.size.wide, at.y - seat_.at.y + 1 - seat_.size.high);

        return true;
    }

    drag_ = Drag::none;

    if (what.button() != Mouse::left) return false;

    raise();

    const std::optional<Area> here = screen_area();

    if (full_ || !here) return false;

    const Spot at{what.where().x - here->at.x, what.where().y - here->at.y};

    if (at.y == 0)
        drag_ = Drag::move;
    else if (at.x == here->size.wide - 1 && at.y == here->size.high - 1)
        drag_ = Drag::resize;
    else
        return false;

    grip_ = at;

    if (Root* top = root(); top != nullptr) top->capture(*this);

    return true;
}

}
