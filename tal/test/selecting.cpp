#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "tal/controls.hpp"
#include "tal/document.hpp"
#include "tal/event.hpp"
#include "tal/key.hpp"
#include "tal/layout.hpp"
#include "tal/scene.hpp"
#include "tal/window.hpp"

#include "tal/testing.hpp"

// A selection: the chosen option on one row with a mark, chosen again by the side keys, and the
// list of its options dropped under it, or over it, in a frame, taken by Enter or a click.

namespace {

using namespace std::chrono_literals;

using tal::Code;
using tal::Document;
using tal::Hooks;
using tal::Key;
using tal::Select;
using tal::Size;
using tal::testing::Scene;

// A column that keeps the keys nobody below took.
class Catcher : public tal::Column {
public:
    std::vector<Code> fell;

    // Hears the keys last, and lets them go on.
    Catcher() {
        hooks_.add(otherwise<tal::KeyEvent>([this](tal::KeyEvent& what) { fell.push_back(what.key.code()); }));
    }

private:
    Hooks hooks_;
};

// A selection of `options` on a full window, `down` rows from the top of a screen of `size`, after
// a label of two cells; it has the focus, and its changes are counted.
struct Shown {
    Scene scene;
    Document document{scene.time()};
    tal::Layers* layers = nullptr;
    Catcher* catcher = nullptr;
    Select* select = nullptr;
    std::size_t changes = 0;
    Hooks hooks;

    explicit Shown(std::vector<std::string> options, Size size = {20, 8}, int down = 0, std::size_t chosen = 0) : scene(size) {
        layers = &document.children().add<tal::Layers>();

        auto made = std::make_shared<tal::Window>();

        made->add_class("screen");
        made->full = true;

        tal::Window& screen = layers->open(made);

        catcher = &screen.children().add<Catcher>();

        for (int at = 0; at < down; ++at) (void)catcher->children().add<tal::Label>(".");

        tal::Row& row = catcher->children().add<tal::Row>();

        (void)row.children().add<tal::Label>("> ");
        select = &row.children().add<Select>(std::move(options), chosen);
        select->share(tal::Share{1, 0});
        hooks.add(select->on<tal::Changed>([this](tal::Changed& what) {
            if (what.of(select->chosen)) ++changes;
        }));
        scene.show(document);
        select->focus();
        scene.wait(0ms);
    }

    // Presses `key` and lets the frame go.
    void press(Key key) {
        scene.press(key);
        scene.wait(0ms);
    }
};

}

// One row: the chosen option cut at its end where it does not fit, a space and the mark at the
// right edge, reversed in focus and faint when disabled. Alone it asks for the widest option and
// two cells; a choice past the end is the last option.
TRIAL("Select: the chosen option cut, the mark, the states") {
    Shown shown{{"Shelves with covers", "List", "Table"}};

    SAME(shown.scene.row(0), std::string{"> Shelves with co\xe2\x80\xa6 \xe2\x96\xbc"});
    OUGHT(shown.scene.at({2, 0}).look.reverse);
    OUGHT(shown.scene.at({19, 0}).look.reverse);
    OUGHT(!shown.scene.at({0, 0}).look.reverse);

    shown.select->enable(false);
    shown.scene.wait(0ms);

    OUGHT(!shown.scene.at({2, 0}).look.reverse);
    OUGHT(shown.scene.at({2, 0}).look.faint);

    Shown narrow{{"one"}, {4, 8}};

    SAME(narrow.scene.row(0), std::string{">  \xe2\x96\xbc"});

    Shown thin{{"one"}, {3, 8}};

    SAME(thin.scene.row(0), std::string{"> \xe2\x96\xbc"});

    Select alone{{"ab", "abcd"}, 9};

    OUGHT(alone.measured(tal::Limits{{0, 0}, {40, 5}}) == (Size{6, 1}));
    SAME(*alone.chosen, std::size_t{1});
    SAME(alone.options().size(), std::size_t{2});
}

