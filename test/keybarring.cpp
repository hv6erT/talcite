#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/encoding.hpp"
#include "talcite/keybaritem.hpp"
#include "talcite/keybar.hpp"
#include "talcite/menu.hpp"
#include "talcite/menuitem.hpp"
#include "talcite/menubar.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

using namespace talcite;

// The key bar is the outer window and everything else sits in its content, so almost every trial
// starts the same way: a root, a window, something inside that takes the focus.
#include "doing.hpp"

namespace {

// A focusable node of a fixed size that paints its area with one letter and counts the clicks it
// receives.
class Box : public ui::Node {
public:
    Box(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    // Always focusable, so that it can hold the focus while the key bar's keys are pressed.
    [[nodiscard]] bool focusable() const override { return true; }

    // The fixed size, whatever space is offered.
    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

    int hit = 0;

protected:
    // Fills the whole given size with the letter, shifted to the visible part.
    View paint(ui::Size given, ui::Area seen) override {
        View made{seen.size};

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                made.put({x - seen.at.x, y - seen.at.y}, what_, Style{});

        return made;
    }

    // Counts a click that reached this box.
    bool handle(ui::ClickEvent&) override {
        ++hit;

        return true;
    }

private:
    ui::Size wants_;
    char32_t what_ = U'?';
};

// Reads one row of a view as text, with trailing spaces removed.
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

TRIAL("KeyBar: the items share the row and touch side by side") {
    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();

    (void)window->add(ui::Code::f1, "Kopiuj");
    (void)window->add(ui::Code::f2, "Wyjdź");
    top.children().add(window);

    const View made = top.render({{0, 0}, {16, 4}});

    same(read(made, 3), std::string{"1Kopiuj 2Wyjdź"});

    // The items share the width equally and stand one next to another.
    same(window->items()[0]->area(), (ui::Area{{0, 3}, {8, 1}}));
    same(window->items()[1]->area(), (ui::Area{{8, 3}, {8, 1}}));
}

// The remainder of the division goes to the items from the left, so the bar fills the row to the
// cell and no stub is left at the right edge.
TRIAL("KeyBar: the remainder of the division goes to the items from the left") {
    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();

    for (int at = 0; at < 3; ++at) (void)window->add(ui::Code::f1, "");

    top.children().add(window);

    (void)top.render({{0, 0}, {11, 4}});

    same(window->items()[0]->area(), (ui::Area{{0, 3}, {4, 1}}));
    same(window->items()[1]->area(), (ui::Area{{4, 3}, {4, 1}}));
    same(window->items()[2]->area(), (ui::Area{{8, 3}, {3, 1}}));
}

// The tag is derived from the key rather than standing next to it in a separate field; otherwise
// one could write a key bar that lies about what to press.
TRIAL("KeyBar: an item does not take the focus, even when enabled") {
    ui::KeyBar window;

    const auto one = window.add(ui::Code::f5, "Kopiuj");

    ought(one->enabled());
    ought(!one->focusable());
}

TRIAL("KeyBar: the tag comes from the key") {
    same((ui::KeyBarItem{ui::Code::f1, ""}.tag()), std::string{"1"});
    same((ui::KeyBarItem{ui::Code::f5, ""}.tag()), std::string{"5"});
    same((ui::KeyBarItem{ui::Code::f10, ""}.tag()), std::string{"10"});
    same((ui::KeyBarItem{ui::Code::f12, ""}.tag()), std::string{"12"});
    same((ui::KeyBarItem{U'q', ""}.tag()), std::string{"q"});

    // Escape and the other named keys have no tag in Midnight Commander.
    same((ui::KeyBarItem{ui::Code::escape, ""}.tag()), std::string{});
    same((ui::KeyBarItem{ui::Key{}, ""}.tag()), std::string{});
}

TRIAL("KeyBar: the key bubbles up from the focus") {
    proving::Hooks heard;

    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');

    int done = 0;

    window->content(body);
    (void)proving::doing(heard, window->add(ui::Code::f5, "Kopiuj"), [&done] { ++done; });
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {16, 4}});

