#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "tal/action.hpp"
#include "tal/controls.hpp"
#include "tal/document.hpp"
#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/key.hpp"
#include "tal/layout.hpp"
#include "tal/node.hpp"
#include "tal/scene.hpp"
#include "tal/style.hpp"
#include "tal/window.hpp"

#include "tal/testing.hpp"

// Windows in their layers on a scene: opening and closing with the focus going in and coming back,
// modal windows confining the focus and the mouse, keys bound to a window, messages and forms,
// shadows, and modeless windows raised and stepped through.

namespace {

using namespace std::chrono_literals;

using tal::Action;
using tal::Area;
using tal::Closed;
using tal::Code;
using tal::Document;
using tal::Form;
using tal::Hooks;
using tal::Key;
using tal::KeyEvent;
using tal::Label;
using tal::Layers;
using tal::Limits;
using tal::Message;
using tal::Modal;
using tal::Mods;
using tal::Node;
using tal::Run;
using tal::Size;
using tal::Spot;
using tal::Window;
using tal::testing::Scene;

// Whether `what` throws an exception of type E.
template <typename E, typename F>
bool throws(F what) {
    try {
        what();
    } catch (const E&) {
        return true;
    }

    return false;
}

// A node that takes the focus and the plain letters it is given, as a text field does.
class Field : public Node {
public:
    Field() {
        focusable(true);

        hooks_.add(otherwise<KeyEvent>([this](KeyEvent& what) {
            if (what.key.letter() == 0 || what.key.mods() != Mods{}) return;

            heard += static_cast<char>(what.key.letter());
            what.take();
        }));
    }

    std::string heard;

protected:
    // A few cells of one row.
    Size measure(Limits room) const override {
        (void)room;

        return Size{5, 1};
    }

private:
    Hooks hooks_;
};

// A program's screen on a scene: layers holding a full window without a frame, with two buttons in
// a row, and a record of the actions run.
struct Program {
    Scene scene;
    Document document{scene.time()};
    Layers* layers = nullptr;
    Window* screen = nullptr;
    tal::Button* first = nullptr;
    tal::Button* second = nullptr;
    std::shared_ptr<Action> left = std::make_shared<Action>("Left");
    std::shared_ptr<Action> right = std::make_shared<Action>("Right");
    std::vector<std::string> ran;
    Hooks hooks;

    // Builds the screen, shows it and focuses its first button.
    explicit Program(Size size = {30, 10}) : scene(size) {
        layers = &document.children().add<Layers>();

        auto made = std::make_shared<Window>();

        made->add_class("screen");
        made->full = true;

        screen = &layers->open(made);

        tal::Row& row = screen->children().add<tal::Row>(1);

        first = &row.children().add<tal::Button>(left);
        second = &row.children().add<tal::Button>(right);

        heard(left);
        heard(right);

        scene.show(document);
    }

    // Records the runs of `one` by its text.
    void heard(const std::shared_ptr<Action>& one) {
        hooks.add(one->on<Run>([this](Run& what) { ran.push_back(what.action.text()); }));
    }

    // A new action shown as `text` with `letter`, whose runs are recorded.
    std::shared_ptr<Action> action(std::string text, std::optional<char32_t> letter = std::nullopt) {
        auto made = std::make_shared<Action>(std::move(text), letter);

        heard(made);

        return made;
    }

