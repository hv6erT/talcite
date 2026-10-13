#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/modals.hpp"
#include "talcite/encoding.hpp"
#include "talcite/field.hpp"
#include "talcite/root.hpp"
#include "talcite/row.hpp"

using namespace talcite;

#include "doing.hpp"

namespace {

// A focusable node of a fixed size that fills itself with x.
class Box : public ui::Node {
public:
    explicit Box(ui::Size wants) : wants_(wants) {}

    // Always takes the focus.
    [[nodiscard]] bool focusable() const override { return true; }

    // Always the size it was made with.
    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

protected:
    // Fills the given size with x.
    View paint(ui::Size given, ui::Area seen) override {
        View made{seen.size};

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                made.put({x - seen.at.x, y - seen.at.y}, U'x', Style{});

        return made;
    }

private:
    ui::Size wants_;
};

// The text of row `y` of the view, without trailing spaces.
std::string read(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter != 0) made += talcite::spelled(letter);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

}

// The label is plain and is drawn in full. There is no convention about characters inside it, so
// an ampersand, a bracket or anything else is an ordinary letter and means nothing.
TRIAL("Button: the label is drawn in full, with no conventions about characters") {
    ui::Root top;

    const auto one = std::make_shared<ui::Button>("Tak & nie");

    top.children().add(one);

    same(one->text(), std::string{"Tak & nie"});
    same(one->wants({40, 4}), (ui::Size{13, 1}));

    const View made = top.render({{0, 0}, {13, 1}});

    same(read(made, 0), std::string{"[ Tak & nie ]"});
}

namespace {

// A root that broadcasts a shortcut key itself. In a program a dialog, the legend or a menu does
// this, but broadcasting is protected, so the trial needs its own way in.
class Top : public ui::Root {
public:
    using ui::Root::broadcast;
};

// Whether the button answers this shortcut key. We ask the way a window does, by delivering the
// event, not through a separate query method, because there deliberately is none.
bool hotly(Top& top, const std::shared_ptr<ui::Button>& one, ui::Key what) {
    int done = 0;

    ui::Hook held = one->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });

    ui::KeyEvent key{what};

    (void)top.broadcast(key);

    one->off(held);

    return done != 0;
}

}

TRIAL("Button: the caller gives the shortcut keys") {
    Top top;

    const auto one = std::make_shared<ui::Button>("Tak");

    top.children().add(one);

    // Without keys the button answers nothing.
    ought(!hotly(top, one, ui::Key{U'T'}));

    one->keys().add(U'T');

    ought(hotly(top, one, ui::Key{U'T'}));

    // Letters regardless of case.
    ought(hotly(top, one, ui::Key{U't'}));

    ought(!hotly(top, one, ui::Key{U'n'}));

    // There may be several keys, and they need not be letters.
    one->keys().add(std::vector<ui::Key>{U'T', ui::Code::f2});

    ought(hotly(top, one, ui::Key{U't'}));
    ought(hotly(top, one, ui::Key{ui::Code::f2}));
    ought(!hotly(top, one, ui::Key{ui::Code::f3}));

    // The set clears.
    one->keys().clear();

    ought(one->keys().empty());
    ought(!hotly(top, one, ui::Key{U't'}));

    // An empty key is not a key.
    one->keys().add(U'T');

    ought(!hotly(top, one, ui::Key{}));
}