    ui::KeyEvent what{ui::Code::f5};

    ought(top.dispatch(what));
    same(done, 1);

    // A key the key bar does not hold is none of its business.
    ui::KeyEvent other{ui::Code::f6};

    ought(!top.dispatch(other));
    same(done, 1);
}

// Ctrl+F5 and Alt+F5 are by design different shortcuts from F5, and the key bar does not model
// layers under modifiers, so it does not take them instead of pretending they are the same.
TRIAL("KeyBar: does not catch a key with a modifier") {
    proving::Hooks heard;

    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');

    int done = 0;

    window->content(body);
    (void)proving::doing(heard, window->add(ui::Code::f5, "Kopiuj"), [&done] { ++done; });
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {16, 4}});

    ui::KeyEvent held{ui::Key{ui::Code::f5, ui::Mods{true, false, false}}};

    ought(!top.dispatch(held));

    ui::KeyEvent aside{ui::Key{ui::Code::f5, ui::Mods{false, true, false}}};

    ought(!top.dispatch(aside));

    same(done, 0);
}

// A click does the same as the key and does not take the focus: after clicking "Kopiuj" (copy),
// what was selected is to be copied, not whatever happened to get the focus.
TRIAL("KeyBar: a click does the same and does not take the focus") {
    proving::Hooks heard;

    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');

    int done = 0;

    window->content(body);
    (void)proving::doing(heard, window->add(ui::Code::f5, "Kopiuj"), [&done] { ++done; });
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {16, 4}});

    ui::ClickEvent what{{3, 3}, ui::Mouse::left};

    ought(top.dispatch(what));
    same(done, 1);
    same(top.focused_node(), static_cast<ui::Node*>(body.get()));
}

TRIAL("KeyBar: a disabled item stays silent on a key and on a click") {
    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');
    const auto one = window->add(ui::Code::f5, "Kopiuj");

    int done = 0;

    const ui::Hook heard_pick = one->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;
        return true;
    });
    one->enabled(false);

    window->content(body);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {16, 4}});

    ui::KeyEvent what{ui::Code::f5};

    ought(!top.dispatch(what));

    ui::ClickEvent where{{3, 3}, ui::Mouse::left};

    // The item takes the click (because it was hit), but does not perform its action.
    ought(top.dispatch(where));

    same(done, 0);
}

TRIAL("KeyBar: the content gets all rows but the last") {
    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();
    const auto body = std::make_shared<Box>(ui::Size{ui::plenty, ui::plenty}, U'x');

    window->content(body);
    (void)window->add(ui::Code::f1, "Raz");
    top.children().add(window);

    const View made = top.render({{0, 0}, {6, 3}});

    same(read(made, 0), std::string{"xxxxxx"});
    same(read(made, 1), std::string{"xxxxxx"});
    same(read(made, 2), std::string{"1Raz"});
}

// An empty bar is a bar that is not there, not an empty strip at the bottom of the screen.
TRIAL("KeyBar: without items it takes no row") {
    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();
    const auto body = std::make_shared<Box>(ui::Size{ui::plenty, ui::plenty}, U'x');

    window->content(body);
    top.children().add(window);

    const View made = top.render({{0, 0}, {6, 3}});

    same(read(made, 2), std::string{"xxxxxx"});
}

TRIAL("KeyBar: an item that is too narrow cuts the name, then the tag") {
    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();

    (void)window->add(ui::Code::f1, "Kopiuj");
    (void)window->add(ui::Code::f2, "Wyjdź");
    top.children().add(window);

    const View made = top.render({{0, 0}, {8, 2}});

    // Four cells per item: the tag and three for the name, with an ellipsis.
    same(read(made, 1), std::string{"1Ko…2Wy…"});

    // With one cell per item only the tag remains, and nothing goes outside its own area.
    const View tight = top.render({{0, 0}, {2, 2}});

    same(read(tight, 1), std::string{"12"});
}

