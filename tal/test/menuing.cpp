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
#include "tal/menu.hpp"
#include "tal/node.hpp"
#include "tal/scene.hpp"
#include "tal/style.hpp"
#include "tal/window.hpp"

#include "tal/testing.hpp"

// Menus on a scene: a bar of titles whose menus open under them in the layers, keys and the mouse
// in them, submenus, picking that closes first, a menu moved back onto the screen at its edges, the
// key bar sharing its row, and pages under tabs.

namespace {

using namespace std::chrono_literals;

using tal::Action;
using tal::Area;
using tal::Code;
using tal::Column;
using tal::Document;
using tal::Hooks;
using tal::Key;
using tal::KeyBar;
using tal::Label;
using tal::Layers;
using tal::Limits;
using tal::Menu;
using tal::MenuBar;
using tal::Mods;
using tal::Node;
using tal::Run;
using tal::Size;
using tal::Spot;
using tal::State;
using tal::Tabs;
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

// Where `one` stands on the screen.
Spot spot(const Node& one) {
    Spot at = one.area().at;

    for (const Node* above = one.parent(); above != nullptr; above = above->parent()) at = Spot{at.x + above->area().at.x, at.y + above->area().at.y};

    return at;
}

// A node of three cells across that can be made to take no hover.
class Ground : public Node {
public:
    // Takes no hover, or takes it again.
    void ground(bool on) { hoverable(!on); }

protected:
    // Three cells of one row.
    Size measure(Limits room) const override {
        (void)room;

        return Size{3, 1};
    }
};

// Whether the row `said` begins with `start`.
bool begins(const std::string& said, const std::string& start) { return said.rfind(start, 0) == 0; }

// A program's screen on a scene: layers holding a full window without a frame, with a menu bar, a
// button, the room left and a key bar in a column, and a record of the actions run. The menu File
// holds Open (F3), Save, a line, the submenu Recent (One, Two) and Quit (F10); Edit holds Undo,
// Cut and Paste, disabled from the start; Help holds About.
struct Program {
    Scene scene;
    Document document{scene.time()};
    Layers* layers = nullptr;
    Window* screen = nullptr;
    MenuBar* bar = nullptr;
    Menu* recent = nullptr;
    tal::Button* button = nullptr;
    KeyBar* keys = nullptr;
    std::vector<std::string> ran;
    Hooks hooks;
    std::shared_ptr<Action> open = action("Open", U'O', Key{Code::f3});
    std::shared_ptr<Action> save = action("Save", U'S');
    std::shared_ptr<Action> one = action("One", U'O');
    std::shared_ptr<Action> two = action("Two", U'T');
    std::shared_ptr<Action> quit = action("Quit", U'Q', Key{Code::f10});
    std::shared_ptr<Action> undo = action("Undo", U'U');
    std::shared_ptr<Action> cut = action("Cut", U't');
    std::shared_ptr<Action> paste = action("Paste", U'P');
    std::shared_ptr<Action> about = action("About", U'A');
    std::shared_ptr<Action> press = action("Press");

    // Builds the screen and shows it.
    explicit Program(Size size = {40, 12}) : scene({.screen = size}) {
        layers = &document.children().add<Layers>();

        auto made = std::make_shared<Window>();

        made->add_class("screen");
        made->full = true;

        screen = &layers->open(made);

        Column& column = screen->children().add<Column>();

        bar = &column.children().add<MenuBar>();

        Menu& file = bar->add("File", U'F');

        file.add(open);
        file.add(save);
        file.separator();
        recent = &file.sub("Recent", U'R');
        recent->add(one);
        recent->add(two);
        file.add(quit);

        Menu& edit = bar->add("Edit", U'E');

        paste->enable(false);
        edit.add(undo);
        edit.add(cut);
        edit.add(paste);
        bar->add("Help", U'H').add(about);

        button = &column.children().add<tal::Button>(press);
        column.children().add<Node>().share(tal::Share{1, 0});
        keys = &column.children().add<KeyBar>();

        scene.show(document);
    }

    // A new action shown as `text` with `letter` and `key`, whose runs are recorded with whether a
    // menu was still open then.
    std::shared_ptr<Action> action(std::string text, std::optional<char32_t> letter = std::nullopt, std::optional<Key> key = std::nullopt) {
        auto made = std::make_shared<Action>(std::move(text), letter, key);

        hooks.add(made->on<Run>([this](Run& what) { ran.push_back(what.action.text() + (bar != nullptr && bar->opened() ? "+open" : "")); }));

        return made;
    }

    // The row of the node with the focus, or -1 without one.
    [[nodiscard]] int focused() const { return document.focused() != nullptr ? spot(*document.focused()).y : -1; }

