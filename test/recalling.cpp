#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/historyfield.hpp"
#include "talcite/list.hpp"
#include "talcite/modal.hpp"
#include "talcite/root.hpp"
#include "talcite/modals.hpp"

using namespace talcite;

namespace {

// The unrolled list, if one is up: veil → frame → list. The trials ask it directly for its rows,
// scrollbar and current position, because those cannot be read off the screen without counting
// cells.
ui::List* rolled(const ui::Modals& window) {
    const ui::Modal* veil = window.current();

    if (veil == nullptr) return nullptr;

    return dynamic_cast<ui::List*>(veil->children().front()->children().front().get());
}

// Counts in `done` every pick that the history field announces, while the returned hook is kept.
ui::Hook counted(const std::shared_ptr<ui::HistoryField>& one, int& done) {
    return one->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });
}

// The history inside a window, because without a window there is nowhere to unroll its veils.
struct Stage {
    ui::Root top;
    std::shared_ptr<ui::Modals> window = std::make_shared<ui::Modals>();
    std::shared_ptr<ui::Column> column = std::make_shared<ui::Column>();
    std::shared_ptr<ui::HistoryField> one = std::make_shared<ui::HistoryField>(ui::plenty);

    // Puts the field in a column inside the window and gives it the focus.
    Stage() {
        column->children().add(one);
        window->content(column);
        top.children().add(window);
        one->focus();
    }

    // Three commands, the most recent first, so "git status" is the one the first up arrow
    // returns to.
    void filled() {
        one->options().insert(0, "make -j8");
        one->options().insert(0, "ls -la /etc");
        one->options().insert(0, "git status");
    }

    // Renders the whole stage at the given size.
    View drawn(ui::Size size = {40, 12}) { return top.render({{0, 0}, size}); }
};

}

// --- What the history knows about itself ---------------------------------------------------------

TRIAL("HistoryField: a fresh one remembers nothing and is an ordinary field") {
    const ui::HistoryField one{20};

    ought(one.options().empty());
    ought(one.text().empty());
    ought(one.focusable());
    ought(!one.opened());
    same(one.keeps(), 60);
}

TRIAL("HistoryField: adding puts the most recent entry at the head") {
    Stage stage;

    stage.filled();

    same(static_cast<int>(stage.one->options().many()), 3);
    same(stage.one->options().front(), std::string{"git status"});
    same(stage.one->options().back(), std::string{"make -j8"});
}

// A repeated entry goes back to the head instead of lying there twice; otherwise the history of
// someone who typed the same thing ten times would consist of it alone.
TRIAL("HistoryField: a repeated entry goes back to the head instead of being added a second time") {
    Stage stage;

    stage.filled();
    stage.one->options().insert(0, "make -j8");

    same(static_cast<int>(stage.one->options().many()), 3);
    same(stage.one->options().front(), std::string{"make -j8"});
}

// The rule belongs to the option set, not to the history, and holds the same in a select and in an
// option group.
TRIAL("HistoryField: an empty entry does not go in") {
    Stage stage;

    stage.one->options().insert(0, "");

    ought(stage.one->options().empty());
}

// The ceiling applies from the next frame. The history tidies up after itself, when painting and on
// a key, instead of demanding to be told about every change of the set. So between adding an entry
// and the first use of the field the list may be one entry too long, and that is the price of
// `Options` not knowing its owner.
TRIAL("HistoryField: the ceiling cuts the oldest") {
    Stage stage;

    stage.one->keeps(2);
    stage.filled();

    (void)stage.drawn();

    same(static_cast<int>(stage.one->options().many()), 2);
    same(stage.one->options().front(), std::string{"git status"});
    same(stage.one->options().back(), std::string{"ls -la /etc"});
}

TRIAL("HistoryField: lowering the ceiling cuts what is already there") {
    Stage stage;

    stage.filled();
    stage.one->keeps(1);

    same(static_cast<int>(stage.one->options().many()), 1);
    same(stage.one->options().front(), std::string{"git status"});
}

TRIAL("HistoryField: clear clears everything") {
    Stage stage;

    stage.filled();
    stage.one->clear();

    ought(stage.one->options().empty());
}

// --- Walking with the arrows ---------------------------------------------------------------------

