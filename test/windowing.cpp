#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/encoding.hpp"
#include "talcite/event.hpp"
#include "talcite/frame.hpp"
#include "talcite/root.hpp"
#include "talcite/row.hpp"
#include "talcite/window.hpp"
#include "talcite/windows.hpp"

using namespace talcite;

namespace {

// A box that takes focus and knows it was clicked, which is enough to check layers, focus and the
// mouse.
class Box : public ui::Node {
public:
    Box(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    // Always accepts focus.
    [[nodiscard]] bool focusable() const override { return true; }

    // Wants the size it was built with, whatever the room.
    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

    int hit = 0;

protected:
    // Fills the given size with the box's character.
    View paint(ui::Size given, ui::Area seen) override {
        View made{seen.size};

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                made.put({x - seen.at.x, y - seen.at.y}, what_, Style{});

        return made;
    }

    // Counts the click and consumes it.
    bool handle(ui::ClickEvent&) override {
        ++hit;

        return true;
    }

private:
    ui::Size wants_;
    char32_t what_ = U'?';
};

// The smallest surroundings needed: a root with windows inside.
struct Stage {
    ui::Root top;
    std::shared_ptr<ui::Windows> window = std::make_shared<ui::Windows>();

    // Hangs the window container on the root.
    Stage() { top.children().add(window); }

    // Opens a full window holding a 10x3 box of `what` and returns it.
    std::shared_ptr<ui::Window> opened(char32_t what) {
        const auto one = std::make_shared<ui::Window>();

        one->children().add(std::make_shared<Box>(ui::Size{10, 3}, what));
        window->open(one);

        return one;
    }
};

// Reads row `y` of a drawn view as UTF-8 text, skipping the empty halves of wide cells.
std::string read(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x)
        if (const char32_t letter = one.at({x, y}).letter; letter != 0) made += talcite::spelled(letter);

    return made;
}

// The first child of a window, or null when it has none.
ui::Node* inside(const ui::Window& one) { return one.children().empty() ? nullptr : one.children()[0].get(); }

}

// --- Layer and current window --------------------------------------------------------------------

// A non-modal window covers nothing by itself: a full one takes the whole room, a seated one only
// its rectangle, and the last one opened is current, that is, on top and with focus.
TRIAL("Window: the last one opened is current, and focus goes into it") {
    Stage stage;

    const auto one = stage.opened(U'a');

    ought(one->current());
    ought(stage.window->current() == one.get());
    same(stage.top.focused_node(), inside(*one));
    same(stage.window->many(), std::size_t{1});

    const auto two = stage.opened(U'b');

    ought(two->current());
    ought(!one->current());
    same(stage.top.focused_node(), inside(*two));
    same(stage.window->many(), std::size_t{2});
}

// A full window paints over the whole room; a seated one stands where it was told, and what lies
// below shows from under it.
TRIAL("Window: a full one takes the whole room, a seated one its rectangle") {
    Stage stage;

    const auto under = stage.opened(U'a');
    const auto over = stage.opened(U'b');

    ought(under->full());

    over->seat({2, 1}, {4, 2});

    ought(!over->full());
    same(over->seat().at.x, 2);

    const View made = stage.top.render({{0, 0}, {10, 3}});

    same(read(made, 0), std::string{"aaaaaaaaaa"});
    same(read(made, 1), std::string{"aabbbbaaaa"});
    same(read(made, 2), std::string{"aabbbbaaaa"});

    // Back to full: the whole room again.
    over->full(true);

    same(read(stage.top.render({{0, 0}, {10, 3}}), 0), std::string{"bbbbbbbbbb"});
}