    // Opens the menu `which` and lets the scene lay it out.
    void opened(std::size_t which) {
        bar->open(which);
        scene.wait(0ms);
    }
};

// A menu opens under its title in the layers, as a modal window: its title lit, the focus on its
// first position. Esc closes it, the title goes out and the focus comes back where it was.
TRIAL("MenuBar: a menu opens under its title with the focus in it, and Esc closes it") {
    Program program;

    OUGHT(program.button->focused());
    OUGHT(!program.bar->opened());
    OUGHT(begins(program.scene.row(0), " File  Edit  Help "));
    OUGHT(program.bar->measured(tal::Limits{{0, 0}, {40, 1}}) == (Size{18, 1}));

    program.opened(0);

    OUGHT(program.bar->opened());
    OUGHT(program.document.confined() != nullptr);
    OUGHT(program.layers->top() == program.document.confined());
    SAME(program.focused(), 2);
    OUGHT(program.scene.at({2, 0}).look.reverse);
    OUGHT(!program.scene.at({8, 0}).look.reverse);
    OUGHT(program.scene.at({3, 2}).look.reverse);

    program.scene.press(Key{Code::escape});

    OUGHT(!program.bar->opened());
    OUGHT(program.document.confined() == nullptr);
    OUGHT(program.button->focused());
    OUGHT(!program.scene.at({2, 0}).look.reverse);
    OUGHT(!begins(program.scene.row(2), "│ Open"));
    OUGHT(program.ran.empty());
}

// The list stands in a thin frame: the text a column in, the shortcut or the submenu's arrow
// against the right edge, a line joined to the frame, and the shadow of the window it stands in.
TRIAL("Menu: a frame, hints on the right, a joined line and a shadow") {
    Program program;

    program.opened(0);

    OUGHT(begins(program.scene.row(1), "┌───────────┐ "));
    OUGHT(begins(program.scene.row(2), "│ Open   F3 │"));
    OUGHT(begins(program.scene.row(3), "│ Save      │"));
    OUGHT(begins(program.scene.row(4), "├───────────┤"));
    OUGHT(begins(program.scene.row(5), "│ Recent  ▸ │"));
    OUGHT(begins(program.scene.row(6), "│ Quit  F10 │"));
    OUGHT(begins(program.scene.row(7), "└───────────┘"));
    OUGHT(program.scene.at({13, 1}).look == program.scene.at({20, 1}).look);
    OUGHT(program.scene.at({13, 2}).look != program.scene.at({20, 2}).look);
    OUGHT(program.scene.at({2, 8}).look != program.scene.at({20, 8}).look);
    OUGHT(program.scene.at({2, 3}).look.under);
}

// The arrows go round the enabled positions, past lines and disabled ones; Home and End go to the
// ends. A disabled position marks no letter, and its letter picks nothing.
TRIAL("Menu: the arrows go round the enabled positions, Home and End to the ends") {
    Program program;

    program.save->enable(false);
    program.opened(0);

    const std::vector<std::pair<Key, int>> steps{{Key{Code::down}, 5}, {Key{Code::down}, 6}, {Key{Code::down}, 2}, {Key{Code::up}, 6},
                                                 {Key{Code::up}, 5},   {Key{Code::home}, 2}, {Key{Code::end}, 6}, {Key{Code::up}, 5}};

    for (const auto& [key, row] : steps) {
        program.scene.press(key);

        SAME(program.focused(), row);
    }

    OUGHT(!program.scene.at({2, 3}).look.under);

    program.scene.press(Key{U's'});

    OUGHT(program.bar->opened());
    OUGHT(program.ran.empty());

    program.scene.press(Key{Code::down, Mods{false, false, true}});

    SAME(program.focused(), 5);
}

// Enter and the space bar pick the position with the focus, a letter picks its position in either
// case; every menu closes before the action runs.
TRIAL("Menu: Enter, the space bar and a letter pick, the menu closed before the action runs") {
    Program program;

    program.opened(0);
    program.scene.press(Key{Code::enter});
    program.opened(0);
    program.scene.press(Key{Code::down});
    program.scene.press(Key{U' '});
    program.opened(0);
    program.scene.press(Key{U'q'});
    program.opened(0);
    program.scene.press(Key{U'S'});
    program.opened(0);
    program.scene.press(Key{U'q', Mods{true, false, false}});
    program.scene.press(Key{U'q', Mods{false, true, false}});

    OUGHT(program.bar->opened());
    OUGHT(program.ran == (std::vector<std::string>{"Open", "Save", "Quit", "Save"}));

    program.scene.press(Key{Code::escape});

    OUGHT(program.button->focused());
}

// A window an action opens stands over the screen, since the menu closed first: it takes the focus
// and the confinement, and closed gives the focus back to what had it before the menu.
TRIAL("Menu: a window opened by the picked action stands on top") {
    Program program;
    Window* opened = nullptr;

    program.hooks.add(program.about->on<Run>([&program, &opened](Run&) {
        opened = &program.layers->open(std::make_shared<tal::Message>("About", "A program.", std::vector<std::shared_ptr<Action>>{}));
    }));

    program.opened(2);
    program.scene.press(Key{Code::enter});
    program.scene.wait(0ms);

    OUGHT(opened != nullptr);
    OUGHT(program.layers->top() == opened);
    OUGHT(program.document.confined() == opened);

    program.scene.press(Key{Code::escape});

    OUGHT(program.button->focused());
}

// The right arrow or the letter of a submenu opens it beside its position, the first position
// focused; Esc and the left arrow close only it, the focus back on its position. A pick in it
// closes everything.
TRIAL("Menu: a submenu opens beside its position, and closes alone") {
    Program program;

    program.opened(0);
    program.scene.press(Key{U'r'});
    program.scene.wait(0ms);

    OUGHT(begins(program.scene.row(4), "├───────────┤┌─────┐"));
    OUGHT(begins(program.scene.row(5), "│ Recent  ▸ ││ One │"));
    SAME(program.focused(), 5);
    OUGHT(program.document.focused() != nullptr && spot(*program.document.focused()).x == 14);

    program.scene.press(Key{Code::escape});

    OUGHT(program.bar->opened());
    OUGHT(program.scene.at({2, 0}).look.reverse);
    SAME(program.focused(), 5);
    OUGHT(program.document.focused() != nullptr && spot(*program.document.focused()).x == 1);

    program.scene.press(Key{Code::right});
    program.scene.wait(0ms);

    OUGHT(spot(*program.document.focused()).x == 14);

    program.scene.press(Key{Code::left});

    OUGHT(program.bar->opened());
    OUGHT(spot(*program.document.focused()).x == 1);

    program.scene.press(Key{Code::enter});
    program.scene.wait(0ms);
    program.scene.press(Key{Code::down});
    program.scene.press(Key{Code::enter});

    OUGHT(!program.bar->opened());
    OUGHT(program.ran == (std::vector<std::string>{"Two"}));
    OUGHT(program.button->focused());
}

// The side arrows open the menu beside, round the ends, but not with Shift; in a submenu the right
// one too, while the left one closes the submenu.
TRIAL("MenuBar: the side arrows step through the menus of the bar") {
    Program program;

    program.opened(0);
    program.scene.press(Key{Code::right, Mods{false, false, true}});
    program.scene.wait(0ms);

    OUGHT(program.scene.at({2, 0}).look.reverse);

    program.scene.press(Key{Code::right});
    program.scene.wait(0ms);

    OUGHT(program.scene.at({8, 0}).look.reverse);
    OUGHT(!program.scene.at({2, 0}).look.reverse);
    OUGHT(begins(program.scene.row(2).substr(6), "│ Undo"));

    program.scene.press(Key{Code::end});

    SAME(program.focused(), 3);

    program.scene.press(Key{Code::right});
    program.scene.press(Key{Code::right});
    program.scene.wait(0ms);

    OUGHT(program.scene.at({2, 0}).look.reverse);

    program.scene.press(Key{Code::left});
    program.scene.wait(0ms);

    OUGHT(program.scene.at({14, 0}).look.reverse);

    program.scene.press(Key{Code::right});
    program.scene.wait(0ms);
    program.scene.press(Key{U'r'});
    program.scene.wait(0ms);
    program.scene.press(Key{Code::right});
    program.scene.wait(0ms);

    OUGHT(program.scene.at({8, 0}).look.reverse);
    OUGHT(begins(program.scene.row(2).substr(6), "│ Undo"));
    OUGHT(program.ran.empty());

    program.scene.press(Key{Code::escape});

    OUGHT(!program.bar->opened());
    OUGHT(program.button->focused());
}

// A press of the left button on a title opens its menu; the mouse over a position gives it the
// focus and a click picks it. Outside the menu a press closes it and reaches nothing beneath, on
// another title opens that one's menu, and on the open one's title closes it, leaving the title
// only hovered.
TRIAL("MenuBar: the mouse opens, moves, picks and closes") {
    Program program;
    const Spot press = spot(*program.button);

    program.scene.click({8, 0}, tal::MouseButton::right);

    OUGHT(!program.bar->opened());

    program.scene.click({8, 0});
    program.scene.wait(0ms);

    OUGHT(program.bar->opened());
    OUGHT(program.scene.at({8, 0}).look.reverse);
    SAME(program.focused(), 2);

    program.scene.point({9, 3});

    SAME(program.focused(), 3);

    program.scene.click({9, 3});

    OUGHT(!program.bar->opened());
    OUGHT(program.ran == (std::vector<std::string>{"Cut"}));

    program.scene.click({13, 0});
    program.scene.wait(0ms);
    program.scene.click({press.x + 1, press.y});

    OUGHT(!program.bar->opened());
    SAME(program.ran.size(), std::size_t{1});
    OUGHT(program.button->focused());

    program.scene.click({1, 0});
    program.scene.wait(0ms);
    program.scene.click({14, 0});
    program.scene.wait(0ms);

    OUGHT(program.scene.at({14, 0}).look.reverse);
    OUGHT(!program.scene.at({2, 0}).look.reverse);

    program.scene.click({14, 0});

    OUGHT(!program.bar->opened());
    OUGHT(program.scene.at({14, 0}).look.reverse);

    program.scene.point({30, 5});

    OUGHT(!program.scene.at({14, 0}).look.reverse);
    OUGHT(program.button->focused());
}

// A click on a submenu's position opens it; a press in the menu under it closes only the submenu,
// so that a click on a position there picks it at once.
TRIAL("Menu: the mouse in a menu under an open submenu closes the submenu") {
    Program program;

    program.opened(0);
    program.scene.click({3, 5});
    program.scene.wait(0ms);

    SAME(spot(*program.document.focused()).x, 14);

    program.scene.click({3, 4});
    program.scene.wait(0ms);

    OUGHT(program.bar->opened());
    OUGHT(!program.scene.where("One").has_value());
    SAME(program.focused(), 5);

    program.scene.click({3, 5});
    program.scene.wait(0ms);
    program.scene.click({3, 3});

    OUGHT(!program.bar->opened());
    OUGHT(program.ran == (std::vector<std::string>{"Save"}));
}

// A bar taken away with a menu open closes it: the layers keep no window of it, and the focus
// comes back.
TRIAL("MenuBar: a bar taken away closes its open menus") {
    Program program;

    program.opened(0);
    program.scene.press(Key{U'r'});
    program.scene.wait(0ms);

    std::shared_ptr<Node> gone = program.bar->parent()->children().remove(*program.bar);

    program.bar = nullptr;
    gone.reset();
    program.scene.wait(0ms);

    OUGHT(program.layers->top() == program.screen);
    OUGHT(program.document.confined() == nullptr);
    OUGHT(program.button->focused());
}

// The bar itself takes no hover, only the title under the mouse, and the box of a menu none, only
// its position.
TRIAL("MenuBar: the bar takes no hover, its title does") {
    Program program;

    program.scene.point({8, 0});

    const auto titles = program.bar->children().begin();
    Node& first = *titles;

    OUGHT(!program.bar->states().has(State::hover));
    OUGHT(program.scene.at({8, 0}).look.reverse);
    OUGHT(!program.scene.at({20, 0}).look.reverse);
    OUGHT(!first.states().has(State::hover));

    program.scene.point({1, 0});

    OUGHT(first.states().has(State::hover));
    OUGHT(!program.bar->states().has(State::hover));

    program.scene.point({30, 0});

    OUGHT(!first.states().has(State::hover));
    OUGHT(!program.bar->states().has(State::hover));

    program.opened(0);
    program.scene.point({5, 3});

    const Node* save = program.document.focused();

    OUGHT(save != nullptr && save->states().has(State::hover));
    OUGHT(save != nullptr && !save->parent()->states().has(State::hover));
}

// A menu that would stick out past the right edge moves left to fit, and one past the bottom moves
// up, over its title; a title past the end of the bar is cut. A menu beyond the last or a bar
// outside layers throws.
TRIAL("MenuBar: a menu moves back onto the screen at the edges, and opening wrongly throws") {
    Program program{Size{16, 7}};

    auto help = program.bar->children().begin();

    ++help;
    ++help;

    SAME((*help).area().size.wide, 4);

    program.opened(2);

    OUGHT(begins(program.scene.row(1).substr(program.scene.row(1).find("┌")), "┌───────┐"));
    SAME(spot(*program.document.focused()).x, 8);

    program.scene.press(Key{Code::escape});
    program.opened(0);

    OUGHT(begins(program.scene.row(0), "┌───────────┐"));
    SAME(program.focused(), 1);

    program.scene.press(Key{U'r'});
    program.scene.wait(0ms);

    const Spot first = spot(*program.document.focused());

    SAME(first.x, 10);
    SAME(first.y, 4);
    OUGHT(throws<std::out_of_range>([&program] { program.bar->open(3); }));

    MenuBar alone;

    alone.add("Lone");

    OUGHT(throws<std::logic_error>([&alone] { alone.open(); }));
    OUGHT(throws<std::invalid_argument>([&alone] { alone.add("Bad").add(nullptr); }));
}

// The frame and its joined lines take the look of the part "frame", the positions not; a menu
// whose sheet gives it no frame draws its lines across the whole row, without joints.
TRIAL("Menu: the frame in the look of its part, and lines without a frame") {
    Program program;
    const std::optional<tal::Color> red{tal::Color{255, 0, 0}};

    program.document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "menu::frame", "style": {"ink": "#ff0000"}}], "glyphs": {}})"));
    program.opened(0);

    OUGHT(program.scene.at({0, 1}).look.ink == red);
    OUGHT(program.scene.at({0, 2}).look.ink == red);
    OUGHT(program.scene.at({5, 4}).look.ink == red);
    OUGHT(program.scene.at({12, 4}).look.ink == red);
    OUGHT(program.scene.at({4, 2}).look.ink != red);

    program.scene.press(Key{Code::escape});
    program.document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "menu.box", "style": {"border": "none"}}], "glyphs": {}})"));
    program.opened(0);

    OUGHT(begins(program.scene.row(1), " Open   F3  "));
    OUGHT(begins(program.scene.row(3), "─────────── "));
    SAME(program.focused(), 1);
}