// The key set answers the same four words as a node's children, and it folds case everywhere,
// not only when matching.
TRIAL("Button: the key set adds, removes and clears") {
    const auto one = std::make_shared<ui::Button>("Tak");

    ought(one->keys().empty());
    same(one->keys().many(), std::size_t{0});

    one->keys().add(U'T');
    one->keys().add(ui::Code::f2);

    same(one->keys().many(), std::size_t{2});
    ought(one->keys().contains(ui::Key{U'T'}));
    ought(one->keys().contains(ui::Key{U't'}));
    ought(one->keys().contains(ui::Key{ui::Code::f2}));
    ought(!one->keys().contains(ui::Key{U'n'}));
    ought(!one->keys().contains(ui::Key{}));

    // A letter ignores Shift (case folds), a coded key does not: Shift+F2 is a different key from
    // F2, as in MC, where Shift+F7 repeats a search and F7 starts one.
    ought(one->keys().contains(ui::Key{U't', ui::Mods{.shift = true}}));
    ought(!one->keys().contains(ui::Key{ui::Code::f2, ui::Mods{.shift = true}}));
    ought(!one->keys().contains(ui::Key{ui::Code::f2, ui::Mods{.ctrl = true}}));

    one->keys().add(ui::Key{ui::Code::f2, ui::Mods{.shift = true}});

    ought(one->keys().contains(ui::Key{ui::Code::f2, ui::Mods{.shift = true}}));
    same(one->keys().many(), std::size_t{3});

    one->keys().remove(ui::Key{ui::Code::f2, ui::Mods{.shift = true}});

    // An empty key does not go in, and the same key a second time adds nothing.
    one->keys().add(ui::Key{});
    one->keys().add(U't');

    same(one->keys().many(), std::size_t{2});

    // Removing ignores case too.
    one->keys().remove(ui::Key{U't'});

    same(one->keys().many(), std::size_t{1});
    ought(!one->keys().contains(ui::Key{U'T'}));

    // Removing something that is not there has no effect.
    one->keys().remove(ui::Key{U'z'});

    same(one->keys().many(), std::size_t{1});

    // The set can be iterated.
    int much = 0;

    for (const ui::Key which : one->keys()) {
        (void)which;

        ++much;
    }

    same(much, 1);
    same(static_cast<int>(one->keys().many()), 1);
    same(one->keys().front(), one->keys().back());
    same(one->keys()[0], one->keys().front());

    // Reaching past the end is the caller's error, just as in the set of options.
    threw((void)one->keys()[1]);

    one->keys().clear();

    threw((void)one->keys().front());

    ought(one->keys().empty());
}

TRIAL("Button: a disabled one does not answer its keys") {
    Top top;

    const auto one = std::make_shared<ui::Button>("Tak");

    top.children().add(one);

    one->keys().add(U'T');
    one->enabled(false);

    ought(!hotly(top, one, ui::Key{U'T'}));
}

// The marking is a consequence, not a separate setting: if the key's letter occurs in the label,
// it is underlined; if not, the button still answers that key.
TRIAL("Button: the key's letter is underlined in the label") {
    ui::Root top;

    const auto one = std::make_shared<ui::Button>("Przerwij");

    one->keys().add(U'w');

    top.children().add(one);

    const View made = top.render({{0, 0}, {12, 1}});

    same(read(made, 0), std::string{"[ Przerwij ]"});

    // "[ " is two cells, and "w" is the sixth letter of the label.
    ought(made.at({7, 0}).style.under);
    same(made.at({7, 0}).letter, U'w');

    ought(!made.at({6, 0}).style.under);
    ought(!made.at({8, 0}).style.under);
}

TRIAL("Button: the first occurrence is underlined, regardless of case") {
    ui::Root top;

    const auto one = std::make_shared<ui::Button>("Przerwij");

    // An upper-case key letter, a lower-case one in the label: it is still found, on the first "r".
    one->keys().add(U'R');

    top.children().add(one);

    const View made = top.render({{0, 0}, {12, 1}});

    ought(made.at({3, 0}).style.under);
    same(made.at({3, 0}).letter, U'r');

    // The second "r" stays plain.
    ought(!made.at({6, 0}).style.under);
}

