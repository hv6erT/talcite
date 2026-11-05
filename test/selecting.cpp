#include "proving.hpp"

#include <memory>
#include <string>
#include <vector>

#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/encoding.hpp"
#include "talcite/list.hpp"
#include "talcite/modal.hpp"
#include "talcite/root.hpp"
#include "talcite/select.hpp"
#include "talcite/modals.hpp"

using namespace talcite;

namespace {

// Three encoding names, the options of every select in these trials.
std::vector<std::string> three() { return {"UTF-8", "ISO-8859-2", "CP1250"}; }

// Reads row `y` of a drawn view as UTF-8 text, skipping the empty halves of wide cells and
// dropping trailing spaces.
std::string read(const Snapshot& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter == 0) continue;

        made += spelled(letter);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

// The rolled-out list, if one is open: veil, then frame, then list. The trials ask it directly
// for its rows, its scrollbar and its current position, since those cannot be read off the screen
// without counting cells.
ui::List* rolled(const ui::Modals& window) {
    const ui::Modal* veil = window.current();

    if (veil == nullptr) return nullptr;

    return dynamic_cast<ui::List*>(veil->children().front()->children().front().get());
}

// Counts in `done` every `ui::PickEvent` that `one` sends, while the returned hook is kept.
ui::Hook counted(const std::shared_ptr<ui::Select>& one, int& done) {
    return one->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });
}

// A select inside a window, because without a window there is nowhere to roll out the veil. That
// is part of the contract, not a limitation of the trial.
struct Stage {
    ui::Root top;
    std::shared_ptr<ui::Modals> window = std::make_shared<ui::Modals>();
    std::shared_ptr<ui::Column> column = std::make_shared<ui::Column>();
    std::shared_ptr<ui::Select> one = std::make_shared<ui::Select>(three());

// Puts the select into a column inside the window and hangs the window on the root.
    Stage() {
        column->children().add(one);
        window->content(column);
        top.children().add(window);
    }

// Draws the whole stage at the given size.
    Snapshot drawn(ui::Size size = {30, 12}) { return top.render({{0, 0}, size}); }
};

}

// --- What a select knows about itself ------------------------------------------------------------

TRIAL("Select: an empty one carries nothing but takes focus") {
    const ui::Select one;

    same(one.options().many(), std::size_t{0});
    ought(!one.value().has_value());
    ought(one.focusable());
    same(one.capacity(), 0);
    ought(!one.opened());
}

TRIAL("Select: a fresh one stands on the first option") {
    const ui::Select one{three()};

    same(one.options().many(), std::size_t{3});
    same(*one.value(), std::string{"UTF-8"});
}

TRIAL("Select: the choice can be given at construction and is clamped") {
    same(*ui::Select(three(), 2).value(), std::string{"CP1250"});
    same(*ui::Select(three(), 9).value(), std::string{"CP1250"});
}

// The longest option plus two cells for a space and the roll-out mark; the height is always one
// row.
TRIAL("Select: wants the longest option and one row") {
    const ui::Select one{three()};

    same(one.wants({40, 10}), (ui::Size{12, 1}));
    same(one.wants({6, 3}), (ui::Size{6, 1}));
}

TRIAL("Select: giving new options clamps the choice") {
    ui::Select one{three(), 2};

    one.options().all({"raz"});

    same(*one.value(), std::string{"raz"});
}

// --- Drawing -------------------------------------------------------------------------------------

// The row comes out as wide as the longest option plus two cells, because that is what it wants,
// and the roll-out mark stands at the right edge of what it was given.
TRIAL("Select: draws the current value and the roll-out mark at the right edge") {
    Stage stage;

    const Snapshot made = stage.drawn();

    same(read(made, 0), std::string{"UTF-8"} + std::string(6, ' ') + "\xe2\x96\xbc");
}