    // Opens a modal form titled `title` with the answers `answers` and lets the scene lay it out.
    Form& form(std::string title, const std::vector<std::shared_ptr<Action>>& answers) {
        auto made = std::make_shared<Form>(std::move(title));

        for (const auto& one : answers) made->answer(one);

        Form& opened = static_cast<Form&>(layers->open(made));

        scene.wait(0ms);

        return opened;
    }
};

// The focus goes into the screen's first button when the program starts; a modal form opened on top
// takes it into its first answer once laid out, and Esc closes it and gives it back.
TRIAL("Layers: a modal window takes the focus in and gives it back when it closes") {
    Program program;

    OUGHT(program.first->focused());
    OUGHT(program.layers->top() == program.screen);

    int closed = 0;
    Form& form = program.form("Ask", {program.action("Yes", U'y'), program.action("No", U'n')});

    program.hooks.add(form.on<Closed>([&closed](Closed&) { ++closed; }));

    OUGHT(program.layers->top() == &form);
    OUGHT(program.document.confined() == &form);
    OUGHT(!program.first->focused());
    const auto* answer = dynamic_cast<const tal::Button*>(program.document.focused());

    OUGHT(answer != nullptr && answer->action().text() == "Yes");

    program.scene.press(Key{Code::escape});

    SAME(closed, 1);
    OUGHT(program.layers->top() == program.screen);
    OUGHT(program.document.confined() == nullptr);
    OUGHT(program.first->focused());
    OUGHT(program.ran.empty());
}

// Tab goes round inside a modal window; the mouse outside it goes to the window itself, as to a
// curtain: the press, the release and the click, and the hover, while nothing beneath hears it.
TRIAL("Layers: a modal window keeps Tab and the mouse inside") {
    Program program;
    auto yes = program.action("Yes");
    auto no = program.action("No");

    Form& form = program.form("Ask", {yes, no});
    std::vector<tal::Press> curtain;

    program.hooks.add(form.on<tal::MouseEvent>([&curtain, &form](tal::MouseEvent& what) {
        if (&what.target() == &form) curtain.push_back(what.press);
    }));

    const Node* start = program.document.focused();

    program.scene.press(Key{Code::tab});

    const Node* then = program.document.focused();

    program.scene.press(Key{Code::tab});

    OUGHT(then != start);
    OUGHT(program.document.focused() == start);

    program.scene.click({1, 0});
    program.scene.point({1, 0});

    OUGHT(program.ran.empty());
    OUGHT(!program.first->states().has(tal::State::hover));
    OUGHT(form.states().has(tal::State::hover));
    OUGHT(curtain == (std::vector<tal::Press>{tal::Press::down, tal::Press::up, tal::Press::click, tal::Press::move}));

    program.scene.press(Key{Code::enter});

    SAME(program.ran.size(), std::size_t{1});
    SAME(program.ran.front(), std::string{"Yes"});
}

// A node beneath a modal window that asks for the focus waits; it gets the focus when the window
// closes, before the place the focus came from, the last one to ask winning.
TRIAL("Layers: a request for the focus from beneath waits for the modal window to close") {
    Program program;
    Form& form = program.form("Ask", {program.action("Ok")});

    program.first->focus();
    program.second->focus();

    OUGHT(!program.second->focused());

    form.close();

    OUGHT(program.second->focused());

    Form& again = program.form("Again", {program.action("Ok")});

    again.close();

    OUGHT(program.second->focused());
}

// Keys and pastes go to the modal window while nothing inside has the focus, and the caret of a
// focus left outside does not show.
TRIAL("Document: a confined focus sends keys to the confinement") {
    Program program;
    auto made = std::make_shared<Window>("Empty", Modal::yes);
    std::vector<std::string> heard;

    program.hooks.add(made->on<KeyEvent>([&heard](KeyEvent& what) { heard.push_back(std::string(1, static_cast<char>(what.key.letter()))); }));
    program.hooks.add(made->on<tal::PasteEvent>([&heard](tal::PasteEvent& what) { heard.push_back(what.text); }));

    Window& empty = program.layers->open(made);

    program.scene.wait(0ms);
    program.scene.press(Key{U'q'});
    program.scene.paste("pasted");

    OUGHT(heard == (std::vector<std::string>{"q", "pasted"}));
    OUGHT(program.first->focused());
    OUGHT(throws<std::logic_error>([&program] {
        Window stranger;

        program.document.confine(&stranger);
    }));

    empty.close();

    OUGHT(program.first->focused());
}

// A window's bound action runs on its shortcut and on its letter, plain or with Alt, but not with
// Ctrl nor when disabled; a node inside that takes the key goes first.
TRIAL("Window: bound actions run on their shortcuts and letters") {
    Program program;
    auto save = std::make_shared<Action>("Save", U's', Key{Code::f2});
    auto gone = program.action("Gone", U'g');

    program.heard(save);
    gone->enable(false);

    auto made = std::make_shared<Window>("Edit", Modal::yes);
    Field& field = made->children().add<Field>();

    made->bind(save);
    made->bind(gone);
    made->bind(save);

    (void)program.layers->open(made);

    program.scene.wait(0ms);

    OUGHT(field.focused());

    program.scene.press(Key{Code::f2});
    program.scene.press(Key{U's', Mods{false, true, false}});
    program.scene.press(Key{U's', Mods{true, false, false}});
    program.scene.press(Key{U'g', Mods{false, true, false}});
    program.scene.press(Key{U'x'});
    program.scene.press(Key{U's'});

    OUGHT(program.ran == (std::vector<std::string>{"Save", "Save"}));
    SAME(field.heard, std::string{"xs"});
    OUGHT(throws<std::invalid_argument>([&made] { made->bind(nullptr); }));
}

// A message shows its text over its answers; an answer runs and closes it, unless a handler takes
// the run; Esc closes it without an answer.
TRIAL("Message: an answer runs and closes it, Esc closes without one") {
    Program program;
    auto yes = program.action("Yes", U'y');
    auto no = program.action("No", U'n');
    auto made = std::make_shared<Message>("Delete", "Delete the file?", std::vector{yes, no});
    Message& message = static_cast<Message&>(program.layers->open(made));

    program.scene.wait(0ms);

    OUGHT(program.scene.where("Delete the file?").has_value());
    OUGHT(program.scene.where("[ Yes ]").has_value());
    OUGHT(program.scene.where(" Delete ").has_value());
    OUGHT(message.has_class("message"));

    program.scene.press(Key{U'n'});

    OUGHT(program.ran == (std::vector<std::string>{"No"}));
    OUGHT(program.layers->top() == program.screen);
    OUGHT(!program.scene.where("Delete the file?").has_value());

    program.hooks.add(yes->on<Run>([](Run& what) { what.take(); }));

    auto held = std::make_shared<Message>("Hold", "Stay?", std::vector{yes});

    (void)program.layers->open(held);

    program.scene.wait(0ms);
    program.scene.press(Key{Code::enter});

    OUGHT(program.layers->top() == held.get());

    program.scene.press(Key{Code::escape});

    OUGHT(program.layers->top() == program.screen);
    OUGHT(program.ran == (std::vector<std::string>{"No", "Yes"}));
}

// A form stands in the middle in the size of its content, with a double frame and its title on the
// top edge, and casts a shadow two columns right and a row down. Closed, it leaves the screen as it
// was.
TRIAL("Form: in the middle with its frame, its title and its shadow") {
    Program program{Size{30, 10}};
    auto ok = program.action("Ok");
    auto made = std::make_shared<Form>("Ask");

    (void)made->body().children().add<Label>("Sure?");
    made->answer(ok);

    const std::string before = program.scene.text();
    Form& form = static_cast<Form&>(program.layers->open(made));

    program.scene.wait(0ms);

    OUGHT(form.area() == (Area{{11, 2}, {8, 5}}));
    SAME(program.scene.at({11, 2}).cluster, std::string{"\xe2\x95\x94"});
    OUGHT(program.scene.where(" Ask ") == (Spot{12, 2}));
    SAME(program.scene.at({12, 3}).cluster, std::string{"S"});
    OUGHT(program.scene.where("[ Ok ]") == (Spot{12, 5}));
    OUGHT(program.scene.at({19, 3}).look.faint);
    OUGHT(program.scene.at({20, 6}).look.faint);
    OUGHT(program.scene.at({13, 7}).look.faint);
    OUGHT(program.scene.at({20, 7}).look.faint);
    OUGHT(!program.scene.at({19, 2}).look.faint);
    OUGHT(!program.scene.at({12, 7}).look.faint);
    OUGHT(!program.scene.at({21, 4}).look.faint);

    form.close();
    program.scene.wait(0ms);

    SAME(program.scene.text(), before);
    OUGHT(!program.scene.at({19, 3}).look.faint);
}

// The shadow keeps the letters under it, and a repaint of what lies only under the shadow keeps it.
TRIAL("Form: its shadow dims the letters under it and survives their repaint") {
    Program program{Size{30, 5}};
    auto beneath = std::make_shared<Window>();

    beneath->add_class("screen");
    beneath->seat(Area{{6, 1}, {2, 1}});

    Label& letters = beneath->children().add<Label>("LL");

    (void)program.layers->open(beneath);

    auto made = std::make_shared<Form>("");

    made->seat(Area{{0, 0}, {6, 3}});

    (void)program.layers->open(made);

    program.scene.wait(0ms);

    SAME(program.scene.at({6, 1}).cluster, std::string{"L"});
    OUGHT(program.scene.at({6, 1}).look.faint);
    OUGHT(!program.scene.at({6, 0}).look.faint);

    letters.text = "YY";
    program.scene.wait(0ms);

    SAME(program.scene.at({6, 1}).cluster, std::string{"Y"});
    OUGHT(program.scene.at({6, 1}).look.faint);
    OUGHT(program.scene.at({7, 1}).look.faint);
}

// A window wants its content and room for its title on the frame; a full one takes all the layers,
// a seated one its seat from the layers' corner.
TRIAL("Window: its size, a full one and a seated one") {
    Program program{Size{40, 12}};
    auto made = std::make_shared<Window>("A rather long title");
    Window& window = program.layers->open(made);

    (void)window.children().add<Label>("ab");

    program.scene.wait(0ms);

    OUGHT(window.measured(Limits{{0, 0}, {40, 12}}) == (Size{23, 3}));

    window.seat(Area{{2, 1}, {8, 4}});
    program.scene.wait(0ms);

    OUGHT(window.area() == (Area{{2, 1}, {8, 4}}));
    OUGHT(window.seat() == (std::optional<Area>{Area{{2, 1}, {8, 4}}}));

    window.full = true;
    program.scene.wait(0ms);

    OUGHT(window.area() == (Area{{0, 0}, {40, 12}}));

    window.full = false;
    window.seat(std::nullopt);
    program.scene.wait(0ms);

    OUGHT(window.area() == (Area{{8, 4}, {23, 3}}));
    OUGHT(program.screen->measured(Limits{{0, 0}, {40, 12}}).wide < 23);
}

// Modeless windows stand over each other in raising order and under every modal one; a press in a
// lower one raises it, and each keeps its own focus. Next and previous go in opening order.
TRIAL("Layers: modeless windows raised by a press, each keeping its focus") {
    Program program{Size{40, 12}};
    auto a = std::make_shared<Window>("A");
    auto b = std::make_shared<Window>("B");

    a->seat(Area{{0, 1}, {20, 5}});
    b->seat(Area{{10, 3}, {20, 5}});

    tal::Column& in_a = a->children().add<tal::Column>();
    tal::Button& a1 = in_a.children().add<tal::Button>(program.action("A1"));
    tal::Button& a2 = in_a.children().add<tal::Button>(program.action("A2"));

    (void)b->children().add<tal::Button>(program.action("B1"));

    (void)program.layers->open(a);
    program.scene.wait(0ms);

    OUGHT(a1.focused());

    a2.focus();

    (void)program.layers->open(b);
    program.scene.wait(0ms);

    OUGHT(program.layers->top() == b.get());
    OUGHT(!a2.focused());

    program.scene.click({0, 1});

    OUGHT(program.layers->top() == a.get());
    OUGHT(a2.focused());

    program.layers->next();
    program.scene.wait(0ms);

    OUGHT(program.layers->top() == b.get());

    program.layers->previous();
    program.scene.wait(0ms);
    program.layers->previous();
    program.scene.wait(0ms);

    OUGHT(program.layers->top() == program.screen);

    program.layers->raise(*a);
    program.scene.wait(0ms);

    OUGHT(a2.focused());
    OUGHT(throws<std::logic_error>([&program] {
        Window stranger;

        program.layers->raise(stranger);
    }));
}

// A modeless window opened while a modal one is open stands under it and does not take the focus;
// raising a modeless window does not lift it over the modal one either.
TRIAL("Layers: a modeless window stays under the modal one") {
    Program program;
    Form& form = program.form("Ask", {program.action("Ok")});
    auto made = std::make_shared<Window>("Under");

    (void)made->children().add<tal::Button>(program.action("U"));

    Window& under = program.layers->open(made);

    program.scene.wait(0ms);

    OUGHT(program.layers->top() == &form);
    OUGHT(program.document.focused() != nullptr);
    OUGHT(!under.children().empty());

    program.layers->raise(*program.screen);

    OUGHT(program.layers->top() == &form);
    OUGHT(program.document.confined() == &form);
}

TRIAL("Layers: opening nothing or a window open already throws") {
    Program program;
    auto made = std::make_shared<Window>("Twice");

    (void)program.layers->open(made);

    OUGHT(throws<std::invalid_argument>([&program] { (void)program.layers->open(nullptr); }));
    OUGHT(throws<std::logic_error>([&program, &made] { (void)program.layers->open(made); }));
    OUGHT(throws<std::logic_error>([&program] {
        auto modal = std::make_shared<Window>("M", Modal::yes);

        program.layers->raise(program.layers->open(modal));
    }));
}

// A window outside layers only hears that it closed.
TRIAL("Window: closing one outside layers") {
    auto made = std::make_shared<Window>("Alone");
    int closed = 0;
    Hooks hooks;

    hooks.add(made->on<Closed>([&closed](Closed&) { ++closed; }));

    made->close();

    SAME(closed, 1);
}

// A key a disabled bound action would answer is left to the window around: neither its shortcut nor
// its letter is taken. A modeless window does not close on Esc.
TRIAL("Window: a disabled action leaves its keys, and a modeless window stays on Esc") {
    Program program;
    auto outer_save = program.action("Outer", U'g');
    auto inner_save = program.action("Inner", U'g');
    auto made = std::make_shared<Window>("Outer", Modal::yes);
    auto inner = std::make_shared<Window>();

    outer_save->key(Key{Code::f3});
    inner_save->key(Key{Code::f3});
    inner_save->enable(false);
    inner->bind(inner_save);
    made->bind(outer_save);

    (void)inner->children().add<Field>();
    (void)made->children().add(inner);

    (void)program.layers->open(made);

    program.scene.wait(0ms);
    program.scene.press(Key{Code::f3});
    program.scene.press(Key{U'g', Mods{false, true, false}});

    OUGHT(program.ran == (std::vector<std::string>{"Outer", "Outer"}));

    Program other;
    auto loose = std::make_shared<Window>("Loose");
    Field& field = loose->children().add<Field>();

    (void)other.layers->open(loose);

    other.scene.wait(0ms);
    other.scene.press(Key{Code::escape});

    OUGHT(field.focused());
    OUGHT(other.layers->top() == loose.get());
}

// A window without a frame shows no title, and a window wider than the room stands in the middle
// with two columns to spare on each side and a row above and below.
TRIAL("Window: no title without a frame, and the room it may take") {
    Program program{Size{30, 10}};
    auto bare = std::make_shared<Window>("Hidden");

    bare->add_class("screen");
    bare->seat(Area{{0, 2}, {20, 3}});

    (void)program.layers->open(bare);

    program.scene.wait(0ms);

    OUGHT(!program.scene.where(" Hidden ").has_value());
    OUGHT(program.scene.at({0, 2}).cluster == " ");

    auto wide = std::make_shared<Window>("Wide");

    (void)wide->children().add<Label>(std::string(50, 'x') + "\n\n\n\n\n\n\n\n\n\n\n");

    Window& placed = program.layers->open(wide);

    program.scene.wait(0ms);

    OUGHT(placed.area() == (Area{{2, 1}, {26, 8}}));
}

// Seats and the full window count from the inside of the layers, which may have padding.
TRIAL("Layers: seats count from the inside of the layers") {
    Scene scene{Size{30, 10}};
    Document document{scene.time()};

    document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "layers", "style": {"padding": 1}}], "glyphs": {}})"));

    Layers& layers = document.children().add<Layers>();
    auto seated = std::make_shared<Window>("S");
    auto full = std::make_shared<Window>("F");

    seated->seat(Area{{2, 1}, {6, 3}});
    full->full = true;

    Window& one = layers.open(seated);
    Window& two = layers.open(full);

    scene.show(document);

    OUGHT(one.area() == (Area{{3, 2}, {6, 3}}));
    OUGHT(two.area() == (Area{{1, 1}, {28, 8}}));
}