// Raising brings a window to the top and gives focus back to where it was: the file one was walking
// over stays where it was left.
TRIAL("Window: raising comes back with focus where it was") {
    Stage stage;

    const auto one = stage.opened(U'a');

    // Two focusable boxes in the first window; focus goes on the second, so that returning to the
    // first focusable one cannot pass for memory.
    const auto first = std::make_shared<Box>(ui::Size{4, 1}, U'd');
    const auto other = std::make_shared<Box>(ui::Size{4, 1}, U'c');

    one->children().clear();

    const auto both = std::make_shared<ui::Row>();

    both->children().add(first);
    both->children().add(other);
    one->children().add(both);

    other->focus();

    const auto two = stage.opened(U'b');

    same(stage.top.focused_node(), inside(*two));

    one->raise();

    ought(one->current());
    same(stage.top.focused_node(), static_cast<ui::Node*>(other.get()));

    two->raise();

    same(stage.top.focused_node(), inside(*two));
}

// --- Order and closing ---------------------------------------------------------------------------

// `next` and `previous` walk the opening order, not the layers: otherwise "the next window" would
// jump through the viewing history and two presses would return to the same place.
TRIAL("Window: next and previous walk the opening order, round and round") {
    Stage stage;

    const auto one = stage.opened(U'a');
    const auto two = stage.opened(U'b');
    const auto three = stage.opened(U'c');

    ought(three->current());

    stage.window->next();
    ought(one->current());

    stage.window->next();
    ought(two->current());

    stage.window->previous();
    ought(one->current());

    stage.window->previous();
    ought(three->current());

    // Raising by hand does not change the walking order.
    one->raise();
    stage.window->next();
    ought(two->current());
}

// Closing announces itself while the window still stands in the tree, and after it whatever was
// left on top becomes current, together with focus. One window round and round is itself.
TRIAL("Window: closing announces itself and gives focus to the top one") {
    Stage stage;

    const auto one = stage.opened(U'a');
    const auto two = stage.opened(U'b');

    int said = 0;
    bool stood = false;

    // The window is held in the handler by a plain pointer: the handler lives in the window itself,
    // so a strong pointer would keep it alive forever and leak it.
    const ui::Hook heard_close = two->on<ui::CloseEvent>([&said, &stood, window = two.get()](ui::CloseEvent&) {
        ++said;
        stood = window->root() != nullptr;

        return false;
    });

    two->close();

    same(said, 1);
    ought(stood);
    same(stage.window->many(), std::size_t{1});
    ought(one->current());
    same(stage.top.focused_node(), inside(*one));

    // Just one: `next` has nowhere to go.
    stage.window->next();
    ought(one->current());

    one->close();

    same(stage.window->many(), std::size_t{0});
    ought(stage.window->current() == nullptr);
}

// A click on a window raises it but does not swallow it. Only what the content did not take
// arrives here: a click on the frame or the margin raises, and a click on the text itself is
// handled by the text and the window does not know about it. That is why the window returns false:
// if it took the click, a click in the background would raise the window instead of placing the
// cursor.
TRIAL("Window: a click on the frame raises, and the content still gets its own") {
    Stage stage;

    const auto one = std::make_shared<ui::Window>();
    const auto framed = std::make_shared<ui::Frame>();
    const auto box = std::make_shared<Box>(ui::Size{8, 1}, U'a');

    framed->children().add(box);
    one->children().add(framed);
    stage.window->open(one);

    const auto two = stage.opened(U'b');

    two->seat({6, 0}, {4, 3});

    (void)stage.top.render({{0, 0}, {10, 3}});

    ought(two->current());

    // A click on the first window's frame: the frame takes nothing, so it reaches the window.
    ui::ClickEvent edge{{0, 0}, ui::Mouse::left};

    (void)stage.top.dispatch(edge);

    ought(one->current());
    same(box->hit, 0);

    // A click on the content: the content takes it, and the window does not know about it.
    two->raise();

    ui::ClickEvent mine{{1, 1}, ui::Mouse::left};

    (void)stage.top.dispatch(mine);

    same(box->hit, 1);
    ought(two->current());
}

// --- Moving and resizing -------------------------------------------------------------------------

