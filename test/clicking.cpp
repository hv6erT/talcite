#include "proving.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "talcite/column.hpp"
#include "talcite/root.hpp"
#include "talcite/row.hpp"

using namespace talcite;

namespace {

// A root that records what reached it and in what order.
class Sheet : public ui::Root {
public:
    // Takes any number of children.
    [[nodiscard]] int capacity() const override { return ui::plenty; }

    std::vector<std::string> told;

protected:
    // Records a raw mouse event as a press, a release or a motion.
    bool handle(ui::MouseEvent& what) override {
        told.push_back(what.press() == ui::Press::down ? "wciśnięcie"
                       : what.press() == ui::Press::up ? "puszczenie"
                                                       : "ruch");

        return true;
    }

    // Records a click with its cell.
    bool handle(ui::ClickEvent& what) override {
        told.push_back("kliknięcie " + std::to_string(what.where().x) + "," +
                       std::to_string(what.where().y));

        return true;
    }

    // Records a double click with its cell.
    bool handle(ui::DoubleClickEvent& what) override {
        told.push_back("podwójne " + std::to_string(what.where().x) + "," +
                       std::to_string(what.where().y));

        return true;
    }
};

// A focusable node of a fixed size that counts the clicks and double clicks it gets.
class Box : public ui::Node {
public:
    explicit Box(ui::Size wants) : wants_(wants) {}

    // Always takes the focus.
    [[nodiscard]] bool focusable() const override { return true; }

    // Always the size it was made with.
    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

    int clicks = 0;
    int doubles = 0;

protected:
    // Counts a click.
    bool handle(ui::ClickEvent&) override {
        ++clicks;

        return true;
    }

    // Counts a double click.
    bool handle(ui::DoubleClickEvent&) override {
        ++doubles;

        return true;
    }

private:
    ui::Size wants_;
};

// The trial's clock. Events carry moments given directly, so whether a second click fit in the
// window is decided by the trial, not by the machine's load. Time stands still until the trial
// itself moves it with `later`.
std::chrono::steady_clock::time_point moment{};

// Moves the trial's clock forward by `much`.
void later(std::chrono::milliseconds much) { moment += much; }

// Sends a press of `which` at `where` at the trial's current moment.
void pressed(ui::Root& onto, ui::Spot where, ui::Mouse which = ui::Mouse::left) {
    ui::MouseEvent what{where, ui::Press::down, which, {}, moment};

    (void)onto.dispatch(what);
}

// Sends a release of `which` at `where` at the trial's current moment.
void freed(ui::Root& onto, ui::Spot where, ui::Mouse which = ui::Mouse::left) {
    ui::MouseEvent what{where, ui::Press::up, which, {}, moment};

    (void)onto.dispatch(what);
}

}

TRIAL("Click: press and release in the same cell is a click, at once") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.size(), std::size_t{3});
    same(top.told[0], std::string{"wciśnięcie"});
    same(top.told[1], std::string{"puszczenie"});
    same(top.told[2], std::string{"kliknięcie 3,2"});
}

TRIAL("Click: after the double-click window the next one in the same cell is single again") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});

    later(top.double_click() + std::chrono::milliseconds{1});

    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.size(), std::size_t{6});
    same(top.told.back(), std::string{"kliknięcie 3,2"});

    // And it opens a new window: the next one right after it is a double click.
    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.back(), std::string{"podwójne 3,2"});
}

// The window's limit belongs to the window: a second click exactly `double_click()` later is still
// a double click.
TRIAL("Click: a second one exactly at the window's limit is a double click") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});

    later(top.double_click());

    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.back(), std::string{"podwójne 3,2"});
}

TRIAL("Click: the double-click window is set on the root, 400 ms by default") {
    Sheet top;

    same(top.double_click(), std::chrono::milliseconds{400});

    top.double_click(std::chrono::milliseconds{100});

    pressed(top, {3, 2});
    freed(top, {3, 2});

    later(std::chrono::milliseconds{150});

    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.back(), std::string{"kliknięcie 3,2"});

    top.double_click(std::chrono::milliseconds{1000});

    later(std::chrono::milliseconds{150});

    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.back(), std::string{"podwójne 3,2"});
}

// A moment that is not given is the moment the event is made; this is how the terminal works.
TRIAL("Click: a mouse event without a given moment carries now") {
    const auto before = std::chrono::steady_clock::now();

    const ui::MouseEvent what{{0, 0}, ui::Press::down, ui::Mouse::left};

    const auto after = std::chrono::steady_clock::now();

    ought(what.when() >= before);
    ought(what.when() <= after);
}