// A letter regardless of case: the same rule as in a button and in a menu. With F keys there is
// nothing to fold, but an item registered with a letter is to answer to both its forms.
TRIAL("KeyBar: a letter answers regardless of case") {
    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');

    int said = 0;

    const auto one = window->add(ui::Key{U'Q'}, "Wyjdź");

    const ui::Hook heard_pick = one->on<ui::PickEvent>([&said](ui::PickEvent&) {
        ++said;

        return true;
    });

    window->content(body);
    top.children().add(window);

    body->focus();

    ui::KeyEvent small{U'q'};

    ought(top.dispatch(small));
    same(said, 1);

    ui::KeyEvent big{U'Q'};

    ought(top.dispatch(big));
    same(said, 2);
}

TRIAL("KeyBar: clearing lets the bar be put together again") {
    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();

    (void)window->add(ui::Code::f1, "Raz");
    (void)window->add(ui::Code::f2, "Dwa");
    top.children().add(window);

    same(static_cast<int>(window->items().size()), 2);
    same(window->capacity(), 3);

    window->clear();

    ought(window->items().empty());
    same(window->capacity(), 1);

    (void)window->add(ui::Code::f3, "Trzy");

    const View made = top.render({{0, 0}, {8, 2}});

    same(read(made, 1), std::string{"3Trzy"});
}

// The key bar is the outer window, and the menu bar sits in its content. Thanks to that, an open
// menu gets the rows down to the second to last and has no way to paint over the key bar, as in
// Midnight Commander.
TRIAL("KeyBar: an open menu does not paint over the key bar") {
    ui::Root top;

    const auto bar = std::make_shared<ui::KeyBar>();
    const auto menus = std::make_shared<ui::Menubar>();

    menus->keys().add(ui::Key{ui::Code::f9});
    const auto one = menus->add("Plik", ui::MenuShape{ui::Border::thin, false});

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    one->children().add(std::make_shared<ui::MenuItem>("Wyjście"));
    one->children().add(std::make_shared<ui::MenuItem>("Jeszcze"));

    bar->content(menus);
    (void)bar->add(ui::Code::f5, "Kopiuj");
    top.children().add(bar);

    one->open();

    const View made = top.render({{0, 0}, {12, 7}});

    ought(one->opened());

    // The last row belongs to the key bar and nothing touches it.
    same(read(made, 6), std::string{"5Kopiuj"});
}

// An open menu is modal: a key it does not handle itself is not to go further up. Without that, F5
// with an open menu would start copying.
TRIAL("KeyBar: with a menu open, the key bar's key does not work") {
    proving::Hooks heard;

    ui::Root top;

    const auto bar = std::make_shared<ui::KeyBar>();
    const auto menus = std::make_shared<ui::Menubar>();

    menus->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');
    const auto one = menus->add("Plik");

    int done = 0;

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));

    menus->content(body);
    bar->content(menus);
    (void)proving::doing(heard, bar->add(ui::Code::f5, "Kopiuj"), [&done] { ++done; });
    top.children().add(bar);

    body->focus();

    (void)top.render({{0, 0}, {16, 7}});

    // With the menu closed, the key bar works.
    ui::KeyEvent first{ui::Code::f5};

    ought(top.dispatch(first));
    same(done, 1);

    // F9 takes the bar, and opening a menu is chosen separately; only an open menu is modal.
    ui::KeyEvent open{ui::Code::f9};

    ought(top.dispatch(open));

    ui::KeyEvent drop{ui::Code::down};

    ought(top.dispatch(drop));
    ought(one->opened());

    ui::KeyEvent second{ui::Code::f5};

    ought(top.dispatch(second));
    same(done, 1);
}

// --- Group ---------------------------------------------------------------------------------------