TRIAL("Select: when too narrow it cuts the text, not the roll-out mark") {
    Stage stage;

    stage.one->value("ISO-8859-2");

    const Snapshot made = stage.drawn({8, 12});

    // The roll-out mark keeps its place, and the text makes way for it with an ellipsis.
    same(made.at({7, 0}).letter, U'▼');
    same(made.at({5, 0}).letter, U'…');
    same(made.at({0, 0}).letter, U'I');
}

TRIAL("Select: has its own role when focused") {
    Stage stage;

    ought(!stage.drawn().at({0, 0}).style.reverse);

    stage.one->focus();

    ought(stage.drawn().at({0, 0}).style.reverse);
}

// --- Keys while closed ---------------------------------------------------------------------------

// Moving is choosing: an arrow changes the value at once, just as in a radio group.
// The side arrows change the value and notify; the up and down arrows leave it alone and go up the
// tree, where a window moves the focus with them.
TRIAL("Select: the side arrows change the value and notify, up and down pass") {
    Stage stage;

    stage.one->focus();

    int done = 0;

    const ui::Hook counting = counted(stage.one, done);

    ui::KeyEvent right{ui::Code::right};

    ought(stage.top.dispatch(right));
    same(*stage.one->value(), std::string{"ISO-8859-2"});
    same(done, 1);

    ui::KeyEvent left{ui::Code::left};

    ought(stage.top.dispatch(left));
    same(*stage.one->value(), std::string{"UTF-8"});
    same(done, 2);

    ui::KeyEvent down{ui::Code::down};
    ui::KeyEvent up{ui::Code::up};

    ought(!stage.top.dispatch(down));
    ought(!stage.top.dispatch(up));
    same(*stage.one->value(), std::string{"UTF-8"});
    same(done, 2);

    ought(!stage.one->opened());
}

// At either end a side arrow goes up the tree too.
TRIAL("Select: at either end it passes the side arrow up") {
    Stage stage;

    stage.one->focus();

    ui::KeyEvent left{ui::Code::left};

    ought(!stage.top.dispatch(left));

    stage.one->value("CP1250");

    ui::KeyEvent right{ui::Code::right};

    ought(!stage.top.dispatch(right));
    same(*stage.one->value(), std::string{"CP1250"});
}

TRIAL("Select: Home and End jump to the ends") {
    Stage stage;

    stage.one->focus();

    ui::KeyEvent end{ui::Code::end};

    ought(stage.top.dispatch(end));
    same(*stage.one->value(), std::string{"CP1250"});

    ui::KeyEvent home{ui::Code::home};

    ought(stage.top.dispatch(home));
    same(*stage.one->value(), std::string{"UTF-8"});
}

// Letters go up the tree, and that is required so that a button's hotkey works while focus is on
// the select. A menu does the same: it takes its own keys and lets the rest through.
TRIAL("Select: letters and Tab go up the tree") {
    Stage stage;

    stage.one->focus();

    ui::KeyEvent letter{U'z'};

    ought(!stage.top.dispatch(letter));

    ui::KeyEvent tab{ui::Code::tab};

    ought(!stage.top.dispatch(tab));
}

TRIAL("Select: setting the value is silent") {
    Stage stage;

    int done = 0;

    const ui::Hook counting = counted(stage.one, done);

    stage.one->value("CP1250");

    same(*stage.one->value(), std::string{"CP1250"});
    same(done, 0);
}

// --- Rolling out ---------------------------------------------------------------------------------

TRIAL("Select: Enter rolls out, Esc closes and gives focus back") {
    Stage stage;

    stage.one->focus();

    ui::KeyEvent enter{ui::Code::enter};

    ought(stage.top.dispatch(enter));
    ought(stage.one->opened());
    same(static_cast<int>(stage.window->many()), 1);

    ui::KeyEvent escape{ui::Code::escape};

    ought(stage.top.dispatch(escape));

    ought(!stage.one->opened());
    ought(stage.window->empty());
    same(stage.top.focused_node(), static_cast<ui::Node*>(stage.one.get()));
}