// A menu standing by itself in more room than it fills, without a frame, leaves the rest blank; a
// menu of a bar shown elsewhere by hand opens no submenu, since no menu of the bar is open.
TRIAL("Menu: standing by itself, it draws no frame it has not, and opens no submenu") {
    Scene scene{{.screen = {10, 3}}};
    Document document{scene.time()};
    MenuBar bar;
    Menu& menu = bar.add("Bar");
    auto one = std::make_shared<Action>("One");

    menu.add(one);
    (void)menu.sub("More").add(one);
    document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "menu.box", "style": {"border": "none"}}], "glyphs": {}})"));
    (void)document.children().add(menu.shared_from_this());
    scene.show(document);

    SAME(scene.row(2), std::string{"          "});

    scene.click({2, 1});

    OUGHT(!bar.opened());
    SAME(scene.row(2), std::string{"          "});

    std::shared_ptr<Node> back = document.children().remove(menu);

    (void)back;
}

// A menu taller than the room shows the positions that fit; the others get no row and no focus.
TRIAL("Menu: positions past the room get no row") {
    Program program{Size{16, 5}};

    program.opened(0);

    OUGHT(begins(program.scene.row(0), "┌───────────┐"));
    OUGHT(begins(program.scene.row(4), "└───────────┘"));
    SAME(program.focused(), 1);

    program.scene.press(Key{Code::end});

    SAME(program.focused(), 2);

    program.scene.press(Key{Code::down});

    SAME(program.focused(), 1);
}