// Raising the window on top changes nothing: the node with the focus keeps it, without a blur.
TRIAL("Layers: raising the top window leaves the focus alone") {
    Program program;
    int blurred = 0;

    program.hooks.add(program.first->on<tal::Blurred>([&blurred](tal::Blurred&) { ++blurred; }));

    program.layers->raise(*program.screen);
    program.scene.wait(0ms);

    SAME(blurred, 0);
    OUGHT(program.first->focused());
}

// A modeless window opened under a modal one takes no focus, and closing a modeless window under a
// modal one leaves the modal one confining; when the modal one closes, the focus goes back where it
// was before it.
TRIAL("Layers: modeless windows under a modal one neither take nor free the focus") {
    Program program;
    Form& form = program.form("Ask", {program.action("Ok")});
    auto made = std::make_shared<Window>("Under");

    (void)made->children().add<tal::Button>(program.action("U"));

    Window& under = program.layers->open(made);

    program.scene.wait(0ms);
    under.close();
    program.scene.wait(0ms);

    OUGHT(program.document.confined() == &form);

    auto again = std::make_shared<Window>("Again");

    (void)again->children().add<tal::Button>(program.action("A"));
    (void)program.layers->open(again);
    program.scene.wait(0ms);

    form.close();
    program.scene.wait(0ms);

    OUGHT(program.first->focused());
}