// The side arrows, Home and End choose at once and tell `Changed`; at an end of the options, and
// for up, down, letters and keys with Ctrl, the key goes on to the nodes above. Enter and the space
// bar drop the list, and Esc closes it with the focus back on the selection.
TRIAL("Select: keys choose at once, Enter and space drop the list") {
    Shown shown{{"one", "two", "three"}};
    Select& select = *shown.select;

    shown.press(Key{Code::right});
    shown.press(Key{Code::right});

    SAME(*select.chosen, std::size_t{2});
    SAME(shown.changes, std::size_t{2});
    OUGHT(shown.scene.row(0).starts_with("> three"));
    OUGHT(shown.catcher->fell.empty());

    shown.press(Key{Code::right});
    shown.press(Key{Code::end});

    OUGHT(shown.catcher->fell == (std::vector<Code>{Code::right, Code::end}));

    shown.press(Key{Code::home});
    shown.press(Key{Code::left});
    shown.press(Key{Code::home});

    SAME(*select.chosen, std::size_t{0});
    SAME(shown.changes, std::size_t{3});

    shown.press(Key{Code::end});
    shown.press(Key{Code::left});

    SAME(*select.chosen, std::size_t{1});

    shown.catcher->fell.clear();
    shown.press(Key{Code::up});
    shown.press(Key{Code::down});
    shown.press(Key{U'x'});
    shown.press(Key{Code::right, tal::Mods{.ctrl = true}});

    SAME(shown.catcher->fell.size(), std::size_t{4});
    SAME(*select.chosen, std::size_t{1});

    shown.press(Key{Code::enter});

    OUGHT(select.opened());

    shown.press(Key{Code::escape});

    OUGHT(!select.opened());
    OUGHT(select.focused());

    shown.press(Key{U' '});

    OUGHT(select.opened());

    shown.press(Key{Code::escape});
    select.open();
    select.open();
    shown.press(Key{Code::escape});

    OUGHT(!select.opened());
    OUGHT(shown.layers->top() == shown.catcher->parent());

    select.open();
    select.close();
    select.close();

    OUGHT(!select.opened());

    shown.scene.wheel({5, 0}, 1);
    shown.scene.wait(0ms);

    OUGHT(!select.opened());
    SAME(*select.chosen, std::size_t{1});
}

// The list stands under the row, as wide as it, in a thin frame, the chosen option current; a list
// that does not fit under the row stands over it, and one that fits neither way moves up as far as
// it must. A list wider than the room left moves left; more options than `rows` get a bar.
TRIAL("Select: the list under the row, over it, moved left, with a bar") {
    {
        Shown shown{{"one", "two", "three"}, {20, 8}, 0, 1};

        shown.press(Key{Code::enter});

        OUGHT(shown.scene.row(1).starts_with("  \xe2\x94\x8c"));
        OUGHT(shown.scene.row(1).ends_with("\xe2\x94\x90"));
        OUGHT(shown.scene.row(3).starts_with("  \xe2\x94\x82two "));
        OUGHT(shown.scene.row(5).starts_with("  \xe2\x94\x94"));
        OUGHT(shown.scene.at({3, 3}).look.reverse);
        OUGHT(!shown.scene.at({3, 2}).look.reverse);
    }

    {
        Shown shown{{"one", "two", "three"}, {20, 14}, 6};

        shown.press(Key{Code::enter});

        SAME(shown.scene.at({2, 7}).cluster, std::string{"\xe2\x94\x8c"});
        SAME(shown.scene.at({2, 11}).cluster, std::string{"\xe2\x94\x94"});
    }

    {
        Shown shown{{"one", "two", "three"}, {20, 8}, 6};

        shown.press(Key{Code::enter});

        SAME(shown.scene.at({2, 1}).cluster, std::string{"\xe2\x94\x8c"});
        SAME(shown.scene.at({2, 5}).cluster, std::string{"\xe2\x94\x94"});
        OUGHT(shown.scene.row(6).starts_with("> one"));
    }

    {
        Shown shown{{"one", "two", "three"}, {20, 6}, 2};

        shown.press(Key{Code::enter});

        SAME(shown.scene.at({2, 1}).cluster, std::string{"\xe2\x94\x8c"});
        SAME(shown.scene.at({2, 5}).cluster, std::string{"\xe2\x94\x94"});
    }

    {
        Shown shown{{"abc", "abcdefghijklmnopqrst"}};

        shown.press(Key{Code::enter});

        OUGHT(shown.scene.row(1).starts_with("\xe2\x94\x8c"));
        OUGHT(shown.scene.row(1).ends_with("\xe2\x94\x90"));
        OUGHT(shown.scene.row(2).starts_with("\xe2\x94\x82" "abc "));
    }

    {
        Shown shown{{"abcdefghijklmnop", "b", "c"}};

        shown.select->rows = 2;
        shown.press(Key{Code::enter});

        SAME(shown.scene.at({1, 1}).cluster, std::string{"\xe2\x94\x8c"});
        OUGHT(shown.scene.row(2).find("abcdefghijklmnop") != std::string::npos);
        SAME(shown.scene.at({18, 2}).cluster, std::string{"\xe2\x96\x88"});
    }

    {
        Shown shown{{"one", "two", "three"}};

        shown.select->rows = 3;
        shown.press(Key{Code::enter});

        SAME(shown.scene.at({18, 2}).cluster, std::string{" "});
        OUGHT(shown.scene.row(5).starts_with("  \xe2\x94\x94"));
    }

    Shown shown{{"one", "two", "three"}};

    shown.select->rows = 2;
    shown.press(Key{Code::enter});

    OUGHT(shown.scene.row(4).starts_with("  \xe2\x94\x94"));
    SAME(shown.scene.at({18, 2}).cluster, std::string{"\xe2\x96\x88"});
    SAME(shown.scene.at({18, 3}).cluster, std::string{"\xe2\x94\x82"});

    shown.press(Key{Code::escape});
    shown.select->rows = 0;
    shown.press(Key{Code::enter});

    OUGHT(shown.scene.row(3).starts_with("  \xe2\x94\x94"));
}