// Case folds beyond basic Latin too: the Russian and Ukrainian menus of twa underline Cyrillic
// letters, and a shortcut with an upper-case letter must find the lower-case one in the label and
// answer both.
TRIAL("Button: case folds in Cyrillic and in Latin with diacritics") {
    Top top;

    const auto one = std::make_shared<ui::Button>("Карта");
    const auto two = std::make_shared<ui::Button>("Mżawka");

    one->keys().add(U'А');
    two->keys().add(U'Ż');

    top.children().add(one);

    const View made = top.render({{0, 0}, {9, 1}});

    same(made.at({3, 0}).letter, U'а');
    ought(made.at({3, 0}).style.under);
    ought(!made.at({2, 0}).style.under);

    ought(hotly(top, one, ui::Key{U'а'}));
    ought(hotly(top, one, ui::Key{U'А'}));
    ought(!hotly(top, one, ui::Key{U'a'}));

    top.children().clear();
    top.children().add(two);

    const View other = top.render({{0, 0}, {10, 1}});

    same(other.at({3, 0}).letter, U'ż');
    ought(other.at({3, 0}).style.under);

    ought(hotly(top, two, ui::Key{U'ż'}));
}

TRIAL("Button: a key not in the label marks nothing") {
    Top top;

    const auto one = std::make_shared<ui::Button>("Tak");

    one->keys().add(std::vector<ui::Key>{ui::Code::f2, U'z'});

    top.children().add(one);

    const View made = top.render({{0, 0}, {7, 1}});

    for (int x = 0; x < 7; ++x) ought(!made.at({x, 0}).style.under);

    // But it answers both.
    ought(hotly(top, one, ui::Key{ui::Code::f2}));
    ought(hotly(top, one, ui::Key{U'z'}));
}

TRIAL("Button: the first key that occurs in the label decides") {
    ui::Root top;

    const auto one = std::make_shared<ui::Button>("Przerwij");

    // There is no "q" in the label, so only "i" is marked.
    one->keys().add(std::vector<ui::Key>{U'q', U'i'});

    top.children().add(one);

    const View made = top.render({{0, 0}, {12, 1}});

    same(made.at({8, 0}).letter, U'i');
    ought(made.at({8, 0}).style.under);
}

TRIAL("Button: the default one has wider brackets") {
    ui::Root top;

    const auto one = std::make_shared<ui::Button>("Tak");

    top.children().add(one);

    same(one->wants({40, 4}), (ui::Size{7, 1}));
    same(read(top.render({{0, 0}, {7, 1}}), 0), std::string{"[ Tak ]"});

    one->primary(true);

    same(one->wants({40, 4}), (ui::Size{9, 1}));
    same(read(top.render({{0, 0}, {9, 1}}), 0), std::string{"[< Tak >]"});
}

TRIAL("Button: with the focus it is all in reverse video") {
    ui::Root top;

    const auto one = std::make_shared<ui::Button>("Tak");

    one->keys().add(U'T');

    top.children().add(one);

    ought(!top.render({{0, 0}, {7, 1}}).at({2, 0}).style.reverse);

    one->focus();

    const View made = top.render({{0, 0}, {7, 1}});

    ought(made.at({0, 0}).style.reverse);
    ought(made.at({2, 0}).style.reverse);

    // The underline stays with the focus too.
    ought(made.at({2, 0}).style.under);
}

// A button reports a height of one, and a row gives its children its own height, so painting
// must keep to one row by itself; otherwise the reverse video spreads over the whole layout row.
TRIAL("Button: paints one row, not as many as it is given") {
    ui::Root top;

    const auto row = std::make_shared<ui::Row>();
    const auto one = std::make_shared<ui::Button>("Tak");
    const auto tall = std::make_shared<Box>(ui::Size{4, 3});

    row->children().add(one);
    row->children().add(tall);
    top.children().add(row);

    one->focus();

    const View made = top.render({{0, 0}, {11, 3}});

    ought(made.at({0, 0}).style.reverse);

    // The second and third rows at the button's column do not belong to it.
    ought(!made.at({0, 1}).style.reverse);
    ought(!made.at({0, 2}).style.reverse);
}

// The bracket is what makes a button a button. When too narrow it cuts the label, not the
// bracket; a cut bracket would leave behind something that looks like plain text.
TRIAL("Button: when too narrow it cuts the label, not the bracket") {
    ui::Root top;

    const auto one = std::make_shared<ui::Button>("Przerwij");

    top.children().add(one);

    same(read(top.render({{0, 0}, {8, 1}}), 0), std::string{"[ Prz… ]"});
    same(read(top.render({{0, 0}, {5, 1}}), 0), std::string{"[ … ]"});
}