// Space does not roll out. In a window with a text field that key belongs to typing, so the select
// must not take it from the application. The button does not take it either.
TRIAL("Select: space does not roll out") {
    Stage stage;

    stage.one->focus();

    ui::KeyEvent space{U' '};

    ought(!stage.top.dispatch(space));
    ought(!stage.one->opened());
}

// The rolled-out list stands below the widget, not in the middle of the screen like a plain window.
TRIAL("Select: the rolled-out list stands below the select") {
    Stage stage;

    (void)stage.drawn();

    stage.one->open();

    const Snapshot made = stage.drawn();

    // Row zero is the select itself; the list's frame starts in row one, at the left edge.
    same(made.at({0, 1}).letter, U'┌');
    same(made.at({0, 2}).letter, U'│');
    same(made.at({1, 2}).letter, U'U');
    same(made.at({1, 3}).letter, U'I');
}

// There is no room below the bottom edge, so the list opens upward. Only the one who has the whole
// screen knows that it does not fit.
TRIAL("Select: with no room below, the list opens upward") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto column = std::make_shared<ui::Column>();
    const auto one = std::make_shared<ui::Select>(three());

    // The select stands in the last row of the screen.
    for (int at = 0; at < 6; ++at)
        column->children().add(std::make_shared<ui::Select>(std::vector<std::string>{"x"}));

    column->children().add(one);
    window->content(column);
    top.children().add(window);

    (void)top.render({{0, 0}, {30, 7}});

    one->open();

    const Snapshot made = top.render({{0, 0}, {30, 7}});

    // The select is in the last row, so row 6 still shows the select itself with its mark.
    same(made.at({11, 6}).letter, U'▼');

    // And the list's frame ends right above it.
    same(made.at({0, 5}).letter, U'└');
    same(made.at({0, 1}).letter, U'┌');
}

// The caller says how many rows are visible at once; fewer than one makes no sense, so zero means
// one.
TRIAL("Select: the number of list rows has a lower bound") {
    ui::Select one{three()};

    same(one.rows(), 8);

    one.rows(0);

    same(one.rows(), 1);

    one.rows(2);

    same(one.rows(), 2);
}

// A list longer than the row count gets as many rows as it was told, with a scrollbar beside it. A
// shorter one gets as many rows as it has items, and no scrollbar.
TRIAL("Select: the list has the rows it was told, and a longer one gets a scrollbar") {
    Stage stage;

    stage.one->rows(2);
    stage.one->open();

    const ui::List* list = rolled(*stage.window);

    ought(list != nullptr);
    same(list->high(), 2);
    ought(list->scrollbar());

    stage.one->close();
    stage.one->rows(5);
    stage.one->open();

    list = rolled(*stage.window);

    ought(list != nullptr);
    same(list->high(), 3);
    ought(!list->scrollbar());
}

// The rolled-out list stands on the current value: Enter without moving chooses what is already
// there instead of jumping to the first item.
TRIAL("Select: the rolled-out list stands on the current value") {
    Stage stage;

    stage.one->value("CP1250");
    stage.one->open();

    const ui::List* list = rolled(*stage.window);

    ought(list != nullptr);
    same(list->which(), std::optional<std::size_t>{2});
}

// Closing by the caller reports "closed" at once, even though the removed veil may live on in the
// root for a moment.
TRIAL("Select: closing reports closed at once") {
    Stage stage;

    stage.one->open();
    stage.one->close();

    ought(!stage.one->opened());
    ought(stage.window->empty());
}

TRIAL("Select: rolling out twice does not make two lists") {
    Stage stage;

    stage.one->open();
    stage.one->open();

    same(static_cast<int>(stage.window->many()), 1);
}

TRIAL("Select: an empty one has nothing to roll out") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Select>();

    window->content(one);
    top.children().add(window);

    one->open();

    ought(!one->opened());
    ought(window->empty());
}

// --- Choosing from the rolled-out list -----------------------------------------------------------

