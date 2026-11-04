#include "proving.hpp"

#include <memory>
#include <string>
#include <vector>

#include "talcite/choice.hpp"
#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/modals.hpp"
#include "talcite/encoding.hpp"
#include "talcite/root.hpp"

using namespace talcite;

namespace {

// The text of row `y` of the view, without trailing spaces.
std::string read(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter == 0) continue;

        made += spelled(letter);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

// Three options to choose from.
std::vector<std::string> three() { return {"Nazwa", "Rozszerzenie", "Rozmiar"}; }

// Counts in `done` every time `one` notifies about a pick, while the returned hook is kept.
ui::Hook counted(const std::shared_ptr<ui::Choice>& one, int& done) {
    return one->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });
}

}

// --- What the group knows about itself ----------------------------------------------------------

TRIAL("Choice: an empty one does not want the focus") {
    const ui::Choice one;

    same(one.options().many(), std::size_t{0});
    ought(!one.focusable());
    same(one.capacity(), 0);
}

TRIAL("Choice: a fresh one stands on the first option") {
    const ui::Choice one{three()};

    same(one.options().many(), std::size_t{3});
    same(*one.value(), std::string{"Nazwa"});
    ought(one.focusable());
}

TRIAL("Choice: the choice can be given at creation and is clamped") {
    same(*ui::Choice(three(), 2).value(), std::string{"Rozmiar"});
    same(*ui::Choice(three(), 9).value(), std::string{"Rozmiar"});
}

// `which` is the index, `value` is the text: the same chosen option under two names, like the index
// and the node in a list. The index setter clamps, the text setter ignores an unknown text.
TRIAL("Choice: which is the index of the chosen option, value its text") {
    ui::Choice one{three()};

    same(one.which(), std::optional<std::size_t>{0});

    one.which(2);

    same(one.which(), std::optional<std::size_t>{2});
    same(*one.value(), std::string{"Rozmiar"});

    one.which(9);

    same(one.which(), std::optional<std::size_t>{2});

    one.value("Nazwa");

    same(one.which(), std::optional<std::size_t>{0});

    one.which(1);
    one.value("nie ma takiej");

    same(one.which(), std::optional<std::size_t>{1});

    ui::Choice empty;

    empty.which(1);

    ought(!empty.which().has_value());
    ought(!empty.value().has_value());

    // An index beyond an empty group is not remembered for later.
    empty.options().add("raz");
    empty.options().add("dwa");

    same(empty.which(), std::optional<std::size_t>{0});
}

// A clamped index is remembered clamped, and a set that shrank under the chosen option gives the
// last one, not an index beyond the set.
TRIAL("Choice: the index is clamped when set and when read") {
    ui::Choice one{three()};

    one.which(9);
    one.options().add("Czas");

    same(one.which(), std::optional<std::size_t>{2});

    one.which(3);
    one.options().remove(3);

    same(one.which(), std::optional<std::size_t>{2});
    same(*one.value(), std::string{"Rozmiar"});
}

// Four cells for the bracket and the space plus the longest text; the height is the number of
// options.
TRIAL("Choice: wants the bracket, the longest text and a row per option") {
    const ui::Choice one{three()};

    same(one.wants({40, 10}), (ui::Size{16, 3}));
    same(one.wants({8, 2}), (ui::Size{8, 2}));
}

// --- Drawing -------------------------------------------------------------------------------------

TRIAL("Choice: the dot stands at the chosen option") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three(), 1);

    top.children().add(one);

    const View made = top.render({{0, 0}, {16, 3}});

    same(read(made, 0), std::string{"( ) Nazwa"});
    same(read(made, 1), std::string{"(•) Rozszerzenie"});
    same(read(made, 2), std::string{"( ) Rozmiar"});
}

// The highlight only with the focus: otherwise a window without the focus in the group would look
// as if the focus were on it.
TRIAL("Choice: the highlight shows only with the focus") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three(), 1);

    top.children().add(one);

    ought(!top.render({{0, 0}, {16, 3}}).at({0, 1}).style.reverse);

    one->focus();

    const View made = top.render({{0, 0}, {16, 3}});

    ought(made.at({0, 1}).style.reverse);
    ought(!made.at({0, 0}).style.reverse);
    ought(!made.at({0, 2}).style.reverse);
}

// --- Movement ------------------------------------------------------------------------------------