// Layers taken away under an open menu take its window with them: the bar counts no menu open, and
// once in new layers it opens a menu with only that title lit.
TRIAL("MenuBar: layers gone under an open menu, and the bar in new layers") {
    Program program;
    const std::shared_ptr<Window> screen = std::static_pointer_cast<Window>(program.screen->shared_from_this());

    program.opened(1);

    std::shared_ptr<Node> gone = program.document.children().remove(*program.layers);

    program.layers = nullptr;
    gone.reset();

    OUGHT(!program.bar->opened());

    Layers& fresh = program.document.children().add<Layers>();

    (void)fresh.open(screen);
    program.scene.wait(0ms);
    program.opened(0);

    OUGHT(program.bar->opened());
    OUGHT(program.scene.at({2, 0}).look.reverse);
    OUGHT(!program.scene.at({8, 0}).look.reverse);
}

// A node that takes no hover loses it at once, and takes it again only when the mouse moves after
// it is allowed to.
TRIAL("Node: a node that takes no hover loses it at once") {
    Scene scene{{.screen = {10, 2}}};
    Document document{scene.time()};
    Ground& ground = document.children().add<Ground>();

    scene.show(document);
    scene.point({1, 0});

    OUGHT(ground.states().has(State::hover));

    ground.ground(true);

    OUGHT(!ground.states().has(State::hover));

    scene.point({2, 0});

    OUGHT(!ground.states().has(State::hover));

    ground.ground(false);

    OUGHT(!ground.states().has(State::hover));

    scene.point({1, 0});

    OUGHT(ground.states().has(State::hover));
}