// Moving works like WindowMove in mcedit: the top row stays in the room, and at least one column
// stays on each side. The room is taken from the container's last frame.
TRIAL("Window: moving keeps the top row and one column in the room") {
    Stage stage;

    const auto one = stage.opened(U'a');

    one->seat({2, 1}, {4, 2});

    (void)stage.top.render({{0, 0}, {10, 5}});

    one->move(3, 2);
    same(one->seat().at.x, 5);
    same(one->seat().at.y, 3);

    // Right to the last column, down to the last row, and not one cell further.
    one->move(100, 100);
    same(one->seat().at.x, 9);
    same(one->seat().at.y, 4);

    // Left so far that one column of the window is left; up to the first row.
    one->move(-100, -100);
    same(one->seat().at.x, -3);
    same(one->seat().at.y, 0);

    // The size stays while moving.
    same(one->seat().size.wide, 4);
    same(one->seat().size.high, 2);
}

// Resizing works like WindowResize: the upper left corner stays, the window grows only up to the
// right and bottom edges, and shrinks at most down to `least`.
TRIAL("Window: resizing up to the room's edge and no less than least") {
    Stage stage;

    const auto one = stage.opened(U'a');

    one->seat({2, 1}, {4, 2});
    one->least({3, 2});

    same(one->least().wide, 3);

    (void)stage.top.render({{0, 0}, {10, 5}});

    one->resize(1, 1);
    same(one->seat().size.wide, 5);
    same(one->seat().size.high, 3);

    one->resize(100, 100);
    same(one->seat().size.wide, 8);
    same(one->seat().size.high, 4);

    one->resize(-100, -100);
    same(one->seat().size.wide, 3);
    same(one->seat().size.high, 2);

    // The corner stays.
    same(one->seat().at.x, 2);
    same(one->seat().at.y, 1);

    // The smallest window has at least one cell.
    one->least({0, -4});
    same(one->least().wide, 1);
    same(one->least().high, 1);
}

// A window that already stands beyond the limit, because the terminal narrowed under it, does not
// jump to the limit: a step outward does nothing, and a step inward moves by one cell. That is how
// mcedit counts: the condition asks about the place where the window stands rather than clipping
// the result.
TRIAL("Window: a window beyond the limit does not jump, it comes back step by step") {
    Stage stage;

    const auto one = stage.opened(U'a');

    one->seat({15, 8}, {12, 6});

    (void)stage.top.render({{0, 0}, {10, 5}});

    one->move(1, 1);
    same(one->seat().at.x, 15);
    same(one->seat().at.y, 8);

    one->move(-1, -1);
    same(one->seat().at.x, 14);
    same(one->seat().at.y, 7);

    // Too wide and too tall: it cannot grow, but it can shrink by a step.
    one->resize(1, 1);
    same(one->seat().size.wide, 12);
    same(one->seat().size.high, 6);

    one->resize(-1, -1);
    same(one->seat().size.wide, 11);
    same(one->seat().size.high, 5);

    // The same on the other side: above the top edge and narrower than `least`. It goes no further
    // up or left and does not shrink, but it does not jump to the limit either.
    one->seat({-20, -2}, {2, 1});
    one->least({3, 2});

    one->move(-1, -1);
    same(one->seat().at.x, -20);
    same(one->seat().at.y, -2);

    one->resize(-1, -1);
    same(one->seat().size.wide, 2);
    same(one->seat().size.high, 1);
}

// A full window has nothing to move, and a window without a room (not drawn yet) has nothing to
// compute limits against. Both stay.
TRIAL("Window: full and undrawn windows stay") {
    Stage stage;

    const auto one = stage.opened(U'a');

    one->seat({2, 1}, {4, 2});

    one->move(1, 1);
    one->move(-1, -1);
    one->resize(1, 1);
    one->resize(-1, -1);
    same(one->seat().at.x, 2);
    same(one->seat().at.y, 1);
    same(one->seat().size.wide, 4);
    same(one->seat().size.high, 2);

    (void)stage.top.render({{0, 0}, {10, 5}});

    one->full(true);
    one->move(1, 1);
    one->resize(1, 1);
    same(one->seat().at.x, 2);
    same(one->seat().at.y, 1);
    same(one->seat().size.wide, 4);
    same(one->seat().size.high, 2);

    // A window outside a container stays too.
    ui::Window alone;

    alone.seat({1, 1}, {2, 2});
    alone.move(1, 1);
    alone.resize(1, 1);
    same(alone.seat().at.x, 1);
    same(alone.seat().size.wide, 2);
}