TRIAL("Button: Enter presses the focused one, space does not") {
    ui::Root top;

    int done = 0;

    const auto one = proving::doing(std::make_shared<ui::Button>("Tak"), [&done] { ++done; });

    top.children().add(one);

    one->focus();

    ui::KeyEvent enter{ui::Code::enter};

    ought(top.dispatch(enter));
    same(done, 1);

    // Space is not the button's business. That habit comes from a world other than Midnight
    // Commander, and it would take from the application a key that, in a window with a text field,
    // belongs to typing.
    ui::KeyEvent space{U' '};

    ought(!top.dispatch(space));
    same(done, 1);

    // A plain letter is not the button's business; the dialog takes care of that.
    ui::KeyEvent other{U'z'};

    ought(!top.dispatch(other));
    same(done, 1);
}

// Ctrl+Enter and Alt+Space are other shortcuts by design. The legend filters them out the same
// way.
TRIAL("Button: does not catch keys with a modifier") {
    ui::Root top;

    int done = 0;

    const auto one = proving::doing(std::make_shared<ui::Button>("Tak"), [&done] { ++done; });

    top.children().add(one);

    one->focus();

    ui::KeyEvent held{ui::Key{ui::Code::enter, ui::Mods{true, false, false}}};

    ought(!top.dispatch(held));

    ui::KeyEvent aside{ui::Key{U' ', ui::Mods{false, true, false}}};

    ought(!top.dispatch(aside));

    same(done, 0);
}

TRIAL("Button: a disabled one takes no focus and does not answer") {
    ui::Root top;

    int done = 0;

    const auto one = proving::doing(std::make_shared<ui::Button>("Tak"), [&done] { ++done; });

    one->enabled(false);

    top.children().add(one);

    ought(!one->focusable());

    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(nullptr));

    ought(top.render({{0, 0}, {7, 1}}).at({2, 0}).style.faint);

    one->pick();

    same(done, 0);
}

// Disabling gives up the focus. Without this the focus would stay on a node that is no longer
// focusable, breaking the very invariant this flag exists for.
TRIAL("Button: disabling gives up the focus") {
    ui::Root top;

    const auto one = std::make_shared<ui::Button>("Tak");

    top.children().add(one);

    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(one.get()));

    one->enabled(false);

    // Outside a dialog there is no focusable ancestor (the root takes no focus), so the focus
    // goes out entirely.
    same(top.focused_node(), static_cast<ui::Node*>(nullptr));

    // Disabling a button without the focus changes nothing.
    const auto other = std::make_shared<ui::Button>("Nie");

    top.children().remove(one.get());
    top.children().add(other);

    other->focus();
    one->enabled(true);

    same(top.focused_node(), static_cast<ui::Node*>(other.get()));
}

TRIAL("Button: a click takes the focus and presses") {
    ui::Root top;

    int done = 0;

    const auto one = proving::doing(std::make_shared<ui::Button>("Tak"), [&done] { ++done; });

    top.children().add(one);

    (void)top.render({{0, 0}, {7, 1}});

    ui::ClickEvent what{{3, 0}, ui::Mouse::left};

    ought(top.dispatch(what));

    same(done, 1);
    same(top.focused_node(), static_cast<ui::Node*>(one.get()));
}