TRIAL("HistoryField: up goes into the past, down comes back from it") {
    Stage stage;

    stage.filled();

    ui::KeyEvent up{ui::Code::up};

    ought(stage.top.dispatch(up));
    same(stage.one->text(), std::string{"git status"});

    ought(stage.top.dispatch(up));
    same(stage.one->text(), std::string{"ls -la /etc"});

    ui::KeyEvent down{ui::Code::down};

    ought(stage.top.dispatch(down));
    same(stage.one->text(), std::string{"git status"});
}

// This is the thing without which the history is a trap: once in, there is no way back to your own
// line. Down from the most recent entry restores what the user was typing.
TRIAL("HistoryField: down from the most recent returns to what the user was typing") {
    Stage stage;

    stage.filled();
    stage.one->text("cos wlasnego");

    ui::KeyEvent up{ui::Code::up};

    ought(stage.top.dispatch(up));
    same(stage.one->text(), std::string{"git status"});

    ui::KeyEvent down{ui::Code::down};

    ought(stage.top.dispatch(down));
    same(stage.one->text(), std::string{"cos wlasnego"});
}

// Further steps up must come back to the same own line, not to the previous entry.
TRIAL("HistoryField: the own line is remembered once, on entering the past") {
    Stage stage;

    stage.filled();
    stage.one->text("moje");

    ui::KeyEvent up{ui::Code::up};
    ui::KeyEvent down{ui::Code::down};

    ought(stage.top.dispatch(up));
    ought(stage.top.dispatch(up));
    ought(stage.top.dispatch(up));

    same(stage.one->text(), std::string{"make -j8"});

    ought(stage.top.dispatch(down));
    ought(stage.top.dispatch(down));
    ought(stage.top.dispatch(down));

    same(stage.one->text(), std::string{"moje"});
}

// At either end the arrow goes up the tree, so that in a window it passes on instead of getting
// stuck.
TRIAL("HistoryField: at the ends it passes the arrow up") {
    Stage stage;

    stage.filled();

    ui::KeyEvent down{ui::Code::down};

    // At the own line there is nowhere to go forward.
    ought(!stage.top.dispatch(down));

    ui::KeyEvent up{ui::Code::up};

    ought(stage.top.dispatch(up));
    ought(stage.top.dispatch(up));
    ought(stage.top.dispatch(up));

    // The oldest entry is the end of the past.
    ought(!stage.top.dispatch(up));
    same(stage.one->text(), std::string{"make -j8"});
}

TRIAL("HistoryField: an empty one does not take the arrows") {
    Stage stage;

    ui::KeyEvent up{ui::Code::up};

    ought(!stage.top.dispatch(up));

    ui::KeyEvent down{ui::Code::down};

    ought(!stage.top.dispatch(down));
}

// --- Notification --------------------------------------------------------------------------------

// We notify whenever an entry from the past lands in the field. Typing is not announced and will
// not be; returning to the own line is not taking anything from the past either.
TRIAL("HistoryField: taking an entry notifies, returning to your own does not") {
    Stage stage;

    stage.filled();

    int done = 0;

    const ui::Hook counting = counted(stage.one, done);

    ui::KeyEvent up{ui::Code::up};

    ought(stage.top.dispatch(up));
    same(done, 1);

    ought(stage.top.dispatch(up));
    same(done, 2);

    ui::KeyEvent down{ui::Code::down};

    ought(stage.top.dispatch(down));
    same(done, 3);

    ought(stage.top.dispatch(down));
    same(done, 3);
}

TRIAL("HistoryField: typing does not notify") {
    Stage stage;

    stage.filled();

    int done = 0;

    const ui::Hook counting = counted(stage.one, done);

    ui::KeyEvent letter{U'x'};

    ought(stage.top.dispatch(letter));

    same(stage.one->text(), std::string{"x"});
    same(done, 0);
}

// --- The text field underneath -------------------------------------------------------------------

// The history is a field and takes nothing away from it: letters, erasing and the cursor must keep
// working.
TRIAL("HistoryField: it is still an ordinary text field") {
    Stage stage;

    stage.filled();

    for (const char32_t one : {U'a', U'b', U'c'}) {
        ui::KeyEvent what{one};

        ought(stage.top.dispatch(what));
    }

    same(stage.one->text(), std::string{"abc"});

    ui::KeyEvent left{ui::Code::left};

    ought(stage.top.dispatch(left));

    ui::KeyEvent back{ui::Code::backspace};

    ought(stage.top.dispatch(back));

    same(stage.one->text(), std::string{"ac"});
}