// A full window wants the whole room, a seated one its own place, regardless of the room.
TRIAL("Window: a full one wants the room, a seated one its own size") {
    ui::Window one;

    same(one.wants({10, 5}).wide, 10);
    same(one.wants({10, 5}).high, 5);

    one.seat({1, 1}, {4, 2});

    same(one.wants({10, 5}).wide, 4);
    same(one.wants({10, 5}).high, 2);
}

namespace {

// A node that counts the clicks reaching it, to check that the window does not swallow them.
class Catcher : public ui::Node {
public:
    int hit = 0;

protected:
    // Counts the click and consumes it.
    bool handle(ui::ClickEvent&) override {
        ++hit;

        return true;
    }
};

}

// A window does not swallow a click: it raises itself and passes the click on up the tree, since a
// non-modal window must not eat the mouse of whatever stands above it.
TRIAL("Window: a click on a window goes on up") {
    ui::Root top;

    const auto catcher = std::make_shared<Catcher>();
    const auto window = std::make_shared<ui::Windows>();

    catcher->children().add(window);
    top.children().add(catcher);

    const auto one = std::make_shared<ui::Window>();

    one->children().add(std::make_shared<ui::Frame>());
    window->open(one);

    (void)top.render({{0, 0}, {10, 3}});

    ui::ClickEvent edge{{0, 0}, ui::Mouse::left};

    (void)top.dispatch(edge);

    same(catcher->hit, 1);
}

// --- Dragging with the mouse ---------------------------------------------------------------------

namespace {

// Sends one mouse event of the given kind at `where` to the root.
void mouse(ui::Root& top, ui::Spot where, ui::Press press, ui::Mouse which = ui::Mouse::left) {
    ui::MouseEvent what{where, press, which};

    (void)top.dispatch(what);
}

}

// A press on the top row grabs the position, and the window follows the cursor at the same grab
// point. A cursor beyond the limit pulls the window up to the limit, and when it comes back, back to
// where it holds it; the mouse reaches the window from outside it too, because the window asks for a
// hold.
TRIAL("Window: dragging by the top row moves the window, and the grab stays in place") {
    Stage stage;

    const auto one = stage.opened(U'a');

    one->seat({2, 1}, {6, 3});

    (void)stage.top.render({{0, 0}, {20, 10}});

    mouse(stage.top, {4, 1}, ui::Press::down);

    ought(one->dragged());

    mouse(stage.top, {10, 5}, ui::Press::move);
    same(one->seat().at.x, 8);
    same(one->seat().at.y, 5);

    mouse(stage.top, {100, 100}, ui::Press::move);
    same(one->seat().at.x, 19);
    same(one->seat().at.y, 9);

    mouse(stage.top, {10, 5}, ui::Press::move);
    same(one->seat().at.x, 8);
    same(one->seat().at.y, 5);

    // The size stays while moving.
    same(one->seat().size.wide, 6);
    same(one->seat().size.high, 3);

    mouse(stage.top, {12, 7}, ui::Press::up);

    ought(!one->dragged());

    mouse(stage.top, {15, 8}, ui::Press::move, ui::Mouse::none);
    same(one->seat().at.x, 8);
    same(one->seat().at.y, 5);
}

// A press in the lower right corner grabs the size: the corner follows the cursor, no less than
// `least`.
TRIAL("Window: dragging by the lower right corner resizes") {
    Stage stage;

    const auto one = stage.opened(U'a');

    one->seat({2, 1}, {6, 3});
    one->least({3, 2});

    (void)stage.top.render({{0, 0}, {20, 10}});

    mouse(stage.top, {7, 3}, ui::Press::down);

    ought(one->dragged());

    mouse(stage.top, {11, 6}, ui::Press::move);
    same(one->seat().size.wide, 10);
    same(one->seat().size.high, 6);

    mouse(stage.top, {0, 0}, ui::Press::move);
    same(one->seat().size.wide, 3);
    same(one->seat().size.high, 2);

    // The upper left corner stays.
    same(one->seat().at.x, 2);
    same(one->seat().at.y, 1);

    mouse(stage.top, {0, 0}, ui::Press::up);

    ought(!one->dragged());
}