// The choice comes back as one event from the select, not from the list, so the application does
// not need to know that any list was involved.
TRIAL("Select: an item chosen with Enter comes back as an event and closes the list") {
    Stage stage;

    stage.one->focus();

    int done = 0;

    const ui::Hook counting = counted(stage.one, done);

    stage.one->open();

    (void)stage.drawn();

    ui::KeyEvent down{ui::Code::down};

    ought(stage.top.dispatch(down));

    ui::KeyEvent enter{ui::Code::enter};

    ought(stage.top.dispatch(enter));

    same(*stage.one->value(), std::string{"ISO-8859-2"});
    same(done, 1);
    ought(!stage.one->opened());
}

// The choice arrives once the list is gone: a caller that opens something new in response, or asks
// `opened()`, sees the state after the list rolled up.
TRIAL("Select: the pick event arrives with the list rolled up") {
    Stage stage;

    bool open = true;

    const ui::Hook heard_pick = stage.one->on<ui::PickEvent>([&open, &stage](ui::PickEvent&) {
        open = stage.one->opened();

        return true;
    });

    stage.one->focus();
    stage.one->open();

    (void)stage.drawn();

    ui::KeyEvent down{ui::Code::down};

    ought(stage.top.dispatch(down));

    ui::KeyEvent enter{ui::Code::enter};

    ought(stage.top.dispatch(enter));

    ought(!open);
}

// Choosing the same item is not a change, the same rule as clicking an already chosen option in a
// radio group.
TRIAL("Select: choosing the same item does not notify") {
    Stage stage;

    stage.one->focus();

    int done = 0;

    const ui::Hook counting = counted(stage.one, done);

    stage.one->open();

    (void)stage.drawn();

    ui::KeyEvent enter{ui::Code::enter};

    ought(stage.top.dispatch(enter));

    same(done, 0);
    ought(!stage.one->opened());
}

// --- Clicking ------------------------------------------------------------------------------------

// A click rolls out, and focus goes into the rolled-out list, as with every other veil that opens.
// It returns to the select only after the list closes.
TRIAL("Select: a click takes focus and rolls out") {
    Stage stage;

    (void)stage.drawn();

    ui::ClickEvent what{{2, 0}, ui::Mouse::left};

    ought(stage.top.dispatch(what));
    ought(stage.one->opened());
    ought(stage.top.focused_node() != static_cast<ui::Node*>(stage.one.get()));

    ui::KeyEvent escape{ui::Code::escape};

    ought(stage.top.dispatch(escape));

    ought(!stage.one->opened());
    same(stage.top.focused_node(), static_cast<ui::Node*>(stage.one.get()));
}

// A click beside the rolled-out list rolls it up and gives the focus back, choosing nothing; a
// click on one of its rows chooses it, as before.
TRIAL("Select: a click beside the rolled-out list closes it, a click on a row chooses") {
    Stage stage;

    int done = 0;

    const ui::Hook counting = counted(stage.one, done);

    (void)stage.drawn();

    stage.one->focus();
    stage.one->open();

    (void)stage.drawn();

    ui::ClickEvent beside{{25, 10}, ui::Mouse::left};

    ought(stage.top.dispatch(beside));
    ought(!stage.one->opened());
    ought(stage.window->empty());
    same(stage.top.focused_node(), static_cast<ui::Node*>(stage.one.get()));
    same(done, 0);

    stage.one->open();

    (void)stage.drawn();

    // Rows: the select in row zero, the frame from row one, the options in rows two to four.
    ui::ClickEvent third{{2, 4}, ui::Mouse::left};

    ought(stage.top.dispatch(third));
    ought(!stage.one->opened());
    same(done, 1);
    same(*stage.one->value(), std::string{"CP1250"});
}

// --- Disabled ------------------------------------------------------------------------------------

