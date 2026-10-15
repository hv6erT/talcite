#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/modals.hpp"
#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/tick.hpp"

using namespace talcite;

namespace {

// Reads row `y` of a drawn view as UTF-8 text, dropping trailing spaces.
std::string read(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) utf8::encode(one.at({x, y}).letter, made);

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

// Counts in `done` every `ui::PickEvent` that `one` sends, while the returned hook is kept.
ui::Hook counted(const std::shared_ptr<ui::Tick>& one, int& done) {
    return one->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });
}

}

// --- What a tick box knows about itself ----------------------------------------------------------

TRIAL("Tick: a fresh one is empty and willing to take focus") {
    const ui::Tick one{"Zawijaj"};

    ought(!one.ticked());
    ought(one.enabled());
    ought(one.focusable());
    same(one.capacity(), 0);
    same(one.text(), std::string{"Zawijaj"});
}

TRIAL("Tick: the state can be given at construction") {
    const ui::Tick one{"Zawijaj", true};

    ought(one.ticked());
}

// Four cells for the bracket and a space, the rest for the text: one row, not as many as given.
TRIAL("Tick: wants the bracket plus the text and one row") {
    const ui::Tick one{"Zawijaj"};

    same(one.wants({40, 5}), (ui::Size{11, 1}));
    same(one.wants({6, 5}), (ui::Size{6, 1}));
}

// --- Drawing -------------------------------------------------------------------------------------

TRIAL("Tick: draws the bracket with the state and the text") {
    ui::Root top;

    const auto one = std::make_shared<ui::Tick>("Zawijaj");

    top.children().add(one);

    same(read(top.render({{0, 0}, {11, 1}}), 0), std::string{"[ ] Zawijaj"});

    one->ticked(true);

    same(read(top.render({{0, 0}, {11, 1}}), 0), std::string{"[x] Zawijaj"});
}

// A box that is too narrow cuts the text, not the bracket: the bracket is what makes it a tick box.
TRIAL("Tick: when too narrow it cuts the text, not the bracket") {
    ui::Root top;

    const auto one = std::make_shared<ui::Tick>("Zawijaj");

    top.children().add(one);

    const std::string made = read(top.render({{0, 0}, {7, 1}}), 0);

    same(made.substr(0, 4), std::string{"[ ] "});
    same(made.size(), std::size_t{4 + 3 + 2});
}

TRIAL("Tick: has its own role when focused") {
    ui::Root top;

    const auto one = std::make_shared<ui::Tick>("Zawijaj");

    top.children().add(one);

    ought(!top.render({{0, 0}, {11, 1}}).at({0, 0}).style.reverse);

    one->focus();

    ought(top.render({{0, 0}, {11, 1}}).at({0, 0}).style.reverse);
}

// --- Toggling ------------------------------------------------------------------------------------

// Setting it from the program does not notify, because filling a window with initial values is not
// pressing. Toggling through `pick` notifies.
TRIAL("Tick: setting the state is silent, toggling notifies") {
    ui::Root top;

    const auto one = std::make_shared<ui::Tick>("Zawijaj");

    top.children().add(one);

    int done = 0;

    const ui::Hook counting = counted(one, done);

    one->ticked(true);

    same(done, 0);

    one->pick();

    ought(!one->ticked());
    same(done, 1);
}

TRIAL("Tick: space toggles, and Enter goes up") {
    ui::Root top;

    const auto one = std::make_shared<ui::Tick>("Zawijaj");

    top.children().add(one);

    one->focus();

    int done = 0;

    const ui::Hook counting = counted(one, done);

    ui::KeyEvent space{U' '};

    ought(top.dispatch(space));
    ought(one->ticked());
    same(done, 1);

    ui::KeyEvent enter{ui::Code::enter};

    ought(!top.dispatch(enter));
    ought(one->ticked());
    same(done, 1);
}

TRIAL("Tick: a click takes focus and toggles") {
    ui::Root top;

    const auto one = std::make_shared<ui::Tick>("Zawijaj");

    top.children().add(one);

    (void)top.render({{0, 0}, {11, 1}});

    int done = 0;

    const ui::Hook counting = counted(one, done);

    ui::ClickEvent what{{2, 0}, ui::Mouse::left};

    ought(top.dispatch(what));
    ought(one->ticked());
    same(done, 1);
    same(top.focused_node(), static_cast<ui::Node*>(one.get()));
}

// --- Disabled ------------------------------------------------------------------------------------

TRIAL("Tick: when disabled it does not toggle and does not take focus") {
    ui::Root top;

    const auto one = std::make_shared<ui::Tick>("Zawijaj");

    top.children().add(one);

    one->enabled(false);

    ought(!one->focusable());

    int done = 0;

    const ui::Hook counting = counted(one, done);

    one->pick();

    ought(!one->ticked());
    same(done, 0);
}

// A disabled box must not keep focus: focus would get stuck on something Tab no longer leaves,
// because the walk does not see disabled widgets.
TRIAL("Tick: disabling gives up focus") {
    ui::Root top;

    const auto one = std::make_shared<ui::Tick>("Zawijaj");

    top.children().add(one);

    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(one.get()));

    one->enabled(false);

    ought(top.focused_node() != static_cast<ui::Node*>(one.get()));
}

// --- Hotkey --------------------------------------------------------------------------------------

// A tick box answers its letter from anywhere in the window, the same way a button does, because
// the window hands out the key through an event instead of asking about the type.
TRIAL("Tick: the hotkey works from anywhere in the window") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto one = std::make_shared<ui::Tick>("Zawijaj");
    const auto other = std::make_shared<ui::Tick>("Wielkość liter");

    one->keys().add(U'Z');
    other->keys().add(U'W');

    column->children().add(one);
    column->children().add(other);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    // Focus stands on the first box, and we press the letter of the second.
    same(top.focused_node(), static_cast<ui::Node*>(one.get()));

    ui::KeyEvent what{U'w'};

    ought(top.dispatch(what));

    ought(!one->ticked());
    ought(other->ticked());
}

TRIAL("Tick: when disabled it does not answer its key") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto one = std::make_shared<ui::Tick>("Zawijaj");

    one->keys().add(U'Z');

    column->children().add(one);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    one->enabled(false);

    ui::KeyEvent what{U'z'};

    (void)top.dispatch(what);

    ought(!one->ticked());
}