// Two modal windows: a request from beneath both waits while the lower one still confines, and is
// granted only when that one closes too.
TRIAL("Layers: a waiting request outlasts the upper of two modal windows") {
    Program program;
    Form& lower = program.form("Lower", {program.action("L")});
    Form& upper = program.form("Upper", {program.action("U")});

    program.second->focus();

    upper.close();
    program.scene.wait(0ms);

    OUGHT(!program.second->focused());
    OUGHT(program.document.focused() != nullptr);
    OUGHT(program.document.confined() == &lower);

    lower.close();
    program.scene.wait(0ms);

    OUGHT(program.second->focused());
}

// A full form gives its body the room to spare and keeps its answers at the bottom; an answer's
// letter runs it.
TRIAL("Form: the body takes the room, and an answer's letter runs it") {
    Program program{Size{20, 8}};
    auto save = program.action("Save", U's');
    auto made = std::make_shared<Form>("F");

    made->full = true;
    (void)made->body().children().add<Label>("body");
    made->answer(program.action("Keep", U'k'));
    made->answer(save);

    (void)program.layers->open(made);

    program.scene.wait(0ms);

    OUGHT(program.scene.where("[ Keep ]").has_value());
    SAME(program.scene.where("[ Keep ]")->y, 6);

    program.scene.press(Key{U's'});

    OUGHT(program.ran == (std::vector<std::string>{"Save"}));
}