// A window anchored stands at its anchor in the size of its content, moved left and up as far as it
// must to fit, and never past the corner; one that does not fit under its anchor stands over the
// rows above it that belong to its owner, when it fits there. A seat goes before the anchor, a full
// window before both. The spot of the screen for an anchor is counted inside the layers' border and
// padding.
TRIAL("Window: an anchor, moved back to fit, after a seat; Layers: the screen counted inside") {
    Scene scene{{.screen = {20, 10}}};
    Document document{scene.time()};
    Column& column = document.children().add<Column>();

    (void)column.children().add<Label>("top");

    Column& inner = column.children().add<Column>();

    inner.share(tal::Share{1, 0});
    (void)inner.children().add<Label>("middle");

    Layers& layers = inner.children().add<Layers>();

    layers.share(tal::Share{1, 0});

    auto made = std::make_shared<Window>();

    made->children().add<Label>("abcd");
    made->anchor(Spot{3, 2});

    Window& window = layers.open(made);

    scene.show(document);

    OUGHT(window.anchor() == std::optional<Spot>{Spot{3, 2}});
    OUGHT(window.area() == (Area{{3, 2}, {6, 3}}));

    window.anchor(Spot{17, 9});
    scene.wait(0ms);

    OUGHT(window.area() == (Area{{14, 5}, {6, 3}}));
    SAME(window.above(), 0);

    window.anchor(Spot{17, 7});
    scene.wait(0ms);
    window.anchor(Spot{17, 7}, 2);
    scene.wait(0ms);

    OUGHT(window.area() == (Area{{14, 2}, {6, 3}}));
    SAME(window.above(), 2);

    window.anchor(Spot{17, 7}, -4);
    scene.wait(0ms);

    OUGHT(window.area() == (Area{{14, 5}, {6, 3}}));
    SAME(window.above(), 0);

    window.anchor(Spot{17, 4}, 2);
    scene.wait(0ms);

    OUGHT(window.area() == (Area{{14, 4}, {6, 3}}));

    window.anchor(Spot{17, 7}, 6);
    scene.wait(0ms);

    OUGHT(window.area() == (Area{{14, 5}, {6, 3}}));

    window.anchor(Spot{-3, -2});
    scene.wait(0ms);

    OUGHT(window.area() == (Area{{0, 0}, {6, 3}}));

    window.seat(Area{{1, 1}, {4, 2}});
    scene.wait(0ms);

    OUGHT(window.area() == (Area{{1, 1}, {4, 2}}));

    window.full = true;
    scene.wait(0ms);

    OUGHT(window.area() == (Area{{0, 0}, {20, 8}}));

    window.full = false;
    window.seat(std::nullopt);
    window.anchor(std::nullopt);
    scene.wait(0ms);

    OUGHT(window.area() == (Area{{7, 2}, {6, 3}}));
    OUGHT(layers.local(Spot{5, 7}) == (Spot{5, 5}));

    document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "layers", "style": {"border": "thin", "padding": [1, 0, 0, 2]}}], "glyphs": {}})"));
    scene.wait(0ms);

    OUGHT(layers.local(Spot{5, 7}) == (Spot{2, 3}));
}