// Enter on a row of the list or a click on it chooses that option and closes the list, the focus
// back on the selection; Esc and a press beside the list close it and choose nothing. Taking the
// option already chosen tells nothing.
TRIAL("Select: Enter or a click chooses, Esc or a press beside closes") {
    Shown shown{{"one", "two", "three"}};
    Select& select = *shown.select;

    shown.press(Key{Code::enter});
    shown.press(Key{Code::down});
    shown.press(Key{Code::enter});

    OUGHT(!select.opened());
    OUGHT(select.focused());
    SAME(*select.chosen, std::size_t{1});
    SAME(shown.changes, std::size_t{1});

    shown.press(Key{Code::enter});
    shown.scene.click({4, 4});
    shown.scene.wait(0ms);

    OUGHT(!select.opened());
    SAME(*select.chosen, std::size_t{2});

    shown.press(Key{Code::enter});
    shown.press(Key{Code::up});
    shown.press(Key{Code::escape});

    SAME(*select.chosen, std::size_t{2});

    shown.scene.click({10, 0});
    shown.scene.wait(0ms);

    OUGHT(select.opened());

    shown.scene.click({0, 7});
    shown.scene.wait(0ms);

    OUGHT(!select.opened());
    SAME(*select.chosen, std::size_t{2});

    shown.press(Key{Code::enter});
    shown.press(Key{Code::enter});

    SAME(shown.changes, std::size_t{2});

    shown.press(Key{Code::enter});
    shown.press(Key{Code::home});
    shown.press(Key{Code::enter});

    SAME(*select.chosen, std::size_t{0});
    SAME(shown.changes, std::size_t{3});
}

// A disabled selection takes no keys and drops nothing, and disabling it or a node above it closes
// its list at once, choosing nothing. Taken off the tree, the list closes at the next key or touch
// of the mouse. A selection without options takes the focus, shows the mark alone and lets every
// key go on.
TRIAL("Select: disabled, gone, and without options") {
    {
        Shown shown{{"one", "two"}};
        Select& select = *shown.select;

        select.enable(false);
        shown.scene.wait(0ms);
        select.open();
        shown.scene.click({3, 0});
        shown.scene.wait(0ms);

        OUGHT(!select.opened());

        select.enable(true);
        shown.scene.wait(0ms);
        select.focus();
        shown.press(Key{Code::enter});
        shown.press(Key{Code::down});
        select.enable(false);

        OUGHT(!select.opened());

        select.enable(true);
        shown.scene.wait(0ms);
        select.focus();
        shown.press(Key{Code::enter});
        select.parent()->enable(false);

        OUGHT(!select.opened());
        SAME(*select.chosen, std::size_t{0});
        SAME(shown.changes, std::size_t{0});
    }

    {
        Shown shown{{"one", "two"}};

        shown.press(Key{Code::enter});

        tal::Node& row = *shown.select->parent();
        std::shared_ptr<tal::Node> gone = row.children().remove(*shown.select);

        OUGHT(gone != nullptr);

        tal::Window* list = shown.layers->top();

        shown.press(Key{Code::down});

        OUGHT(shown.layers->top() != list);

        shown.press(Key{Code::enter});
        gone.reset();
        shown.press(Key{Code::down});
        shown.press(Key{Code::enter});
    }

    {
        Shown shown{{"one", "two"}};

        shown.press(Key{Code::enter});

        tal::Window* list = shown.layers->top();
        const std::shared_ptr<tal::Node> gone = shown.select->parent()->children().remove(*shown.select);

        shown.document.mouse(tal::Press::move, tal::MouseButton::none, {4, 3});
        shown.scene.wait(0ms);

        OUGHT(gone != nullptr);
        OUGHT(shown.layers->top() != list);
    }

    Shown empty{{}};

    OUGHT(empty.select->focused());
    SAME(empty.scene.row(0), std::string{">                  \xe2\x96\xbc"});

    empty.press(Key{Code::enter});
    empty.press(Key{Code::right});
    empty.select->open();

    OUGHT(!empty.select->opened());
    OUGHT(empty.catcher->fell == (std::vector<Code>{Code::enter, Code::right}));
}