TRIAL("Select: when disabled it does not roll out, does not choose and gives up focus") {
    Stage stage;

    stage.one->focus();
    stage.one->enabled(false);

    ought(!stage.one->focusable());
    ought(stage.top.focused_node() != static_cast<ui::Node*>(stage.one.get()));

    stage.one->open();

    ought(!stage.one->opened());

    ui::KeyEvent down{ui::Code::down};

    (void)stage.top.dispatch(down);

    same(*stage.one->value(), std::string{"UTF-8"});
}

TRIAL("Select: disabling it while open closes the rolled-out list") {
    Stage stage;

    stage.one->focus();
    stage.one->open();

    ought(stage.one->opened());
    ought(stage.top.focused_node() != static_cast<ui::Node*>(stage.one.get()));

    stage.one->enabled(false);

    ought(!stage.one->opened());
    ought(stage.window->empty());

    // Focus was in the list. The closed list gives it back to the select, which is now disabled and
    // passes it on.
    ought(stage.top.focused_node() != static_cast<ui::Node*>(stage.one.get()));
    ought(!stage.one->focused());
}

namespace {

// A container that, as a widget, listens for disable events in its subtree.
class Watcher : public ui::Column {
public:
    const ui::Handler* seen = nullptr;

protected:
    // Remembers the target of the disable event and consumes it.
    bool handle(ui::DisableEvent& what) override {
        seen = what.target();

        return true;
    }
};

}

TRIAL("Select: rolling up the list does not hide the disable event from ancestors") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto column = std::make_shared<Watcher>();
    const auto one = std::make_shared<ui::Select>(three());

    column->children().add(one);
    window->content(column);
    top.children().add(window);

    one->open();
    one->enabled(false);

    ought(!one->opened());
    same(static_cast<const void*>(column->seen), static_cast<const void*>(one.get()));
}

// --- In a dialog ---------------------------------------------------------------------------------

// Focus was in the rolled-out list, and the closed list gives it back to whoever had it before:
// the select, which no longer accepts it. Focus must then land on the dialog, because a dialog
// swallows keys and hands out hotkeys only while focus is inside it; otherwise a key would go
// straight to the root.
TRIAL("Select: disabled in a dialog with the list rolled out, it gives focus to the dialog") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto one = std::make_shared<ui::Select>(three());

    column->children().add(one);
    dialog->children().add(column);
    top.children().add(window);
    window->open(dialog);
    (void)top.render({{0, 0}, {30, 12}});

    one->focus();
    one->open();
    one->enabled(false);

    ought(!one->opened());
    same(static_cast<const void*>(top.focused_node()), static_cast<const void*>(dialog.get()));

    ui::KeyEvent key{ui::Key{U'x'}};

    ought(top.dispatch(key));
}

// A select is one stop of the Tab cycle, and a rolled-out list does not break the cycle.
TRIAL("Select: in a window it is one stop for Tab") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto one = std::make_shared<ui::Select>(three());
    const auto other = std::make_shared<ui::Select>(three());

    column->children().add(one);
    column->children().add(other);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    same(top.focused_node(), static_cast<ui::Node*>(one.get()));

    ui::KeyEvent tab{ui::Code::tab};

    ought(top.dispatch(tab));
    same(top.focused_node(), static_cast<ui::Node*>(other.get()));

    ought(top.dispatch(tab));
    same(top.focused_node(), static_cast<ui::Node*>(one.get()));
}

// The rolled-out list lies above the dialog, because both are veils in the same window and the
// stack decides the order. Closing the list gives focus back to the dialog, not to the application.
TRIAL("Select: a list rolled out in a dialog closes above it and gives focus back to it") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto one = std::make_shared<ui::Select>(three());

    column->children().add(one);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    (void)top.render({{0, 0}, {30, 12}});

    one->open();

    same(static_cast<int>(window->many()), 2);
    ought(one->opened());
    ought(window->current() != static_cast<ui::Modal*>(dialog.get()));

    ui::KeyEvent escape{ui::Code::escape};

    ought(top.dispatch(escape));

    same(static_cast<int>(window->many()), 1);
    same(top.focused_node(), static_cast<ui::Node*>(one.get()));
}