TRIAL("Click: a second one within the window gives a double click, without a second single") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});
    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.back(), std::string{"podwójne 3,2"});

    // One click, the first, as in Windows; the second release is already a double click.
    int clicks = 0;

    for (const std::string& one : top.told) clicks += one.rfind("kliknięcie", 0) == 0 ? 1 : 0;

    same(clicks, 1);
}

TRIAL("Click: after a double click it starts over") {
    Sheet top;

    pressed(top, {1, 1});
    freed(top, {1, 1});
    pressed(top, {1, 1});
    freed(top, {1, 1});

    pressed(top, {1, 1});
    freed(top, {1, 1});

    same(top.told.back(), std::string{"kliknięcie 1,1"});

    // The third opened a new window, so the fourth is a double click again.
    pressed(top, {1, 1});
    freed(top, {1, 1});

    same(top.told.back(), std::string{"podwójne 1,1"});
}

TRIAL("Click: release in another cell is a drag") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {5, 2});

    same(top.told.size(), std::size_t{2});

    // The drag opened no window: a click in the release cell is single.
    pressed(top, {5, 2});
    freed(top, {5, 2});

    same(top.told.back(), std::string{"kliknięcie 5,2"});
}

TRIAL("Click: a drag does not break the double-click window after a click") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});

    pressed(top, {7, 7});
    freed(top, {9, 9});

    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.back(), std::string{"podwójne 3,2"});
}

TRIAL("Click: a second one in another cell is a separate single") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});

    pressed(top, {8, 4});
    freed(top, {8, 4});

    same(top.told.back(), std::string{"kliknięcie 8,4"});

    // The window moved to the second cell: a third click there is a double click.
    pressed(top, {8, 4});
    freed(top, {8, 4});

    same(top.told.back(), std::string{"podwójne 8,4"});
}

TRIAL("Click: a second one with another button is not a double") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});

    pressed(top, {3, 2}, ui::Mouse::right);
    freed(top, {3, 2}, ui::Mouse::right);

    same(top.told.back(), std::string{"kliknięcie 3,2"});

    // The window moved to the right button: a second right click is a double click.
    pressed(top, {3, 2}, ui::Mouse::right);
    freed(top, {3, 2}, ui::Mouse::right);

    same(top.told.back(), std::string{"podwójne 3,2"});
}

TRIAL("Click: releasing a button other than the pressed one means nothing") {
    Sheet top;

    pressed(top, {3, 2}, ui::Mouse::left);
    freed(top, {3, 2}, ui::Mouse::right);

    same(top.told.size(), std::size_t{2});
}

TRIAL("Click: a release alone, without a press, starts nothing") {
    Sheet top;

    freed(top, {3, 2});

    same(top.told.size(), std::size_t{1});

    // Nothing is waiting: a full click in this cell is single.
    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.back(), std::string{"kliknięcie 3,2"});
}

TRIAL("Click: mouse motion decides nothing") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});

    ui::MouseEvent what{{4, 4}, ui::Press::move, ui::Mouse::none, {}, moment};

    (void)top.dispatch(what);

    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.back(), std::string{"podwójne 3,2"});
}

TRIAL("Click: the raw event goes before the derived one") {
    Sheet top;

    pressed(top, {3, 2});
    freed(top, {3, 2});
    pressed(top, {3, 2});
    freed(top, {3, 2});

    same(top.told.size(), std::size_t{6});
    same(top.told[2], std::string{"kliknięcie 3,2"});
    same(top.told[4], std::string{"puszczenie"});
    same(top.told[5], std::string{"podwójne 3,2"});
}

TRIAL("Click: targets the node under the cursor, not the root") {
    Sheet top;

    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Box>(ui::Size{6, 2});
    const auto second = std::make_shared<Box>(ui::Size{6, 2});

    column->children().add(first);
    column->children().add(second);
    top.children().add(column);

    (void)top.render({{0, 0}, {6, 4}});

    pressed(top, {2, 3});
    freed(top, {2, 3});

    later(top.double_click() + std::chrono::milliseconds{1});

    same(first->clicks, 0);
    same(second->clicks, 1);

    pressed(top, {2, 3});
    freed(top, {2, 3});
    pressed(top, {2, 3});
    freed(top, {2, 3});

    same(second->doubles, 1);
}

// --- Mouse capture -------------------------------------------------------------------------------

namespace {

// A field that counts raw mouse events and, when told to, asks for capture on a press.
class Grabber : public ui::Node {
public:
    explicit Grabber(bool grabs) : grabs_(grabs) {}