namespace {

// A dialog in a modal with two buttons, "Tak" with the key T and "Nie" with the key N.
struct Both {
    std::shared_ptr<ui::Modals> window;
    std::shared_ptr<ui::Dialog> dialog;
    std::shared_ptr<ui::Button> first;
    std::shared_ptr<ui::Button> second;
};

// Opens a `Both` dialog in `top`; pressing the first button increments `*done`.
Both asking(ui::Root& top, int* done) {
    Both made;

    made.window = std::make_shared<ui::Modals>();
    made.dialog = std::make_shared<ui::Dialog>();
    made.first = proving::doing(std::make_shared<ui::Button>("Tak"), [done] { ++*done; });
    made.second = std::make_shared<ui::Button>("Nie");

    made.first->keys().add(U'T');
    made.second->keys().add(U'N');

    const auto column = std::make_shared<ui::Column>();

    column->children().add(made.first);
    column->children().add(made.second);
    made.dialog->children().add(column);

    top.children().add(made.window);
    made.window->open(made.dialog);

    (void)top.render({{0, 0}, {20, 10}});

    return made;
}

}

// The limit of the walk comes from modality. Tab bubbles up like every key, and the dialog
// handles it, so it never escapes the dialog.
TRIAL("Dialog: Tab walks the focusable nodes and wraps") {
    ui::Root top;

    int done = 0;

    const Both both = asking(top, &done);

    same(top.focused_node(), static_cast<ui::Node*>(both.first.get()));

    ui::KeyEvent tab{ui::Code::tab};

    ought(top.dispatch(tab));
    same(top.focused_node(), static_cast<ui::Node*>(both.second.get()));

    ought(top.dispatch(tab));
    same(top.focused_node(), static_cast<ui::Node*>(both.first.get()));
}

TRIAL("Dialog: Shift+Tab walks backwards") {
    ui::Root top;

    int done = 0;

    const Both both = asking(top, &done);

    ui::KeyEvent back{ui::Key{ui::Code::tab, ui::Mods{false, false, true}}};

    ought(top.dispatch(back));
    same(top.focused_node(), static_cast<ui::Node*>(both.second.get()));

    ought(top.dispatch(back));
    same(top.focused_node(), static_cast<ui::Node*>(both.first.get()));
}

// The down and up arrows walk like Tab and Shift+Tab: a dialog does not send them out as
// shortcuts, and the modal moves the focus with them.
TRIAL("Dialog: the down and up arrows walk like Tab") {
    ui::Root top;

    int done = 0;

    const Both both = asking(top, &done);

    ui::KeyEvent down{ui::Code::down};

    ought(top.dispatch(down));
    same(top.focused_node(), static_cast<ui::Node*>(both.second.get()));

    ui::KeyEvent up{ui::Code::up};

    ought(top.dispatch(up));
    same(top.focused_node(), static_cast<ui::Node*>(both.first.get()));
    same(done, 0);
}

TRIAL("Dialog: Tab does not leave the dialog") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto outside = std::make_shared<Box>(ui::Size{10, 3});
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto one = std::make_shared<ui::Button>("Tak");

    dialog->children().add(one);
    window->content(outside);
    top.children().add(window);
    window->open(dialog);

    (void)top.render({{0, 0}, {20, 10}});

    ui::KeyEvent tab{ui::Code::tab};

    for (int turn = 0; turn < 4; ++turn) {
        ought(top.dispatch(tab));

        // The only focusable node in the dialog, so the focus does not leave it, and certainly
        // not for what lies below.
        same(top.focused_node(), static_cast<ui::Node*>(one.get()));
    }
}

// Marking a button as the default one is only drawing. The window searches the tree for nobody
// and presses nothing on the caller's behalf: Enter belongs to whoever has the focus, and when the
// focus is off the buttons, Enter presses nothing. Whoever wants "Enter submits the form" hangs a
// handler where the focus really is, on the field, and knows what should happen then.
TRIAL("Dialog: Enter does not press the default button when the focus is off the buttons") {
    ui::Root top;

    int done = 0;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto aside = std::make_shared<Box>(ui::Size{6, 1});
    const auto one = proving::doing(std::make_shared<ui::Button>("Tak"), [&done] { ++done; });

    one->primary(true);

    const auto column = std::make_shared<ui::Column>();

    column->children().add(aside);
    column->children().add(one);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    // The focus is put off the buttons, as it stands in a text field.
    aside->focus();

    ui::KeyEvent enter{ui::Code::enter};

    // The modal still swallows everything nobody took; modality stays.
    ought(top.dispatch(enter));
    same(done, 0);
}