// A press elsewhere raises the window but does not grab it; the right button does not grab at all,
// and neither does a full window.
TRIAL("Window: the middle raises without grabbing, the right button and a full window do not grab") {
    Stage stage;

    const auto one = stage.opened(U'a');
    const auto two = stage.opened(U'b');

    one->seat({0, 0}, {6, 4});
    two->seat({8, 0}, {6, 4});

    (void)stage.top.render({{0, 0}, {20, 10}});

    ought(two->current());

    mouse(stage.top, {2, 2}, ui::Press::down);

    ought(one->current());
    ought(!one->dragged());

    mouse(stage.top, {5, 5}, ui::Press::move);
    same(one->seat().at.x, 0);
    same(one->seat().at.y, 0);

    mouse(stage.top, {5, 5}, ui::Press::up);

    // The right edge and the bottom edge are not yet the corner.
    mouse(stage.top, {5, 2}, ui::Press::down);
    ought(!one->dragged());
    mouse(stage.top, {5, 2}, ui::Press::up);

    mouse(stage.top, {2, 3}, ui::Press::down);
    ought(!one->dragged());
    mouse(stage.top, {2, 3}, ui::Press::up);

    mouse(stage.top, {9, 0}, ui::Press::down, ui::Mouse::right);

    ought(!two->dragged());
    ought(one->current());

    mouse(stage.top, {9, 0}, ui::Press::up, ui::Mouse::right);

    two->full(true);

    (void)stage.top.render({{0, 0}, {20, 10}});

    mouse(stage.top, {9, 0}, ui::Press::down);

    ought(two->current());
    ought(!two->dragged());
}

// A lost release does not leave the window grabbed: a new press elsewhere starts over, and a move
// no longer carries the window.
TRIAL("Window: a new press after a lost release does not drag on") {
    Stage stage;

    const auto one = stage.opened(U'a');

    one->seat({2, 1}, {6, 3});

    (void)stage.top.render({{0, 0}, {20, 10}});

    mouse(stage.top, {4, 1}, ui::Press::down);

    ought(one->dragged());

    mouse(stage.top, {4, 2}, ui::Press::down);

    ought(!one->dragged());

    mouse(stage.top, {12, 7}, ui::Press::move);

    same(one->seat().at.x, 2);
    same(one->seat().at.y, 1);
}

namespace {

// A node above the windows that counts the raw mouse events reaching it.
class Watcher : public ui::Node {
public:
    int heard = 0;

protected:
    // Counts the mouse event and consumes it.
    bool handle(ui::MouseEvent&) override {
        ++heard;

        return true;
    }
};

}

// Dragging belongs to the window: the press, the moves and the release do not go up. A press the
// window did not grab goes on, like a click.
TRIAL("Window: dragging does not leave the window") {
    ui::Root top;

    const auto watcher = std::make_shared<Watcher>();
    const auto window = std::make_shared<ui::Windows>();

    watcher->children().add(window);
    top.children().add(watcher);

    const auto one = std::make_shared<ui::Window>();

    one->children().add(std::make_shared<ui::Frame>());
    window->open(one);
    one->seat({2, 1}, {6, 3});

    (void)top.render({{0, 0}, {20, 10}});

    mouse(top, {4, 1}, ui::Press::down);
    mouse(top, {8, 4}, ui::Press::move);
    mouse(top, {8, 4}, ui::Press::up);

    same(one->seat().at.x, 6);
    same(watcher->heard, 0);

    (void)top.render({{0, 0}, {20, 10}});

    // The middle of the window, which now stands at (6, 1): the window does not grab it, so the
    // press goes up.
    mouse(top, {8, 2}, ui::Press::down);

    ought(!one->dragged());
    same(watcher->heard, 1);
}