// An entry taken from the past has the cursor at the end, so that typing can go on at once.
TRIAL("HistoryField: a taken entry has the cursor at the end") {
    Stage stage;

    stage.filled();

    ui::KeyEvent up{ui::Code::up};

    ought(stage.top.dispatch(up));

    same(stage.one->cursor(), std::string{"git status"}.size());
}

// --- Unrolling -----------------------------------------------------------------------------------

// There is no default shortcut: nothing is bound before the caller says anything. Whoever wants
// unrolling from the keyboard gives the key.
TRIAL("HistoryField: without a given key nothing unrolls") {
    Stage stage;

    stage.filled();

    ui::Mods alt;
    alt.alt = true;

    ui::KeyEvent what{ui::Key{U'h', alt}};

    ought(!stage.top.dispatch(what));
    ought(!stage.one->opened());
}

TRIAL("HistoryField: the given unroll key opens the list, Esc closes it") {
    Stage stage;

    stage.filled();

    ui::Mods alt;
    alt.alt = true;

    stage.one->keys().add(ui::Key{U'h', alt});

    ui::KeyEvent what{ui::Key{U'h', alt}};

    ought(stage.top.dispatch(what));
    ought(stage.one->opened());
    same(static_cast<int>(stage.window->many()), 1);

    ui::KeyEvent escape{ui::Code::escape};

    ought(stage.top.dispatch(escape));

    ought(!stage.one->opened());
    same(stage.top.focused_node(), static_cast<ui::Node*>(stage.one.get()));
}

TRIAL("HistoryField: the unroll key can be changed") {
    Stage stage;

    stage.filled();
    stage.one->keys().add(ui::Key{ui::Code::f4});

    ui::KeyEvent what{ui::Code::f4};

    ought(stage.top.dispatch(what));
    ought(stage.one->opened());
}

// A plain key without a modifier must type, not unroll.
TRIAL("HistoryField: a plain letter without a modifier does not unroll") {
    Stage stage;

    stage.filled();

    ui::KeyEvent what{U'h'};

    ought(stage.top.dispatch(what));

    ought(!stage.one->opened());
    same(stage.one->text(), std::string{"h"});
}

TRIAL("HistoryField: an empty one has nothing to unroll") {
    Stage stage;

    ui::Mods alt;
    alt.alt = true;

    ui::KeyEvent what{ui::Key{U'h', alt}};

    (void)stage.top.dispatch(what);

    ought(!stage.one->opened());
    ought(stage.window->empty());
}

// The unrolled list stands below the field, as with a select, because it is the same veil.
TRIAL("HistoryField: the unrolled list stands below the field") {
    Stage stage;

    stage.filled();

    (void)stage.drawn();

    stage.one->open();

    const View made = stage.drawn();

    same(made.at({0, 1}).letter, U'┌');
    same(made.at({1, 2}).letter, U'g');
}

TRIAL("HistoryField: the number of list rows has a lower bound") {
    ui::HistoryField one{20};

    same(one.rows(), 8);

    one.rows(-3);

    same(one.rows(), 1);

    one.rows(2);

    same(one.rows(), 2);

    Stage stage;

    stage.filled();
    stage.one->rows(2);
    stage.one->open();

    const ui::List* list = rolled(*stage.window);

    ought(list != nullptr);
    same(list->high(), 2);
    ought(list->scrollbar());
}

// The list stands where the walk stands: two steps up and unrolling shows the second entry.
TRIAL("HistoryField: the unrolled list stands on the walk's entry") {
    Stage stage;

    stage.filled();

    ui::KeyEvent up{ui::Code::up};

    ought(stage.top.dispatch(up));
    ought(stage.top.dispatch(up));

    stage.one->open();

    const ui::List* list = rolled(*stage.window);

    ought(list != nullptr);
    same(list->which(), std::optional<std::size_t>{1});
}