// A node that captures the mouse on a press, and records what reaches it.
class Grabber : public Node {
public:
    Grabber() {
        hooks_.add(on<tal::MouseEvent>([this](tal::MouseEvent& what) {
            ++heard;

            if (what.press == tal::Press::down) capture();
        }));
    }

    int heard = 0;

private:
    Hooks hooks_;
};

// Confining the focus ends a capture outside, so that the mouse reaches the confinement again; a
// confined node leaving the tree frees the focus.
TRIAL("Document: confining ends a capture outside, and a confinement leaving frees it") {
    Scene scene{Size{20, 4}};
    Document document{scene.time()};
    tal::Row& row = document.children().add<tal::Row>();
    Grabber& outside = row.children().add<Grabber>();
    Grabber& inside = row.children().add<Grabber>();

    outside.share(tal::Share{1, 0});
    inside.share(tal::Share{1, 0});

    scene.show(document);

    document.mouse(tal::Press::down, tal::MouseButton::left, Spot{1, 1});

    SAME(outside.heard, 1);

    document.confine(&inside);
    document.mouse(tal::Press::move, tal::MouseButton::left, Spot{15, 1});

    SAME(inside.heard, 1);
    SAME(outside.heard, 1);

    const std::shared_ptr<Node> gone = row.children().remove(inside);

    OUGHT(document.confined() == nullptr);
}

// A shadow that comes from a class goes with it, also where it fell; a moved window takes its
// shadow along.
TRIAL("Window: a shadow comes and goes with its class and moves with the window") {
    Program program{Size{30, 8}};

    program.document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "window.dark", "style": {"shadow": true}}], "glyphs": {}})"));

    auto made = std::make_shared<Window>("W");

    made->seat(Area{{2, 1}, {6, 3}});

    Window& window = program.layers->open(made);

    program.scene.wait(0ms);

    OUGHT(!program.scene.at({8, 2}).look.faint);

    window.add_class("dark");
    program.scene.wait(0ms);

    OUGHT(program.scene.at({8, 2}).look.faint);

    window.seat(Area{{12, 1}, {6, 3}});
    program.scene.wait(0ms);

    OUGHT(!program.scene.at({8, 2}).look.faint);
    OUGHT(program.scene.at({18, 2}).look.faint);

    window.drop_class("dark");
    program.scene.wait(0ms);

    OUGHT(!program.scene.at({18, 2}).look.faint);
}

}