    // Five cells by one.
    [[nodiscard]] ui::Size wants(ui::Size) const override { return {5, 1}; }

    int downs = 0;
    int moves = 0;
    int ups = 0;

protected:
    // Counts the event by kind and, on a press, asks the root for capture when `grabs_` is set.
    bool handle(ui::MouseEvent& what) override {
        if (what.press() == ui::Press::down) {
            ++downs;

            if (grabs_ && root() != nullptr) root()->capture(*this);
        } else if (what.press() == ui::Press::move) {
            ++moves;
        } else {
            ++ups;
        }

        return true;
    }

private:
    bool grabs_ = false;
};

// Two `Grabber` fields side by side in a row of ten cells; only the left one may capture.
struct Pair {
    ui::Root top;
    std::shared_ptr<ui::Row> row = std::make_shared<ui::Row>();
    std::shared_ptr<Grabber> left;
    std::shared_ptr<Grabber> right = std::make_shared<Grabber>(false);

    // Builds the tree and draws it once, so that the fields have their areas.
    explicit Pair(bool grabs) : left(std::make_shared<Grabber>(grabs)) {
        row->children().add(left);
        row->children().add(right);
        top.children().add(row);

        (void)top.render({{0, 0}, {10, 1}});
    }

    // Sends a raw mouse event to the root.
    void sent(ui::Spot where, ui::Press press, ui::Mouse which = ui::Mouse::left) {
        ui::MouseEvent what{where, press, which};

        (void)top.dispatch(what);
    }
};

}

// A node that asked for capture gets motion and the release wherever the cursor is, until the
// release. After it, motion goes under the cursor again.
TRIAL("Root: the captor gets motion and release outside itself") {
    Pair pair{true};

    pair.sent({1, 0}, ui::Press::down);
    pair.sent({7, 0}, ui::Press::move);
    pair.sent({8, 0}, ui::Press::up);

    same(pair.left->downs, 1);
    same(pair.left->moves, 1);
    same(pair.left->ups, 1);
    same(pair.right->moves, 0);
    same(pair.right->ups, 0);

    pair.sent({7, 0}, ui::Press::move, ui::Mouse::none);

    same(pair.left->moves, 1);
    same(pair.right->moves, 1);
}

// Capture is on request: whoever did not ask gets no motion outside itself; this is how hovering
// over menu items works.
TRIAL("Root: without a request motion goes under the cursor") {
    Pair pair{false};

    pair.sent({1, 0}, ui::Press::down);
    pair.sent({7, 0}, ui::Press::move);
    pair.sent({8, 0}, ui::Press::up);

    same(pair.left->downs, 1);
    same(pair.left->moves, 0);
    same(pair.left->ups, 0);
    same(pair.right->moves, 1);
    same(pair.right->ups, 1);
}

// A lost release does not tie the mouse forever: a new press goes under the cursor and removes
// the old capture.
TRIAL("Root: a press removes an old capture") {
    Pair pair{true};

    pair.sent({1, 0}, ui::Press::down);
    pair.sent({7, 0}, ui::Press::down);
    pair.sent({8, 0}, ui::Press::move);

    same(pair.left->downs, 1);
    same(pair.right->downs, 1);
    same(pair.left->moves, 0);
    same(pair.right->moves, 1);
}

// A captor that left the tree during a drag gets nothing more.
TRIAL("Root: a captor that left the tree gets nothing") {
    Pair pair{true};

    pair.sent({1, 0}, ui::Press::down);

    (void)pair.row->children().remove(pair.left.get());
    (void)pair.top.render({{0, 0}, {10, 1}});

    // The right field has moved into the left one's place.
    pair.sent({1, 0}, ui::Press::move);

    same(pair.left->moves, 0);
    same(pair.right->moves, 1);
}

// The back and forward buttons are commands, not pointing: they make no click, and a back press in
// the middle of a left click does not break it.
TRIAL("Click: the back and forward buttons make no clicks") {
    Sheet top;

    pressed(top, {3, 2}, ui::Mouse::back);
    freed(top, {3, 2}, ui::Mouse::back);

    same(top.told.size(), std::size_t{2});

    pressed(top, {3, 2});
    pressed(top, {3, 2}, ui::Mouse::forward);
    freed(top, {3, 2}, ui::Mouse::forward);
    freed(top, {3, 2});

    same(top.told.size(), std::size_t{7});
    same(top.told.back(), std::string{"kliknięcie 3,2"});
}