// Movement is choosing: an arrow moves the selection at once, not a separate cursor.
TRIAL("Choice: the arrows change the choice and notify") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three());

    top.children().add(one);
    one->focus();

    int done = 0;

    const ui::Hook counting = counted(one, done);

    ui::KeyEvent down{ui::Code::down};

    ought(top.dispatch(down));
    same(*one->value(), std::string{"Rozszerzenie"});
    same(done, 1);

    ought(top.dispatch(down));
    same(*one->value(), std::string{"Rozmiar"});
    same(done, 2);

    ui::KeyEvent up{ui::Code::up};

    ought(top.dispatch(up));
    same(*one->value(), std::string{"Rozszerzenie"});
    same(done, 3);
}

// At the edge of the group the key goes up: in a window the arrow should then move to the next
// widget, not get stuck.
TRIAL("Choice: at the edge it passes the arrow up") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three());

    top.children().add(one);
    one->focus();

    ui::KeyEvent up{ui::Code::up};

    ought(!top.dispatch(up));
    same(*one->value(), std::string{"Nazwa"});

    one->value("Rozmiar");

    ui::KeyEvent down{ui::Code::down};

    ought(!top.dispatch(down));
    same(*one->value(), std::string{"Rozmiar"});
}

TRIAL("Choice: Home and End jump to the edges") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three());

    top.children().add(one);
    one->focus();

    ui::KeyEvent end{ui::Code::end};

    ought(top.dispatch(end));
    same(*one->value(), std::string{"Rozmiar"});

    ui::KeyEvent home{ui::Code::home};

    ought(top.dispatch(home));
    same(*one->value(), std::string{"Nazwa"});
}

// Home on the first option and End on the last change nothing, so the key goes up, as an arrow at
// the edge does.
TRIAL("Choice: Home and End at the edge they lead to pass the key up") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three());

    int done = 0;

    const ui::Hook heard = counted(one, done);

    top.children().add(one);
    one->focus();

    ui::KeyEvent home{ui::Code::home};

    ought(!top.dispatch(home));
    same(*one->value(), std::string{"Nazwa"});

    one->value("Rozmiar");

    ui::KeyEvent end{ui::Code::end};

    ought(!top.dispatch(end));
    same(*one->value(), std::string{"Rozmiar"});
    same(done, 0);
}

// Setting from the program does not notify, just as in a check box.
TRIAL("Choice: setting the choice is silent") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three());

    top.children().add(one);

    int done = 0;

    const ui::Hook counting = counted(one, done);

    one->value("Rozmiar");

    same(*one->value(), std::string{"Rozmiar"});
    same(done, 0);
}

TRIAL("Choice: Enter and letters go up") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three());

    top.children().add(one);
    one->focus();

    ui::KeyEvent enter{ui::Code::enter};

    ought(!top.dispatch(enter));

    ui::KeyEvent letter{U'z'};

    ought(!top.dispatch(letter));
}

// --- Click ---------------------------------------------------------------------------------------

TRIAL("Choice: a click chooses its option and takes the focus") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three());

    top.children().add(one);

    (void)top.render({{0, 0}, {16, 3}});

    int done = 0;

    const ui::Hook counting = counted(one, done);

    ui::ClickEvent what{{6, 2}, ui::Mouse::left};

    ought(top.dispatch(what));
    same(*one->value(), std::string{"Rozmiar"});
    same(done, 1);
    same(top.focused_node(), static_cast<ui::Node*>(one.get()));

    // A click on the option already chosen is not a change.
    ought(top.dispatch(what));
    same(done, 1);
}

// --- In a window ---------------------------------------------------------------------------------

// The group is one stop of the walk: Tab enters it once and leaves it once, and the arrows move
// inside.
TRIAL("Choice: in a window it is one Tab stop") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto one = std::make_shared<ui::Choice>(three());
    const auto other = std::make_shared<ui::Choice>(three());

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

TRIAL("Choice: a disabled one does not choose and gives up the focus") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three());

    top.children().add(one);
    one->focus();

    one->enabled(false);

    ought(!one->focusable());
    ought(top.focused_node() != static_cast<ui::Node*>(one.get()));

    ui::KeyEvent down{ui::Code::down};

    (void)top.dispatch(down);

    same(*one->value(), std::string{"Nazwa"});
}

// The set of options has no pointer to its owner, so emptying it cannot notify the group. The
// focus therefore leaves at the next drawing, but it does leave.
TRIAL("Choice: emptied while focused, it gives up the focus when drawn") {
    ui::Root top;

    const auto one = std::make_shared<ui::Choice>(three());

    top.children().add(one);
    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(one.get()));

    one->options().clear();

    ought(!one->focusable());
    same(top.focused_node(), static_cast<ui::Node*>(one.get()));

    (void)top.render({{0, 0}, {16, 3}});

    ought(top.focused_node() != static_cast<ui::Node*>(one.get()));
}