// Unrolling first tidies up after the caller, as painting and keys do: entries above the ceiling do
// not reach the list, even when nothing has been drawn yet.
TRIAL("HistoryField: unrolling trims to the ceiling") {
    Stage stage;

    stage.one->keeps(2);
    stage.filled();
    stage.one->open();

    const ui::List* list = rolled(*stage.window);

    ought(list != nullptr);
    same(list->children().many(), std::size_t{2});
}

TRIAL("HistoryField: a pick from the list goes into the field and notifies") {
    Stage stage;

    stage.filled();

    int done = 0;

    const ui::Hook counting = counted(stage.one, done);

    stage.one->open();

    (void)stage.drawn();

    ui::KeyEvent down{ui::Code::down};

    ought(stage.top.dispatch(down));

    ui::KeyEvent enter{ui::Code::enter};

    ought(stage.top.dispatch(enter));

    same(stage.one->text(), std::string{"ls -la /etc"});
    same(done, 1);
    ought(!stage.one->opened());
}

// Adding a fresh entry ends the walk: we are back at what is in the field.
TRIAL("HistoryField: adding an entry ends the walk") {
    Stage stage;

    stage.filled();

    ui::KeyEvent up{ui::Code::up};

    ought(stage.top.dispatch(up));
    same(stage.one->text(), std::string{"git status"});

    stage.one->options().insert(0, "cargo build");

    ui::KeyEvent down{ui::Code::down};

    // Since the walk has ended, down has nowhere to go and goes up the tree.
    ought(!stage.top.dispatch(down));
}

// --- Disabled ------------------------------------------------------------------------------------

TRIAL("HistoryField: a disabled one neither unrolls nor walks") {
    Stage stage;

    stage.filled();
    stage.one->enabled(false);

    stage.one->open();

    ought(!stage.one->opened());

    ui::KeyEvent up{ui::Code::up};

    (void)stage.top.dispatch(up);

    ought(stage.one->text().empty());
}

TRIAL("HistoryField: disabling midway closes the unrolled list and gives up the focus") {
    Stage stage;

    stage.filled();
    stage.one->open();

    ought(stage.one->opened());
    ought(stage.top.focused_node() != static_cast<ui::Node*>(stage.one.get()));

    stage.one->enabled(false);

    ought(!stage.one->opened());
    ought(stage.window->empty());
    ought(!stage.one->focused());
}

namespace {

// A container that, as a widget, listens for disabling in its subtree.
class Watcher : public ui::Column {
public:
    const ui::Handler* seen = nullptr;

protected:
    // Records which node was disabled and takes the event.
    bool handle(ui::DisableEvent& what) override {
        seen = what.target();

        return true;
    }
};

}

TRIAL("HistoryField: rolling up the list does not hide the disabling from ancestors") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto column = std::make_shared<Watcher>();
    const auto one = std::make_shared<ui::HistoryField>(ui::plenty);

    column->children().add(one);
    window->content(column);
    top.children().add(window);

    one->options().insert(0, "git status");
    one->open();
    one->enabled(false);

    ought(!one->opened());
    same(static_cast<const void*>(column->seen), static_cast<const void*>(one.get()));
}

// A hotkey checks availability in `KeyedNode`, and the history gets it from the text field. A
// disabled one must not swallow its key, or the same shortcut would reach nobody else.
TRIAL("HistoryField: a disabled one does not take its key") {
    Stage stage;

    stage.filled();

    ui::Mods alt;
    alt.alt = true;

    stage.one->keys().add(ui::Key{U'h', alt});
    stage.one->enabled(false);

    ui::KeyEvent with{ui::Key{U'h', alt}};

    ought(!stage.top.dispatch(with));
    ought(!stage.one->opened());
}

// The history is a node with keys, so its unroll key lies in `keys()`, together with its
// modifiers.
TRIAL("HistoryField: a key with Alt answers only with Alt") {
    Stage stage;

    stage.filled();

    ui::Mods alt;
    alt.alt = true;

    stage.one->keys().add(ui::Key{U'h', alt});

    ui::KeyEvent bare{ui::Key{U'h'}};

    ought(stage.top.dispatch(bare));
    ought(!stage.one->opened());
    same(stage.one->text(), std::string{"h"});

    ui::KeyEvent with{ui::Key{U'h', alt}};

    ought(stage.top.dispatch(with));
    ought(stage.one->opened());
}