// The keys share the row evenly, the left ones taking what the division leaves: an F key shows its
// number before the text, a letter that the text holds is marked in it instead of a tag, and one
// it does not hold stands as the tag. A disabled key marks nothing; a click runs the action and
// leaves the focus where it was.
TRIAL("KeyBar: keys share the row, tags and marks, a click runs without the focus") {
    Program program;

    program.keys->add(program.action("View", std::nullopt, Key{Code::f3}));
    program.keys->add(program.action("Quit", std::nullopt, Key{U'q'}));
    program.keys->add(program.action("Exit", U'x', Key{Code::f10}));
    program.scene.wait(0ms);

    const std::string row = program.scene.row(11);

    SAME(row.substr(0, 5), std::string{"3View"});
    SAME(row.substr(14, 4), std::string{"Quit"});
    SAME(row.substr(27, 6), std::string{"10Exit"});
    OUGHT(!program.scene.at({0, 11}).look.reverse);
    OUGHT(program.scene.at({1, 11}).look.reverse);
    OUGHT(program.scene.at({13, 11}).look.reverse);
    OUGHT(program.scene.at({14, 11}).look.under);
    OUGHT(!program.scene.at({15, 11}).look.under);
    OUGHT(program.scene.at({39, 11}).look.reverse);

    program.scene.click({30, 11});

    OUGHT(program.ran == (std::vector<std::string>{"Exit"}));
    OUGHT(program.button->focused());

    program.keys->clear();
    program.keys->add(program.action("Quit", std::nullopt, Key{U'z'}));
    program.keys->add(program.action("Copy", U'C'));

    auto off = program.action("Move", U'M');

    off->enable(false);
    program.keys->add(off);
    program.scene.wait(0ms);

    const std::string again = program.scene.row(11);

    SAME(again.substr(0, 5), std::string{"zQuit"});
    SAME(again.substr(14, 4), std::string{"Copy"});
    OUGHT(program.scene.at({14, 11}).look.under);
    SAME(again.substr(27, 4), std::string{"Move"});
    OUGHT(!program.scene.at({27, 11}).look.under);
    OUGHT(!program.scene.at({27, 11}).look.reverse);

    program.scene.click({28, 11});

    SAME(program.ran.size(), std::size_t{1});
    OUGHT(throws<std::invalid_argument>([&program] { program.keys->add(nullptr); }));
}

// A named key other than an F key has no tag; a disabled key shows its tag in its own dim look; a
// key enabled again marks its letter.
TRIAL("KeyBar: no tag for a named key, a dim tag when disabled, the mark when enabled again") {
    Program program{Size{30, 6}};
    auto off = program.action("Off", std::nullopt, Key{Code::f5});
    auto move = program.action("Move", U'M');

    off->enable(false);
    move->enable(false);
    program.keys->add(program.action("Back", std::nullopt, Key{Code::escape}));
    program.keys->add(off);
    program.keys->add(move);
    program.scene.wait(0ms);

    OUGHT(begins(program.scene.row(5), "Back      5Off      Move"));
    OUGHT(program.scene.at({0, 5}).look.reverse);
    OUGHT(program.scene.at({10, 5}).look.faint);
    OUGHT(!program.scene.at({20, 5}).look.under);

    move->enable(true);
    program.scene.wait(0ms);

    OUGHT(program.scene.at({20, 5}).look.under);
}

// A text too long for its key ends with the glyph `elide`, one that just fits stands whole; a bar
// without keys takes no row.
TRIAL("KeyBar: a long text is cut, an empty bar takes no row") {
    Program program{Size{12, 6}};

    OUGHT(program.keys->area().size.high == 0);

    program.keys->add(program.action("Everything", std::nullopt, Key{Code::f1}));
    program.keys->add(program.action("Gopher", U'G'));
    program.scene.wait(0ms);

    SAME(program.scene.row(5), std::string{"1Ever…Gopher"});
    SAME(program.keys->area().size.high, 1);

    program.keys->clear();
    program.scene.wait(0ms);

    SAME(program.keys->area().size.high, 0);
}

// Pages under names in the middle over a thin line, thick under the shown one: Ctrl+PgDn and
// Ctrl+PgUp step round the ends, Alt with a letter and a click on a name show its page. The hidden
// pages stand without room, and the focus moves from a page leaving to the first node of the new
// one; a page with nothing to take it leaves the focus.
TRIAL("Tabs: names over the line, switching pages by keys and clicks, the focus following") {
    Scene scene{{.screen = {30, 8}}};
    Document document{scene.time()};
    Tabs& tabs = document.children().add<Tabs>();
    auto first = std::make_shared<Action>("A");
    auto second = std::make_shared<Action>("B");
    tal::Button& a = tabs.add("One", U'O').children().add<tal::Button>(first);
    tal::Button& b = tabs.add("Two", U'w').children().add<tal::Button>(second);

    (void)tabs.add("Three", U'h').children().add<Label>("Nothing here");

    scene.show(document);
    a.focus();
    scene.wait(0ms);

    OUGHT(begins(scene.row(0), "       One  Two  Three "));
    OUGHT(begins(scene.row(1), "──────━━━━━──────"));
    OUGHT(scene.at({8, 0}).look.bold);
    OUGHT(!scene.at({13, 0}).look.bold);
    OUGHT(scene.at({13, 0}).look.under);
    OUGHT(!b.focusable());
    OUGHT(a.focused());

    scene.press(Key{Code::pagedown, Mods{true, false, false}});
    scene.wait(0ms);

    SAME(*tabs.current, std::size_t{1});
    OUGHT(b.focused());
    OUGHT(!a.focusable());
    OUGHT(begins(scene.row(1), "───────────━━━━━─"));

    scene.press(Key{U'O', Mods{false, true, false}});
    scene.wait(0ms);

    SAME(*tabs.current, std::size_t{0});
    OUGHT(a.focused());

    scene.press(Key{Code::pageup, Mods{true, false, false}});
    scene.wait(0ms);

    SAME(*tabs.current, std::size_t{2});
    OUGHT(a.focused());

    scene.click({12, 0});
    scene.wait(0ms);

    SAME(*tabs.current, std::size_t{1});
    OUGHT(b.focused());

    scene.press(Key{Code::pagedown, Mods{true, false, true}});
    scene.press(Key{U'x', Mods{false, true, false}});
    scene.press(Key{U'o', Mods{true, true, false}});
    scene.click({0, 0});

    SAME(*tabs.current, std::size_t{1});
}