// And when the focus is on a button, Enter presses that button. The button takes the key first,
// because it lies nearer the focus, and that is the only way Enter presses anything.
TRIAL("Dialog: Enter on a button presses that one, not the default") {
    ui::Root top;

    int done = 0;

    const Both both = asking(top, &done);

    int other = 0;

    (void)both.second->on<ui::PickEvent>([&other](ui::PickEvent&) {
        ++other;

        return true;
    });
    both.first->primary(true);

    both.second->focus();

    ui::KeyEvent enter{ui::Code::enter};

    ought(top.dispatch(enter));

    same(other, 1);
    same(done, 0);
}

// Opening puts the focus on the first focusable node, not on the default button. Otherwise, in a
// window that starts with a text field, the first letter typed could be a button's shortcut key
// and close the window with an empty field. What Enter presses is a separate matter, guarded by
// the trials nearby.
TRIAL("Dialog: opening lands on the first focusable node") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto first = std::make_shared<ui::Button>("Tak");
    const auto second = std::make_shared<ui::Button>("Nie");

    second->primary(true);

    const auto column = std::make_shared<ui::Column>();

    column->children().add(first);
    column->children().add(second);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    same(top.focused_node(), static_cast<ui::Node*>(first.get()));
}

// This is what the rule is for: a window with a text field must start with the focus in the
// field, even with the default button right beside it.
TRIAL("Dialog: opening with a text field lands on the field") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto field = std::make_shared<ui::Field>(20);
    const auto yes = std::make_shared<ui::Button>("Szukaj");

    yes->primary(true);

    const auto column = std::make_shared<ui::Column>();

    column->children().add(field);
    column->children().add(yes);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    same(top.focused_node(), static_cast<ui::Node*>(field.get()));

    // Enter stays with the focus: the button marked as default is not pressed because of it.
    int done = 0;

    (void)yes->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });

    ui::KeyEvent enter{ui::Code::enter};

    ought(top.dispatch(enter));
    same(done, 0);
}

TRIAL("Dialog: a shortcut key presses from anywhere in the window") {
    ui::Root top;

    int done = 0;

    const Both both = asking(top, &done);

    // The focus is on "Tak", and its key is pressed as a lower-case letter.
    ui::KeyEvent small{U't'};

    ought(top.dispatch(small));
    same(done, 1);

    ui::KeyEvent large{U'T'};

    ought(top.dispatch(large));
    same(done, 2);

    // A key no button has does nothing, but the dialog swallows it anyway.
    ui::KeyEvent other{U'z'};

    ought(top.dispatch(other));
    same(done, 2);
}

TRIAL("Dialog: a disabled button drops out of the walk and the keys") {
    ui::Root top;

    int done = 0;

    const Both both = asking(top, &done);

    both.first->enabled(false);

    // The focus moved from the disabled button to the nearest focusable ancestor, the dialog
    // itself. If it went out entirely, keys would go straight to the root, and the modal window
    // would stop being modal.
    same(top.focused_node(), static_cast<ui::Node*>(both.dialog.get()));

    ui::KeyEvent tab{ui::Code::tab};

    ought(top.dispatch(tab));
    same(top.focused_node(), static_cast<ui::Node*>(both.second.get()));

    ui::KeyEvent small{U't'};

    ought(top.dispatch(small));
    same(done, 0);
}

// When nothing in the dialog takes the focus, the dialog takes it itself, and then it swallows
// the keys instead of leaving them to the application below.
TRIAL("Dialog: with nothing focusable inside, the focus stays on the dialog") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();

    dialog->children().add(std::make_shared<ui::Column>());

    top.children().add(window);
    window->open(dialog);

    same(top.focused_node(), static_cast<ui::Node*>(dialog.get()));

    ui::KeyEvent tab{ui::Code::tab};

    ought(top.dispatch(tab));
    same(top.focused_node(), static_cast<ui::Node*>(dialog.get()));
}