// The item is made by the bar, not by the caller: then the role, the place in the tree and the
// place in capacity() all come from one call and cannot drift apart.
TRIAL("KeyBar: a removed item also leaves the tree") {
    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();
    const auto one = window->add(ui::Code::f1, "Kopiuj");
    const auto two = window->add(ui::Code::f2, "Wyjdź");

    top.children().add(window);

    same(window->items().size(), std::size_t{2});
    ought(window->children().contains(two.get()));

    window->clear();

    ought(window->items().empty());
    ought(!window->children().contains(two.get()));
    ought(!window->children().contains(one.get()));

    // A cleared bar is put together again, and the old items no longer have anything to do with it.
    const auto again = window->add(ui::Code::f1, "Kopiuj");

    ought(window->children().contains(again.get()));

    same(read(top.render({{0, 0}, {16, 4}}), 3), std::string{"1Kopiuj"});
}

TRIAL("KeyBar: the role root of an item can be replaced") {
    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();
    const auto one = window->add(ui::Code::f1, "Kopiuj");

    same(one->role(), std::string{"talcite.keybar"});

    top.children().add(window);

    ui::Styles these;

    Style key{};
    Style text{};

    key.foreground = Color{false, 1, 1, 1};
    text.foreground = Color{false, 2, 2, 2};

    these["moj.pas.tag"] = key;
    these["moj.pas.text"] = text;

    top.skin(ui::Skin{these});
    one->role("moj.pas");

    const View made = top.render({{0, 0}, {16, 4}});

    same(made.at({0, 3}).style.foreground, (Color{false, 1, 1, 1}));
    same(made.at({1, 3}).style.foreground, (Color{false, 2, 2, 2}));
}

// An item has a set of keys, not one key: the first of them draws the tag, but every one answers.
// This is the same ability as in a menu item, and it comes from the same place.
TRIAL("KeyBar: an item answers to each of its keys") {
    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');

    int said = 0;

    const auto one = window->add(ui::Key{ui::Code::f10}, "Wyjdź");

    one->keys().add(ui::Key{U'q'});

    const ui::Hook heard_pick = one->on<ui::PickEvent>([&said](ui::PickEvent&) {
        ++said;

        return true;
    });

    window->content(body);
    top.children().add(window);

    body->focus();

    same(one->tag(), std::string{"10"});

    ui::KeyEvent named{ui::Code::f10};

    ought(top.dispatch(named));
    same(said, 1);

    ui::KeyEvent letter{U'Q'};

    ought(top.dispatch(letter));
    same(said, 2);
}

// The bar hands out items with a shortcut event, so a hook attached from outside answers on equal
// terms with `handle`, just as in a dialog and in a menu.
TRIAL("KeyBar: an item answers a key through a hook attached from outside") {
    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');

    int said = 0;

    const auto one = window->add(ui::Code::f5, "Kopiuj");

    const ui::Hook heard_shortcut = one->on<ui::ShortcutEvent>([&said](ui::ShortcutEvent&) {
        ++said;

        return true;
    });

    window->content(body);
    top.children().add(window);

    body->focus();

    ui::KeyEvent key{ui::Code::f5};

    ought(top.dispatch(key));
    same(said, 1);
}

// The handing out goes only over the bar's items: the content has no right to get a shortcut from
// the key bar, because the bar is beside the layout, not above the content.
TRIAL("KeyBar: the handing out does not enter the content") {
    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');

    int said = 0;

    const ui::Hook heard_shortcut = body->on<ui::ShortcutEvent>([&said](ui::ShortcutEvent&) {
        ++said;

        return true;
    });

    (void)window->add(ui::Code::f5, "Kopiuj");

    window->content(body);
    top.children().add(window);

    body->focus();

    ui::KeyEvent key{ui::Code::f7};

    ought(!top.dispatch(key));
    same(said, 0);
}