// The focus stays on its page while the page shows, also when the tabs are laid out again, and a
// page left for one with nothing to focus finds it where it was when it comes back.
TRIAL("Tabs: the focus stays where it was on a page that shows") {
    Scene scene{{.screen = {30, 8}}};
    Document document{scene.time()};
    Tabs& tabs = document.children().add<Tabs>();
    auto act = std::make_shared<Action>("A");
    Column& page = tabs.add("One", U'O');

    (void)page.children().add<tal::Button>(act);

    tal::Button& second = page.children().add<tal::Button>(act);
    Label& said = page.children().add<Label>("x");

    (void)tabs.add("Two", U'T').children().add<Label>("Nothing");

    scene.show(document);
    second.focus();
    said.text = "a longer text";
    scene.wait(0ms);

    OUGHT(second.focused());

    scene.press(Key{Code::pagedown, Mods{true, false, false}});

    SAME(*tabs.current, std::size_t{1});
    OUGHT(second.focused());

    scene.press(Key{U'o', Mods{false, true, false}});

    SAME(*tabs.current, std::size_t{0});
    OUGHT(second.focused());
}

// The size stays that of the largest page; a `current` past the last shows the last.
TRIAL("Tabs: the size of the largest page, and a current past the end") {
    Document document;
    Tabs& tabs = document.children().add<Tabs>();

    (void)tabs.add("A").children().add<Label>("short");
    (void)tabs.add("B").children().add<Label>("a longer text");

    OUGHT(tabs.measured(tal::Limits{{0, 0}, {40, 10}}) == (Size{13, 3}));

    Tabs& named = document.children().add<Tabs>();

    (void)named.add("Alpha").children().add<Label>("x");
    (void)named.add("Beta");

    OUGHT(named.measured(tal::Limits{{0, 0}, {40, 10}}) == (Size{13, 3}));

    Scene scene{{.screen = {20, 4}}};
    Document shown{scene.time()};
    Tabs& more = shown.children().add<Tabs>();
    (void)more.add("A").children().add<Label>("first");
    (void)more.add("B").children().add<Label>("second");
    more.current = 5;
    scene.show(shown);

    OUGHT(begins(scene.row(2), "second"));
    OUGHT(scene.at({11, 0}).look.bold);
}

// Names that do not fit leave only the shown one, between the arrows, cut to the room between
// them; a click on an arrow steps, a click elsewhere on the names' row or under an arrow does not.
TRIAL("Tabs: crowded names show the current one between arrows") {
    Scene scene{{.screen = {12, 4}}};
    Document document{scene.time()};
    Tabs& tabs = document.children().add<Tabs>();

    (void)tabs.add("One");
    (void)tabs.add("Two");
    (void)tabs.add("Three");

    scene.show(document);

    SAME(scene.row(0), std::string{"◀   One    ▶"});
    OUGHT(begins(scene.row(1), "───━━━━━────"));

    scene.click({11, 0});
    scene.wait(0ms);

    SAME(*tabs.current, std::size_t{1});

    scene.click({0, 0});
    scene.wait(1s);
    scene.click({0, 0});
    scene.wait(0ms);

    SAME(*tabs.current, std::size_t{2});
    OUGHT(begins(scene.row(0), "◀  Three   ▶"));

    scene.click({5, 1});
    scene.wait(1s);
    scene.click({1, 0});
    scene.wait(1s);
    scene.click({0, 1});

    SAME(*tabs.current, std::size_t{2});

    Scene framed{{.screen = {14, 5}}};
    Document boxed{framed.time()};
    Tabs& inside = boxed.children().add<Tabs>();

    boxed.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "tabs", "style": {"border": "thin"}}], "glyphs": {}})"));
    (void)inside.add("One");
    (void)inside.add("Two");
    (void)inside.add("Three");
    framed.show(boxed);

    SAME(framed.row(1), std::string{"│◀   One    ▶│"});
    SAME(framed.row(2), std::string{"│───━━━━━────│"});

    framed.click({12, 1});

    SAME(*inside.current, std::size_t{1});

    Scene narrow{{.screen = {6, 3}}};
    Document lone{narrow.time()};

    (void)lone.children().add<Tabs>().add("Three");

    narrow.show(lone);

    SAME(narrow.row(0), std::string{"◀ Thr▶"});
}

}