// A hot letter replaces the tag rather than standing next to it: an item whose key is a letter
// present in its name draws only the name and underlines that letter in it, like a menu item.
TRIAL("KeyBar: a letter key is underlined in the name instead of a tag") {
    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();
    const auto one = window->add(ui::Key{U'k'}, "Kopiuj");

    top.children().add(window);

    const View made = top.render({{0, 0}, {6, 2}});

    same(read(made, 1), std::string{"Kopiuj"});
    ought(made.at({0, 1}).style.under);
    ought(!made.at({1, 1}).style.under);
}

// A letter that is not in the name has nothing to underline, so the tag comes back; otherwise it
// would be a shortcut nobody learns about.
TRIAL("KeyBar: a letter key not in the name stays a tag") {
    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();
    const auto one = window->add(ui::Key{U'q'}, "Wyjdź");

    top.children().add(window);

    same(one->tag(), std::string{"q"});

    const View made = top.render({{0, 0}, {6, 2}});

    same(read(made, 1), std::string{"qWyjdź"});
}

// A function key has no letter, so the tag always stays, as in Midnight Commander.
TRIAL("KeyBar: a function key keeps its digit") {
    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();

    (void)window->add(ui::Code::f5, "Kopiuj");

    top.children().add(window);

    const View made = top.render({{0, 0}, {7, 2}});

    same(read(made, 1), std::string{"5Kopiuj"});
}

// An item registered with a function key keeps its digit, even when it also has a letter in
// reserve: the bar reads as a sequence of numbers, and a gap in the sequence would be worse than a
// missing underline. The letter still works and is underlined the same way; it just does not cost
// the digit.
TRIAL("KeyBar: the digit stays, though the item also has a letter") {
    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();
    const auto one = window->add(ui::Code::f5, "Kopiuj");

    one->keys().add(ui::Key{U'k'});

    top.children().add(window);

    const View made = top.render({{0, 0}, {7, 2}});

    same(read(made, 1), std::string{"5Kopiuj"});
    ought(made.at({1, 1}).style.under);
    ought(!made.at({2, 1}).style.under);
}

// Modifiers are part of the key, so the bar does not have to filter them: an item registered for F5
// does not answer Ctrl+F5, and an item registered for Ctrl+F5 answers only to it.
TRIAL("KeyBar: an item with a modifier answers only to its own combination") {
    ui::Root top;

    const auto window = std::make_shared<ui::KeyBar>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');

    int plain = 0;
    int held = 0;

    ui::Mods ctrl;
    ctrl.ctrl = true;

    const auto one = window->add(ui::Code::f5, "Kopiuj");
    const auto two = window->add(ui::Key{ui::Code::f5, ctrl}, "Inne");

    const ui::Hook heard_pick_1 = one->on<ui::PickEvent>([&plain](ui::PickEvent&) { ++plain; return true; });
    const ui::Hook heard_pick_2 = two->on<ui::PickEvent>([&held](ui::PickEvent&) { ++held; return true; });

    window->content(body);
    top.children().add(window);

    body->focus();

    ui::KeyEvent bare{ui::Key{ui::Code::f5}};

    ought(top.dispatch(bare));
    same(plain, 1);
    same(held, 0);

    ui::KeyEvent with{ui::Key{ui::Code::f5, ctrl}};

    ought(top.dispatch(with));
    same(plain, 1);
    same(held, 1);
}

// There is one content: a new one displaces the old, and a null pointer empties the slot. The old
// content leaves the tree completely, so it can be put somewhere else.
TRIAL("KeyBar: new content displaces the old, and an empty one empties the slot") {
    ui::KeyBar window;

    const auto one = std::make_shared<Box>(ui::Size{4, 1}, U'a');
    const auto two = std::make_shared<Box>(ui::Size{4, 1}, U'b');

    window.content(one);
    window.content(two);

    same(window.content(), static_cast<ui::Node*>(two.get()));
    ought(one->parent() == nullptr);
    same(read(window.render({{0, 0}, {4, 2}}), 0), std::string{"bbbb"});

    window.content(nullptr);

    ought(window.content() == nullptr);
    ought(two->parent() == nullptr);
}