// Pages put in before others, taken away and moved: the page shown stays shown, and taking it away
// shows the next one (the one before, for the last), the focus that was on it moving to the page
// shown. Names change; a page or place that does not stand throws std::out_of_range.
TRIAL("Tabs: pages put in, taken away and moved, the shown one staying shown") {
    Scene scene{{.screen = {40, 6}}};
    Document document{scene.time()};
    Tabs& tabs = document.children().add<Tabs>();

    (void)tabs.add("One");
    tal::Button& first = tabs.add("Two").children().add<tal::Button>(std::make_shared<Action>("go"));
    (void)tabs.add("Three");
    tal::Button& other = tabs.page(2).children().add<tal::Button>(std::make_shared<Action>("on"));

    tabs.current = 1;
    scene.show(document);
    first.focus();
    scene.wait(0ms);

    (void)tabs.insert(0, "Zero");

    SAME(*tabs.current, std::size_t{2});
    SAME(tabs.title(2), std::string{"Two"});
    SAME(tabs.count(), std::size_t{4});

    (void)tabs.insert(4, "Four");

    SAME(*tabs.current, std::size_t{2});
    OUGHT(throws<std::out_of_range>([&tabs] { (void)tabs.insert(6, "Six"); }));

    tabs.remove(0);

    SAME(*tabs.current, std::size_t{1});
    SAME(tabs.title(1), std::string{"Two"});

    tabs.remove(1);
    scene.wait(0ms);

    SAME(*tabs.current, std::size_t{1});
    SAME(tabs.title(1), std::string{"Three"});
    OUGHT(other.focused());

    tabs.current = 2;
    tabs.remove(2);

    SAME(*tabs.current, std::size_t{1});
    SAME(tabs.count(), std::size_t{2});

    tabs.move(1, 0);

    SAME(*tabs.current, std::size_t{0});
    SAME(tabs.title(0), std::string{"Three"});

    tabs.move(1, 0);

    SAME(*tabs.current, std::size_t{1});

    tabs.title(0, "Uno");
    scene.wait(0ms);

    SAME(tabs.title(0), std::string{"Uno"});
    OUGHT(scene.row(0).find("Uno") != std::string::npos);
    OUGHT(&tabs.page(1) == other.parent());

    OUGHT(throws<std::out_of_range>([&tabs] { tabs.remove(2); }));
    OUGHT(throws<std::out_of_range>([&tabs] { tabs.move(0, 2); }));
    OUGHT(throws<std::out_of_range>([&tabs] { tabs.move(2, 0); }));
    OUGHT(throws<std::out_of_range>([&tabs] { tabs.title(2, "x"); }));
    OUGHT(throws<std::out_of_range>([&tabs] { (void)tabs.title(2); }));
    OUGHT(throws<std::out_of_range>([&tabs] { (void)tabs.page(2); }));

    tabs.remove(0);
    tabs.remove(0);

    SAME(tabs.count(), std::size_t{0});
    SAME(*tabs.current, std::size_t{0});
}

// Framed with a border, the names stand in its top edge after the heading, a cell of the edge
// between them, and the page right under the edge; a click shows a page, a middle click tells
// `Closing` and closes the page unless a hook takes it. Crowded, the shown name stands between
// arrows after the heading, and a click on an arrow steps. Framed without a border, the names take
// the first row, the page the rows under it.
TRIAL("Tabs: framed, the names in the top edge after the heading") {
    Scene scene{{.screen = {30, 5}}};
    Document document{scene.time()};
    Tabs& tabs = document.children().add<Tabs>();

    document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "tabs", "style": {"border": "thin"}}], "glyphs": {}})"));
    tabs.framed = true;
    tabs.heading = "1";
    (void)tabs.add("Filmy").children().add<Label>("x");
    (void)tabs.add("Pobrane");
    scene.show(document);

    SAME(scene.row(0), std::string{"┌1─ Filmy ─ Pobrane ─────────┐"});
    OUGHT(begins(scene.row(1), "│x "));

    scene.click({12, 0});

    SAME(*tabs.current, std::size_t{1});

    std::vector<std::size_t> asked;
    bool keep = true;
    const tal::Hook closing = tabs.on<tal::Closing>([&](tal::Closing& what) {
        asked.push_back(what.which);

        if (keep) what.take();
    });

    scene.wait(1s);
    scene.click({4, 0}, tal::MouseButton::middle);

    SAME(asked, (std::vector<std::size_t>{0}));
    SAME(tabs.count(), std::size_t{2});

    keep = false;
    scene.wait(1s);
    scene.click({4, 0}, tal::MouseButton::middle);
    scene.wait(0ms);

    SAME(asked, (std::vector<std::size_t>{0, 0}));
    SAME(tabs.count(), std::size_t{1});
    SAME(scene.row(0), std::string{"┌1─ Pobrane ─────────────────┐"});

    Scene narrow{{.screen = {16, 4}}};
    Document small{narrow.time()};
    Tabs& crowd = small.children().add<Tabs>();

    small.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "tabs", "style": {"border": "thin"}}], "glyphs": {}})"));
    crowd.framed = true;
    crowd.heading = "1";
    (void)crowd.add("Filmy");
    (void)crowd.add("Pobrane");
    narrow.show(small);

    SAME(narrow.row(0), std::string{"┌1─◀ Filmy ───▶┐"});

    Scene fits{{.screen = {21, 4}}};
    Document roomy{fits.time()};
    Tabs& full = roomy.children().add<Tabs>();
    Scene short_by_one{{.screen = {20, 4}}};
    Document tight{short_by_one.time()};
    Tabs& cut = tight.children().add<Tabs>();

    for (Document* one : {&roomy, &tight}) one->sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "tabs", "style": {"border": "thin"}}], "glyphs": {}})"));

    for (Tabs* one : {&full, &cut}) {
        one->framed = true;
        one->heading = "1";
        (void)one->add("Filmy");
        (void)one->add("Pobrane");
    }

    fits.show(roomy);
    short_by_one.show(tight);

    SAME(fits.row(0), std::string{"┌1─ Filmy ─ Pobrane ┐"});
    OUGHT(begins(short_by_one.row(0), "┌1─◀"));

    narrow.click({14, 0});

    SAME(*crowd.current, std::size_t{1});

    narrow.wait(1s);
    narrow.click({3, 0});

    SAME(*crowd.current, std::size_t{0});

    Scene bare{{.screen = {20, 3}}};
    Document plain{bare.time()};
    Tabs& open = plain.children().add<Tabs>();

    open.framed = true;
    (void)open.add("A").children().add<Label>("y");
    (void)open.add("B");
    bare.show(plain);

    OUGHT(begins(bare.row(0), " A   B "));
    OUGHT(begins(bare.row(1), "y"));
}
