#include "proving.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "talcite/encoding.hpp"
#include "talcite/menu.hpp"
#include "talcite/menuitem.hpp"
#include "talcite/menuseparator.hpp"
#include "talcite/menubar.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"
#include "talcite/submenu.hpp"

using namespace talcite;

#include "doing.hpp"

namespace {

// The bar's unrolled menu. This is the trial's question, not the library's: the bar says which menu
// is current, and the menu says whether it is unrolled. The bar has no separate `opened()`, since
// that would be a second way to the same answer.
ui::Menu* unfolded(const ui::Menubar& bar) {
    ui::Menu* one = bar.current();

    return one != nullptr && one->opened() ? one : nullptr;
}

// A focusable box that asks for a fixed size, fills it with one letter and counts the clicks it
// takes.
class Box : public ui::Node {
public:
    Box(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    // The box can take the focus.
    [[nodiscard]] bool focusable() const override { return true; }

    // The size given in the constructor, whatever room is offered.
    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

    int hit = 0;

protected:
    // Fills the whole given size with the box's letter, shifted by the visible part.
    void paint(Canvas& onto) const override {
        const ui::Size given = onto.size();

        onto.fill({{0, 0}, given}, Style{});

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                onto.put({x, y}, what_, Style{});
    }

    // Counts the click and takes it.
    bool handle(ui::ClickEvent&) override {
        ++hit;

        return true;
    }

private:
    ui::Size wants_;
    char32_t what_ = U'?';
};

// The text of row `y` of the view, with trailing spaces removed.
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

TRIAL("Menu: a closed one is just the title in row zero") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    top.children().add(window);

    const View made = top.render({{0, 0}, {20, 6}});

    same(read(made, 0), std::string{" Plik"});
    same(read(made, 1), std::string{""});

    // A rolled-up menu is outside the tree: it has no parent and no rectangle. What shows in row
    // zero is its title, a separate node in the row of titles.
    ought(!one->opened());
    ought(one->parent() == nullptr);
}

// Emptying the bar, just as in the key bar and for the same reason: switching context removes the
// old menus and lets in new ones, and `capacity()` has to go hand in hand with the tree.
TRIAL("Menu: emptying the bar allows putting it together anew") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});

    (void)window->add("Plik");
    (void)window->add("Widok");
    top.children().add(window);

    same(static_cast<int>(window->menus().size()), 2);

    window->clear();

    ought(window->menus().empty());
    same(window->children().many(), std::size_t{1});

    (void)window->add("Pomoc");

    const View made = top.render({{0, 0}, {20, 6}});

    same(read(made, 0), std::string{" Pomoc"});
}

TRIAL("Menu: the titles stand side by side") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});

    (void)window->add("Plik");
    (void)window->add("Widok");
    top.children().add(window);

    const View made = top.render({{0, 0}, {20, 6}});

    same(read(made, 0), std::string{" Plik  Widok"});
}

// F9 takes the bar instead of unrolling a menu. Only the down arrow, Enter or a title's letter
// unrolls, as in Midnight Commander with drop_menus off. If F9 unrolled the first menu before the
// user said which, the unrolled menu would swallow the letters of its items, and only what none of
// them wanted would reach the titles.
TRIAL("Menu: F9 takes the bar, down unrolls, Esc closes") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    window->content(body);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {20, 6}});

    ui::KeyEvent open{ui::Code::f9};

    ought(top.dispatch(open));
    same(window->current(), one.get());
    ought(!one->opened());

    ui::KeyEvent drop{ui::Code::down};

    ought(top.dispatch(drop));
    ought(one->opened());

    ui::KeyEvent close{ui::Code::escape};

    ought(top.dispatch(close));
    ought(!one->opened());
}

// The menu rolls itself up, the pair of `open`: the focus goes back to its title, and the next
// unrolling starts from the top. The bar does not reach into the menu's item number.
TRIAL("Menu: close rolls up, gives the focus to the title and forgets the item") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");
    const auto second = std::make_shared<ui::MenuItem>("Przenieś");

    one->children().add(first);
    one->children().add(std::make_shared<ui::MenuSeparator>());
    one->children().add(second);
    window->content(body);
    top.children().add(window);

    body->focus();
    (void)top.render({{0, 0}, {20, 6}});

    one->open();
    one->which(2);

    ought(second->focused());

    one->close();

    ought(!one->opened());
    same(window->current(), one.get());
    ought(!one->which().has_value());

    one->open();

    ought(first->focused());

    // The line is not an item: the arrow skips it, and on a closed menu close breaks nothing.
    ui::KeyEvent down{ui::Code::down};

    ought(top.dispatch(down));
    ought(second->focused());

    window->leave();
    one->close();

    ought(!one->opened());
    ought(body->focused());

    // A rolled-up menu keeps the remembered item: it starts from it when unrolled.
    one->which(2);
    one->close();

    same(one->which(), std::optional<std::size_t>{2});

    one->open();

    ought(second->focused());
}

// `dropdown()` unrolls from the start, like Midnight Commander with "Drop down menus" on: F9 shows
// the first menu at once, a side arrow moves to the neighbor unrolled, Esc closes everything.
// Without the flag the behavior is as above. The program sets the flag from a setting.
TRIAL("Menu: with dropdown() F9 unrolls the first menu, and a side arrow unrolls the neighbor") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto first = window->add("Plik");
    const auto second = window->add("Widok");

    first->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    second->children().add(std::make_shared<ui::MenuItem>("Drzewo"));
    window->content(body);
    top.children().add(window);

    ought(!window->dropdown());

    window->dropdown(true);

    ought(window->dropdown());

    body->focus();

    (void)top.render({{0, 0}, {20, 6}});

    ui::KeyEvent open{ui::Code::f9};

    ought(top.dispatch(open));
    same(window->current(), first.get());
    ought(first->opened());
    ought(window->expanded());

    ui::KeyEvent right{ui::Code::right};

    ought(top.dispatch(right));
    ought(!first->opened());
    ought(second->opened());

    ui::KeyEvent close{ui::Code::escape};

    ought(top.dispatch(close));
    ought(!second->opened());
    ought(window->current() == nullptr);
    ought(body->focused());

    // The second way in, through enter(), as from the key bar's field, unrolls too.
    window->enter();

    ought(first->opened());

    window->leave();

    // Without the flag, as before: just the title.
    window->dropdown(false);

    ought(top.dispatch(open));
    same(window->current(), first.get());
    ought(!first->opened());

    ought(top.dispatch(close));
}

// `open()` is the public way into the bar, the same that F9 does when the bar is idle, the pair of
// `close()`. It is needed because the key bar's field calls a method rather than pressing a key;
// calling `focus()` on the menu instead would unroll the first menu, and a click would do something
// other than F9. A bar that is already active, with the focus on the second title or with a menu
// unrolled, stays as it is.
TRIAL("Menu: open() takes a rolled-up bar and leaves an active one alone") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto first = window->add("Plik");
    const auto second = window->add("Widok");

    first->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    second->children().add(std::make_shared<ui::MenuItem>("Drzewo"));
    window->content(body);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {20, 6}});

    window->enter();

    same(window->current(), first.get());
    ought(unfolded(*window) == nullptr);
    ought(!window->expanded());
    ought(!body->focused());

    // An active bar: the focus on the second title stays where it was.
    ui::KeyEvent right{ui::Code::right};

    ought(top.dispatch(right));
    same(window->current(), second.get());

    window->enter();

    same(window->current(), second.get());
    ought(unfolded(*window) == nullptr);

    // An unrolled menu stays unrolled too.
    second->open();

    ought(second->opened());

    window->enter();

    ought(second->opened());
    ought(window->expanded());

    // Closing gives the focus back to the content, as after F9.
    window->leave();

    ought(body->focused());
}

// The unrolled bit lives in the bar, not in the focus, so it survives the bar being detached from
// the tree: `Root::losing` clears the focus without an event, and the bit stays. So entering has to
// clear it itself; otherwise a bar attached again would enter "rolled up" with the unrolled bit
// set, and the first side arrow would unroll the neighbor. The state comes from walking through an
// empty title: the bit is set, and there is no menu.
TRIAL("Menu: a bar attached again enters rolled up, though it was detached with the unrolled bit") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto first = window->add("Plik");
    const auto second = window->add("Puste");

    first->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    window->content(body);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {20, 6}});

    first->open();

    ui::KeyEvent right{ui::Code::right};

    ought(top.dispatch(right));
    same(window->current(), second.get());
    ought(unfolded(*window) == nullptr);
    ought(window->expanded());

    ought(top.children().remove(window.get()));
    ought(top.focused_node() == nullptr);

    top.children().add(window);
    body->focus();

    ui::KeyEvent open{ui::Code::f9};

    ought(top.dispatch(open));
    same(window->current(), first.get());
    ought(!window->expanded());
}

TRIAL("Menu: an open one draws a box with the items") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik", ui::MenuShape{ui::Border::thin, false});

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj", "F5"));
    one->children().add(std::make_shared<ui::MenuItem>("Wyjście", "F10"));
    top.children().add(window);

    one->open();

    const View made = top.render({{0, 0}, {30, 8}});

    same(read(made, 0), std::string{" Plik"});
    same(read(made, 1), std::string{"┌──────────────┐"});
    same(read(made, 2), std::string{"│ Kopiuj    F5 │"});
    same(read(made, 3), std::string{"│ Wyjście  F10 │"});
    same(read(made, 4), std::string{"└──────────────┘"});
}

TRIAL("Menu: the arrows walk the items and skip the line") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");
    const auto second = std::make_shared<ui::MenuItem>("Wyjście");

    one->children().add(first);
    one->children().add(std::make_shared<ui::MenuSeparator>());
    one->children().add(second);
    top.children().add(window);

    one->open();

    // Opening stands on the first item at once, as in Midnight Commander, because opening is
    // putting the focus there, and the focus sits on an item.
    same(one->which(), std::optional<std::size_t>{0});
    same(top.focused_node(), static_cast<ui::Node*>(first.get()));

    ui::KeyEvent down{ui::Code::down};

    ought(top.dispatch(down));
    same(one->which(), std::optional<std::size_t>{2});
    same(top.focused_node(), static_cast<ui::Node*>(second.get()));

    // The menu wraps around: it is short and visible whole, so there is nowhere to get lost.
    ought(top.dispatch(down));
    same(one->which(), std::optional<std::size_t>{0});

    ui::KeyEvent up{ui::Code::up};

    ought(top.dispatch(up));
    same(one->which(), std::optional<std::size_t>{2});
}

// The number and the node of the highlighted item are two names for one thing, as in the list. An
// item from outside the menu sets nothing.
TRIAL("Menu: which is the number, current the item, and a foreign item sets nothing") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");
    const auto second = std::make_shared<ui::MenuItem>("Wyjście");
    const auto stranger = std::make_shared<ui::MenuItem>("Obca");

    one->children().add(first);
    one->children().add(second);
    top.children().add(window);

    ought(!one->which().has_value());
    ought(one->current() == nullptr);

    one->current(*second);

    same(one->which(), std::optional<std::size_t>{1});
    same(one->current(), second.get());

    one->current(*stranger);

    same(one->current(), second.get());

    one->which(0);

    same(one->current(), first.get());

    // A number past the last item is cut to the last, as in a list.
    one->which(99);

    same(one->which(), std::optional<std::size_t>{1});
    same(one->current(), second.get());
}

// A menu without items has nothing to highlight, whatever number it is given.
TRIAL("Menu: which in a menu without items stays empty") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();
    const auto one = window->add("Pomoc");

    top.children().add(window);

    one->which(2);

    ought(!one->which().has_value());
    ought(one->current() == nullptr);
}

// The highlighted item is the one with the focus; there is no separate marker that could drift
// apart from it. That is also why a handler attached to an item sees the key before the menu.
TRIAL("Menu: the highlight is the item's focus") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");
    const auto second = std::make_shared<ui::MenuItem>("Wyjście");

    one->children().add(first);
    one->children().add(second);
    top.children().add(window);

    one->open();

    View made = top.render({{0, 0}, {20, 6}});

    ought(made.at({1, 2}).style.reverse);
    ought(!made.at({1, 3}).style.reverse);

    int seen = 0;

    const ui::Hook heard = second->on<ui::KeyEvent>([&seen](ui::KeyEvent&) {
        ++seen;

        return true;
    });

    one->which(1);

    same(top.focused_node(), static_cast<ui::Node*>(second.get()));

    made = top.render({{0, 0}, {20, 6}});

    ought(!made.at({1, 2}).style.reverse);
    ought(made.at({1, 3}).style.reverse);

    // A handler attached to the item gets the key before the menu, but does not take it away from
    // the menu: true from the handler ends the path of handlers, and the menu's `handle` runs its
    // own and moves the highlight. The widget and the caller do not hide things from each other;
    // each closes only its own channel.
    ui::KeyEvent down{ui::Code::down};

    ought(top.dispatch(down));
    same(seen, 1);
    same(one->which(), std::optional<std::size_t>{0});
}

// --- Hotkeys -------------------------------------------------------------------------------------

// This is how the menu works in Midnight Commander: in the open File menu ("Plik") pressing "w"
// runs Exit ("Wyjście") without walking with the arrows. The menu does not compare keys; it hands
// them out to its items and waits for one of them to take it.
//
// A pick closes the menu, just as Enter and a click do, so the highlight number is cleared
// afterwards; that is what `Menu::pick` is about too.
TRIAL("Menu: a hotkey picks an item in an open menu") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");
    const auto second = std::make_shared<ui::MenuItem>("Wyjście");

    first->keys().add(U'k');
    second->keys().add(U'w');

    one->children().add(first);
    one->children().add(second);
    top.children().add(window);

    int said = 0;

    const ui::Hook heard_pick = second->on<ui::PickEvent>([&said](ui::PickEvent&) {
        ++said;

        return true;
    });

    one->open();

    ui::KeyEvent what{U'w'};

    ought(top.dispatch(what));

    same(said, 1);
    ought(!one->opened());
}

// A closed menu hands out nothing: the letters then belong to the content below the bar.
TRIAL("Menu: a closed one does not take letters") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");

    first->keys().add(U'k');

    one->children().add(first);
    top.children().add(window);

    int said = 0;

    const ui::Hook heard_pick = first->on<ui::PickEvent>([&said](ui::PickEvent&) {
        ++said;

        return true;
    });

    ui::KeyEvent what{U'k'};

    ought(!top.dispatch(what));
    same(said, 0);
}

// A letter that no item took picks nothing; that the open menu swallows it anyway is a rule of the
// bar, not of the item: an unrolled menu is modal, as in MC.
TRIAL("Menu: a letter nobody took picks nothing") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");

    first->keys().add(U'k');

    one->children().add(first);
    top.children().add(window);

    int said = 0;

    const ui::Hook heard_pick = first->on<ui::PickEvent>([&said](ui::PickEvent&) {
        ++said;

        return true;
    });

    one->open();

    ui::KeyEvent what{U'z'};

    (void)top.dispatch(what);

    same(said, 0);
    ought(one->opened());
}

TRIAL("Menu: a grayed-out item does not answer its letter") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");

    first->keys().add(U'k');
    first->enabled(false);

    one->children().add(first);
    top.children().add(window);

    int said = 0;

    const ui::Hook heard_pick = first->on<ui::PickEvent>([&said](ui::PickEvent&) {
        ++said;

        return true;
    });

    one->open();

    ui::KeyEvent what{U'k'};

    (void)top.dispatch(what);

    same(said, 0);
    ought(one->opened());
}

// With Control we do not catch it; the same rule as in the button and in the key bar.
TRIAL("Menu: a letter with a modifier does not pick an item") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");

    first->keys().add(U'k');

    one->children().add(first);
    top.children().add(window);

    int said = 0;

    const ui::Hook heard_pick = first->on<ui::PickEvent>([&said](ui::PickEvent&) {
        ++said;

        return true;
    });

    one->open();

    ui::Mods with;
    with.alt = true;

    ui::KeyEvent what{ui::Key{U'k', with}};

    (void)top.dispatch(what);

    same(said, 0);
}

// The letter is underlined in the text, and the skin gives the role for it; the widget's code knows
// nothing about it.
TRIAL("Menu: a hotkey is underlined in the item's text") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");

    first->keys().add(U'p');

    one->children().add(first);
    top.children().add(window);

    one->open();

    const View made = top.render({{0, 0}, {20, 6}});

    // " Kopiuj": the text stands one cell from the left, so "p" is in the fourth column of the
    // frame.
    same(made.at({4, 2}).letter, U'p');
    ought(made.at({4, 2}).style.under);
    ought(!made.at({3, 2}).style.under);
}

// Graying out the highlighted item gives the focus to the menu: an open menu stays open, only
// nothing in it is highlighted.
TRIAL("Menu: graying out the highlighted item does not close the menu") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto first = std::make_shared<ui::MenuItem>("Kopiuj");

    one->children().add(first);
    top.children().add(window);

    one->open();

    same(top.focused_node(), static_cast<ui::Node*>(first.get()));

    first->enabled(false);

    ought(one->opened());
    same(top.focused_node(), static_cast<ui::Node*>(one.get()));
    ought(one->current() == nullptr);
}

TRIAL("Menu: Enter calls the task and closes") {
    proving::Hooks heard;

    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    int done = 0;

    one->children().add(proving::doing(heard, std::make_shared<ui::MenuItem>("Kopiuj"), [&done] { ++done; }));
    top.children().add(window);

    one->open();

    ui::KeyEvent down{ui::Code::down};

    (void)top.dispatch(down);

    ui::KeyEvent enter{ui::Code::enter};

    ought(top.dispatch(enter));
    same(done, 1);
    ought(!one->opened());
}

TRIAL("Menu: a click picks the item under the cursor, not the highlighted one") {
    proving::Hooks heard;

    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    int first = 0;
    int second = 0;

    one->children().add(proving::doing(heard, std::make_shared<ui::MenuItem>("Kopiuj"), [&first] { ++first; }));
    one->children().add(proving::doing(heard, std::make_shared<ui::MenuItem>("Wyjście"), [&second] { ++second; }));
    top.children().add(window);

    one->open();
    one->which(0);

    (void)top.render({{0, 0}, {30, 8}});

    ui::ClickEvent what{{3, 3}, ui::Mouse::left};

    ought(top.dispatch(what));

    same(first, 0);
    same(second, 1);
    ought(!one->opened());
}

// The focus leaves the bar, so the menu rolls up. Unrolling is attaching, so the bar removes it
// explicitly on `UnfocusEvent`. Leaving is done the way every content widget does it after a
// click, by taking the focus, because the focus, not the click, is the mechanism here.
TRIAL("Menu: the focus leaving the bar rolls up the unrolled menu") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    window->content(body);
    top.children().add(window);

    (void)top.render({{0, 0}, {30, 8}});

    one->open();
    ought(one->opened());

    body->focus();

    ought(!one->opened());
    ought(unfolded(*window) == nullptr);
    ought(!window->expanded());
    ought(body->focused());
}

TRIAL("Menu: a click on the title opens and closes") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    top.children().add(window);

    (void)top.render({{0, 0}, {30, 8}});

    ui::ClickEvent what{{2, 0}, ui::Mouse::left};

    ought(top.dispatch(what));
    ought(one->opened());

    (void)top.render({{0, 0}, {30, 8}});

    ought(top.dispatch(what));
    ought(!one->opened());
}

// A title without items is a button in the bar, not an empty list: it does not unroll, it is
// highlighted like the others when walking with the arrows, and Enter and a click call its own
// `PickEvent`.
TRIAL("Menu: without items it does not unroll, Enter and a click call the title's event") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto left = window->add("Lewy");
    const auto file = window->add("Plik");

    file->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    window->content(body);
    top.children().add(window);

    body->focus();

    int picked = 0;

    const ui::Hook heard_pick = left->on<ui::PickEvent>([&picked](ui::PickEvent&) {
        ++picked;

        return true;
    });

    (void)top.render({{0, 0}, {30, 8}});

    // F9 puts the focus on the first menu, the empty one: the focus is on the menu itself, nothing
    // stands open, nothing lies over the content.
    ui::KeyEvent open{ui::Code::f9};

    ought(top.dispatch(open));
    same(window->current(), left.get());
    ought(!left->opened());

    // Below the title the content shows, not a box.
    const View shut = top.render({{0, 0}, {30, 8}});

    same(read(shut, 1), std::string{"xxxxxxxxxx"});

    // The right arrow goes to the full menu; it does not carry unrolling with it, because the bar
    // entered rolled up. Only the down arrow unrolls, the same rule as with F9.
    ui::KeyEvent right{ui::Code::right};

    ought(top.dispatch(right));
    same(window->current(), file.get());
    ought(!file->opened());

    ui::KeyEvent drop{ui::Code::down};

    ought(top.dispatch(drop));
    ought(file->opened());

    ui::KeyEvent back{ui::Code::left};

    ought(top.dispatch(back));
    same(window->current(), left.get());
    ought(!file->opened());

    // Enter calls the title's event and closes the bar.
    ui::KeyEvent enter{ui::Code::enter};

    ought(top.dispatch(enter));
    same(picked, 1);
    ought(window->current() == nullptr);
    ought(body->focused());
    ought(unfolded(*window) == nullptr);

    // A click on the title does the same, without opening.
    (void)top.render({{0, 0}, {30, 8}});

    ui::ClickEvent what{{2, 0}, ui::Mouse::left};

    ought(top.dispatch(what));
    same(picked, 2);
    ought(!left->opened());
    ought(window->current() == nullptr);
}

TRIAL("Menu: left and right switch without closing") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto first = window->add("Plik");
    const auto second = window->add("Widok");

    first->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    second->children().add(std::make_shared<ui::MenuItem>("Odśwież"));
    top.children().add(window);

    first->open();

    ui::KeyEvent right{ui::Code::right};

    ought(top.dispatch(right));
    ought(!first->opened());
    ought(second->opened());

    ought(top.dispatch(right));
    ought(first->opened());
}

TRIAL("Menu: Esc returns to whoever had the focus") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    window->content(body);
    top.children().add(window);

    body->focus();

    ui::KeyEvent open{ui::Code::f9};

    ought(top.dispatch(open));

    // Esc returns from an active bar just as from an unrolled menu: the bar remembers the focus
    // from before entering when it is first taken, not when unrolling.
    ui::KeyEvent close{ui::Code::escape};

    ought(top.dispatch(close));
    same(static_cast<const ui::Node*>(top.focused_node()), static_cast<const ui::Node*>(body.get()));
}

TRIAL("Menu: moving from menu to menu does not lose the application's focus") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto first = window->add("Plik");
    const auto second = window->add("Widok");

    first->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    second->children().add(std::make_shared<ui::MenuItem>("Odśwież"));
    window->content(body);
    top.children().add(window);

    body->focus();

    ui::KeyEvent open{ui::Code::f9};
    ui::KeyEvent right{ui::Code::right};
    ui::KeyEvent close{ui::Code::escape};

    (void)top.dispatch(open);
    (void)top.dispatch(right);
    (void)top.dispatch(close);

    same(static_cast<const ui::Node*>(top.focused_node()), static_cast<const ui::Node*>(body.get()));
}

TRIAL("Menu: the content stands below the row of titles") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{40, 40}, U'x');

    window->content(body);
    (void)window->add("Plik");
    top.children().add(window);

    const View made = top.render({{0, 0}, {6, 3}});

    same(read(made, 0), std::string{" Plik"});
    same(read(made, 1), std::string{"xxxxxx"});
    same(window->content()->parent()->area().at.y, 1);
    same(window->content()->parent()->area().size.high, 2);
    same(body->area().at.y, 0);
}

TRIAL("Menu: an open one covers the content for hit testing") {
    proving::Hooks heard;

    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{40, 40}, U'x');
    const auto one = window->add("Plik");

    int done = 0;

    one->children().add(proving::doing(heard, std::make_shared<ui::MenuItem>("Kopiuj"), [&done] { ++done; }));
    window->content(body);
    top.children().add(window);

    one->open();

    (void)top.render({{0, 0}, {30, 8}});

    ui::ClickEvent what{{3, 2}, ui::Mouse::left};

    ought(top.dispatch(what));

    same(done, 1);
    same(body->hit, 0);
}

TRIAL("Menu: a closed one does not catch clicks where it was open") {
    proving::Hooks heard;

    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{40, 40}, U'x');
    const auto one = window->add("Plik");

    int done = 0;

    one->children().add(proving::doing(heard, std::make_shared<ui::MenuItem>("Kopiuj"), [&done] { ++done; }));
    window->content(body);
    top.children().add(window);

    one->open();

    (void)top.render({{0, 0}, {30, 8}});

    window->leave();

    (void)top.render({{0, 0}, {30, 8}});

    ui::ClickEvent what{{3, 2}, ui::Mouse::left};

    ought(top.dispatch(what));

    same(done, 0);
    same(body->hit, 1);
}

TRIAL("Menu: the line reaches the frame") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik", ui::MenuShape{ui::Border::thin, false});

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    one->children().add(std::make_shared<ui::MenuSeparator>());
    one->children().add(std::make_shared<ui::MenuItem>("Wyjście"));
    top.children().add(window);

    one->open();

    const View made = top.render({{0, 0}, {30, 9}});

    same(read(made, 3), std::string{"├─────────┤"});
}

TRIAL("Menu: a grayed-out item cannot be picked") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");
    const auto item = std::make_shared<ui::MenuItem>("Kopiuj");

    int done = 0;

    const ui::Hook heard_pick = item->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });
    item->enabled(false);

    one->children().add(item);
    top.children().add(window);

    one->open();

    ui::KeyEvent down{ui::Code::down};

    (void)top.dispatch(down);

    ought(one->current() == nullptr);

    ui::KeyEvent enter{ui::Code::enter};

    (void)top.dispatch(enter);

    same(done, 0);
}

// The box stands below its own title, not below the first one around. This is the only place where
// the menu computes its position (from the title's `standing()`, every frame), and the only trial
// that asks about it beyond the edge case: with the first menu the title stands at zero, so a box
// at zero would look correctly computed even if it were not computed at all.
TRIAL("Menu: the second menu's box stands below the second title") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    (void)window->add("Plik", ui::MenuShape{ui::Border::thin, false});
    const auto second = window->add("Widok", ui::MenuShape{ui::Border::thin, false});

    second->children().add(std::make_shared<ui::MenuItem>("Odśwież"));
    top.children().add(window);

    second->open();

    const View made = top.render({{0, 0}, {30, 6}});

    // " Plik " takes six columns, so "Widok" starts in the sixth, and the left corner stands there.
    same(read(made, 0), std::string{" Plik  Widok"});
    same(made.at({6, 1}).letter, U'┌');
    same(made.at({0, 1}).letter, U' ');
}

TRIAL("Menu: at the right edge it slides out to the left") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto first = window->add("Plik");
    const auto second = window->add("Widok", ui::MenuShape{ui::Border::thin, false});

    first->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    second->children().add(std::make_shared<ui::MenuItem>("Bardzo długa pozycja"));
    top.children().add(window);

    second->open();

    const View made = top.render({{0, 0}, {26, 6}});

    same(second->area().at.x, 2);
    same(read(made, 0), std::string{" Plik  Widok"});
    same(read(made, 1), std::string{"  ┌──────────────────────┐"});
}

TRIAL("Menu: the shadow does not eat what lies below") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{40, 40}, U'x');
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    window->content(body);
    top.children().add(window);

    one->open();

    const View made = top.render({{0, 0}, {30, 8}});

    same(read(made, 1), std::string{"┌────────┐xxxxxxxxxxxxxxxxxxxx"});
    same(made.at({10, 2}).letter, U'x');
    ought(made.at({10, 2}).style.faint);
    ought(!made.at({10, 1}).style.faint);
    same(made.at({2, 4}).letter, U'x');
    ought(made.at({2, 4}).style.faint);
}

TRIAL("Menu: the bar accepts nothing from outside") {
    ui::Menubar window;

    bool threw = false;

    try {
        window.children().add(std::make_shared<Box>(ui::Size{4, 4}, U'a'));
    } catch (const std::logic_error&) {
        threw = true;
    }

    ought(threw);
    same(static_cast<int>(window.children().many()), 1);
}

TRIAL("Menu: the content stands below the bar") {
    ui::Menubar window;

    ought(window.content() == nullptr);

    const auto one = std::make_shared<Box>(ui::Size{8, 4}, U'a');

    window.content(one);

    same(window.content(), static_cast<ui::Node*>(one.get()));

    // The content slot sits in a column below the row of titles, and the column is the first child
    // of the bar's stack.
    same(static_cast<const ui::Node*>(one->parent()->parent()->parent()),
         static_cast<const ui::Node*>(&window));

    const View made = window.render({{0, 0}, {8, 3}});

    same(read(made, 1), std::string{"aaaaaaaa"});
}

TRIAL("Menu: closing clears the highlight") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    top.children().add(window);

    one->open();
    one->which(0);

    window->leave();

    ought(one->current() == nullptr);
}

// --- Hovering with the mouse ---------------------------------------------------------------------

// The highlight follows the cursor, as in Midnight Commander. Motion without a pressed button comes
// only when the application asks for it in `Setup::hover`, but the path itself is the same as for
// any other mouse event, and that path is what is checked here.
TRIAL("Menu: hovering moves the highlight") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    one->children().add(std::make_shared<ui::MenuItem>("Wyjście"));
    top.children().add(window);

    one->open();
    one->which(0);

    (void)top.render({{0, 0}, {30, 8}});

    same(one->which(), std::optional<std::size_t>{0});

    ui::MouseEvent what{{3, 3}, ui::Press::move, ui::Mouse::none};

    (void)top.dispatch(what);

    same(one->which(), std::optional<std::size_t>{1});
}

// Motion belongs to nobody exclusively: the item moves the highlight and lets the event go on, so
// as not to take it away from someone who needs it too.
TRIAL("Menu: hovering does not swallow the event") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    one->children().add(std::make_shared<ui::MenuItem>("Wyjście"));
    top.children().add(window);

    one->open();

    (void)top.render({{0, 0}, {30, 8}});

    ui::MouseEvent what{{3, 3}, ui::Press::move, ui::Mouse::none};

    ought(!top.dispatch(what));
}

TRIAL("Menu: hovering over a disabled item does not move the highlight") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    const auto second = std::make_shared<ui::MenuItem>("Wyjście");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    one->children().add(second);
    top.children().add(window);

    second->enabled(false);

    one->open();
    one->which(0);

    (void)top.render({{0, 0}, {30, 8}});

    ui::MouseEvent what{{3, 3}, ui::Press::move, ui::Mouse::none};

    (void)top.dispatch(what);

    same(one->which(), std::optional<std::size_t>{0});
}

// A closed menu is just the title in the bar: there is no item under the cursor then, so there is
// nothing to highlight and nothing happens.
TRIAL("Menu: hovering over a closed one opens nothing") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    top.children().add(window);

    (void)top.render({{0, 0}, {30, 8}});

    ui::MouseEvent what{{1, 0}, ui::Press::move, ui::Mouse::none};

    (void)top.dispatch(what);

    ought(!one->opened());
}

// --- Group ------------------------------------------------------------------------------------

TRIAL("Menu: a removed menu disappears from the tree too") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto first = window->add("Plik");
    const auto second = window->add("Widok");

    top.children().add(window);

    same(window->menus().size(), std::size_t{2});

    // A rolled-up menu is not in the tree; an unrolled one is a child of the bar, and only then.
    ought(!window->children().contains(second.get()));

    second->children().add(std::make_shared<ui::MenuItem>("Odśwież"));
    second->open();

    ought(window->children().contains(second.get()));

    window->clear();

    ought(window->menus().empty());
    ought(!window->children().contains(second.get()));
    ought(!window->children().contains(first.get()));

    // An emptied bar is put together anew, and the old menus have nothing to do with it anymore.
    const auto again = window->add("Plik");

    same(window->menus().size(), std::size_t{1});
    same(read(top.render({{0, 0}, {26, 6}}), 0), std::string{" Plik"});
}

TRIAL("Menu: the stem of the title's role can be replaced") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik");

    same(one->role(), std::string{"talcite.menubar"});

    top.children().add(window);

    ui::Styles these;

    Style bar{};

    bar.foreground = Color{false, 3, 3, 3};

    these["moj.pasek"] = bar;

    top.skin(ui::Skin{these});
    one->role("moj.pasek");

    same(top.render({{0, 0}, {26, 6}}).at({1, 0}).style.foreground, (Color{false, 3, 3, 3}));
}

// --- What the menu accepts -----------------------------------------------------------------------

// `capacity()` says how many, `accepts()` says what. The menu walks its children as items (it
// counts them, skips the separators, reaches for `enabled()`), so a foreign child would be a hole
// in the middle of the walk. Instead of handing out that hole over and over on every access, we
// refuse once, at attaching.
TRIAL("Menu: accepts items and separators, not a foreign child") {
    const auto one = std::make_shared<ui::Menu>("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    one->children().add(std::make_shared<ui::MenuSeparator>());

    same(one->children().many(), std::size_t{2});

    bool threw = false;

    try {
        one->children().add(std::make_shared<ui::Span>("obce"));
    } catch (const std::logic_error&) {
        threw = true;
    }

    ought(threw);
    same(one->children().many(), std::size_t{2});
}

// The line follows the stem of the item's role, so replacing the stem covers the line as well.
TRIAL("MenuSeparator: the line follows the stem of the item's role") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto menu = window->add("Plik");
    const auto line = std::make_shared<ui::MenuSeparator>();

    same(line->role(), std::string{"talcite.menu"});

    // The line does not stretch the menu, so without an ordinary item next to it it would have
    // zero width and would not draw a single cell.
    menu->children().add(std::make_shared<ui::MenuItem>("Otwórz"));
    menu->children().add(line);

    top.children().add(window);

    ui::Styles these;

    Style look{};

    look.foreground = Color{false, 5, 5, 5};

    these["moje.menu.frame"] = look;

    top.skin(ui::Skin{these});
    line->role("moje.menu");

    menu->open();

    const View made = top.render({{0, 0}, {20, 6}});

    bool found = false;

    for (int y = 0; y < 6 && !found; ++y)
        for (int x = 0; x < 20 && !found; ++x)
            if (made.at({x, y}).style.foreground == Color{false, 5, 5, 5}) found = true;

    ought(found);
}

// A title's letter switches menus while the bar is active. The title is a node with keys, so it
// takes the letter by the same mechanism as a menu item and a key bar item.
TRIAL("Menu: a title's letter switches menus while the bar is active") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto first = window->add("Plik");
    const auto second = window->add("Widok");

    first->keys().add(ui::Key{U'p'});
    second->keys().add(ui::Key{U'w'});

    first->children().add(std::make_shared<ui::MenuItem>("Otwórz"));
    second->children().add(std::make_shared<ui::MenuItem>("Odśwież"));

    window->content(body);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {20, 6}});

    ui::KeyEvent nine{ui::Code::f9};

    // F9 takes the bar rolled up, and that is exactly why the title's letter has somewhere to work
    // now: if the first menu were unrolled, it would swallow the letter for its items.
    ought(top.dispatch(nine));
    same(window->current(), first.get());
    same(unfolded(*window), static_cast<ui::Menu*>(nullptr));

    ui::KeyEvent look{U'w'};

    ought(top.dispatch(look));
    same(unfolded(*window), second.get());

    ui::KeyEvent file{U'P'};

    ought(top.dispatch(file));
    same(unfolded(*window), first.get());
}

// A title without items is a button in the bar, and its letter does what a click on it does: it
// calls the title's event and leaves the bar, instead of unrolling an empty list.
TRIAL("Menu: a title's letter on a menu without items picks it, as a click does") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto first = window->add("Plik");
    const auto help = window->add("Pomoc");

    first->keys().add(ui::Key{U'p'});
    help->keys().add(ui::Key{U'o'});

    first->children().add(std::make_shared<ui::MenuItem>("Otwórz"));

    window->content(body);
    top.children().add(window);

    body->focus();

    int picked = 0;

    const ui::Hook heard_pick = help->on<ui::PickEvent>([&picked](ui::PickEvent&) {
        ++picked;

        return true;
    });

    (void)top.render({{0, 0}, {20, 6}});

    ui::KeyEvent nine{ui::Code::f9};

    ought(top.dispatch(nine));
    same(window->current(), first.get());

    ui::KeyEvent letter{U'o'};

    ought(top.dispatch(letter));
    same(picked, 1);
    same(unfolded(*window), static_cast<ui::Menu*>(nullptr));
    same(top.focused_node(), static_cast<ui::Node*>(body.get()));
}

// Without an active bar a letter must not unroll anything; otherwise typing over a panel would open
// menus.
TRIAL("Menu: a title's letter with the bar closed does nothing") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto first = window->add("Plik");

    first->keys().add(ui::Key{U'p'});
    first->children().add(std::make_shared<ui::MenuItem>("Otwórz"));

    window->content(body);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {20, 6}});

    ui::KeyEvent file{U'p'};

    ought(!top.dispatch(file));
    ought(unfolded(*window) == nullptr);
}

// An item of an open menu takes the letter before the titles: it lies closer to the focus.
TRIAL("Menu: an item's letter wins over a title's letter") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto first = window->add("Plik");
    const auto second = window->add("Widok");

    first->keys().add(ui::Key{U'p'});
    second->keys().add(ui::Key{U'w'});

    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto item = std::make_shared<ui::MenuItem>("Wytnij");

    item->keys().add(ui::Key{U'w'});

    int said = 0;

    const ui::Hook heard_pick = item->on<ui::PickEvent>([&said](ui::PickEvent&) {
        ++said;

        return true;
    });

    first->children().add(item);
    second->children().add(std::make_shared<ui::MenuItem>("Odśwież"));

    window->content(body);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {20, 6}});

    ui::KeyEvent nine{ui::Code::f9};

    ought(top.dispatch(nine));

    // Unrolling is the condition of the question here: the item's priority over the title is
    // decided only with the menu unrolled. With it rolled up the same letter belongs to the title;
    // Midnight Commander divides it the same way, with one variable, is_dropped.
    ui::KeyEvent drop{ui::Code::down};

    ought(top.dispatch(drop));

    ui::KeyEvent cut{U'w'};

    ought(top.dispatch(cut));
    same(said, 1);
}

// The title underlines its letter, just like a menu item and a button.
TRIAL("Menu: a title's letter is underlined in the bar") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto first = window->add("Plik");

    first->keys().add(ui::Key{U'l'});
    first->children().add(std::make_shared<ui::MenuItem>("Otwórz"));

    top.children().add(window);

    (void)top.render({{0, 0}, {20, 3}});

    const View made = top.render({{0, 0}, {20, 3}});

    ought(made.at({2, 0}).style.under);
    ought(!made.at({1, 0}).style.under);
}

// Without a given key the bar does not open from the keyboard. The library takes no key
// combination that nobody told it about; the caller gives the key, and can change it or leave it
// out.
TRIAL("Menu: without a given key the bar does not open from the keyboard") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto one = window->add("Plik");

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    window->content(body);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {20, 6}});

    ui::KeyEvent nine{ui::Code::f9};

    ought(!top.dispatch(nine));
    ought(!one->opened());
}

// The key given by the caller toggles the bar: it opens a closed one and closes an open one. And it
// does not have to be F9: the bar is a node with keys, like a menu item and a key bar item.
TRIAL("Menu: the given key toggles the bar") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();
    const auto body = std::make_shared<Box>(ui::Size{10, 4}, U'x');
    const auto one = window->add("Plik");

    window->keys().add(ui::Key{ui::Code::f2});

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    window->content(body);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {20, 6}});

    ui::KeyEvent two{ui::Code::f2};

    ought(top.dispatch(two));
    same(window->current(), one.get());

    ought(top.dispatch(two));
    ought(window->current() == nullptr);

    ui::KeyEvent nine{ui::Code::f9};

    ought(!top.dispatch(nine));
    ought(!one->opened());
}

TRIAL("Menu: double-width characters widen the box by the width of the sides") {
    ui::Root top;

    top.skin(ui::Skin{{},
                      {{"talcite.line.thin.upper.left", U'口'},
                       {"talcite.line.thin.upper.right", U'口'},
                       {"talcite.line.thin.lower.left", U'口'},
                       {"talcite.line.thin.lower.right", U'口'},
                       {"talcite.line.thin.across", U'一'},
                       {"talcite.line.thin.upright", U'丨'},
                       {"talcite.line.thin.left.joint", U'卜'},
                       {"talcite.line.thin.right.joint", U'卜'}}});

    const auto window = std::make_shared<ui::Menubar>();

    window->keys().add(ui::Key{ui::Code::f9});
    const auto one = window->add("Plik", ui::MenuShape{ui::Border::thin, false});

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj", "F5"));
    one->children().add(std::make_shared<ui::MenuSeparator>());
    one->children().add(std::make_shared<ui::MenuItem>("Wyjście", "F10"));
    top.children().add(window);

    one->open();

    const View made = top.render({{0, 0}, {30, 8}});

    // The inside has fourteen cells, as in the trial with an ordinary frame; the sides add two
    // each.
    same(one->wants({80, 25}).wide, 18);
    same(read(made, 1), std::string{"口一一一一一一一口"});
    same(read(made, 2), std::string{"丨 Kopiuj    F5 丨"});
    same(read(made, 3), std::string{"卜一一一一一一一卜"});
    same(read(made, 4), std::string{"丨 Wyjście  F10 丨"});
    same(read(made, 5), std::string{"口一一一一一一一口"});
}

namespace {

// A bar with one menu, "Plik", drawn without a shadow, whose first item is the submenu "Biblioteka"
// with the hint "Filmy" and the list "Ustawienia", "Historia", and whose second item is "Wyjście".
// The trial gets the parts it asks about.
struct Cascade {
    std::shared_ptr<ui::Menubar> window = std::make_shared<ui::Menubar>();
    std::shared_ptr<ui::Menu> menu;
    std::shared_ptr<ui::Submenu> deeper = std::make_shared<ui::Submenu>("Biblioteka", "Filmy");
    std::shared_ptr<ui::MenuItem> settings = std::make_shared<ui::MenuItem>("Ustawienia");
    std::shared_ptr<ui::MenuItem> history = std::make_shared<ui::MenuItem>("Historia");
    std::shared_ptr<ui::MenuItem> quit = std::make_shared<ui::MenuItem>("Wyjście");

    // Puts the bar together in `top`, with the box `body` as its content.
    Cascade(ui::Root& top, const std::shared_ptr<ui::Node>& body) {
        window->keys().add(ui::Key{ui::Code::f9});
        menu = window->add("Plik", ui::MenuShape{ui::Border::thin, false});

        deeper->menu().children().add(settings);
        deeper->menu().children().add(history);

        menu->children().add(deeper);
        menu->children().add(quit);

        window->content(body);
        top.children().add(window);
    }
};

// Sends one key to the root.
bool pressed(ui::Root& top, ui::KeyEvent what) { return top.dispatch(what); }

}

TRIAL("Submenu: the right arrow opens the list beside the item, with its first item in the item's row") {
    ui::Root top;

    const auto body = std::make_shared<Box>(ui::Size{40, 8}, U' ');
    Cascade made{top, body};

    top.unicode(true);
    body->focus();
    made.menu->open();

    (void)top.render({{0, 0}, {40, 8}});

    ought(made.deeper->focused());
    ought(pressed(top, ui::KeyEvent{ui::Code::right}));

    ought(made.deeper->menu().opened());
    ought(made.menu->opened());
    ought(made.settings->focused());

    const View seen = top.render({{0, 0}, {40, 8}});

    same(read(seen, 1), std::string{"┌─────────────────────┌────────────┐"});
    same(read(seen, 2), std::string{"│ Biblioteka  Filmy ▸ │ Ustawienia │"});
    same(read(seen, 3), std::string{"│ Wyjście             │ Historia   │"});
    same(read(seen, 4), std::string{"└─────────────────────└────────────┘"});
}

// A list too tall for the rows under its item moves up, as far as it must, and stays whole on the
// screen; the bar places it, as it places every menu it opens.
TRIAL("Submenu: a list too tall for the rows below moves up") {
    ui::Root top;

    const auto body = std::make_shared<Box>(ui::Size{40, 6}, U' ');
    Cascade made{top, body};

    made.deeper->menu().children().add(std::make_shared<ui::MenuItem>("Kosz"));
    made.deeper->menu().children().add(std::make_shared<ui::MenuItem>("Źródła"));

    body->focus();
    made.menu->open();

    (void)top.render({{0, 0}, {40, 6}});

    ought(pressed(top, ui::KeyEvent{ui::Code::right}));

    (void)top.render({{0, 0}, {40, 6}});

    same(made.deeper->menu().area().at.y, 0);
    same(made.deeper->menu().area().size.high, 6);
}

TRIAL("Submenu: Esc and the left arrow close the list alone and give the focus back to the item") {
    ui::Root top;

    const auto body = std::make_shared<Box>(ui::Size{40, 8}, U' ');
    Cascade made{top, body};

    body->focus();
    made.menu->open();

    (void)top.render({{0, 0}, {40, 8}});

    for (const ui::Code key : {ui::Code::escape, ui::Code::left}) {
        ought(pressed(top, ui::KeyEvent{ui::Code::right}));
        ought(made.deeper->menu().opened());

        ought(pressed(top, ui::KeyEvent{key}));

        ought(!made.deeper->menu().opened());
        ought(made.menu->opened());
        ought(made.deeper->focused());
        same(made.menu->which(), std::optional<std::size_t>{0});
    }

    // The second Esc is the bar's again: it leaves, and the focus goes back to the content.
    ought(pressed(top, ui::KeyEvent{ui::Code::escape}));

    ought(!made.menu->opened());
    ought(body->focused());
}

TRIAL("Submenu: Enter on an item of the list picks it and leaves the bar") {
    proving::Hooks heard;

    ui::Root top;

    const auto body = std::make_shared<Box>(ui::Size{40, 8}, U' ');
    Cascade made{top, body};

    int done = 0;

    (void)proving::doing(heard, made.history, [&done] { ++done; });

    body->focus();
    made.menu->open();

    (void)top.render({{0, 0}, {40, 8}});

    ought(pressed(top, ui::KeyEvent{ui::Code::enter}));
    ought(made.deeper->menu().opened());
    same(done, 0);

    ought(pressed(top, ui::KeyEvent{ui::Code::down}));
    ought(made.history->focused());
    ought(pressed(top, ui::KeyEvent{ui::Code::enter}));

    same(done, 1);
    ought(!made.deeper->menu().opened());
    ought(!made.menu->opened());
    ought(body->focused());
    same(made.window->children().many(), std::size_t{1});
}

TRIAL("Submenu: picking it fires no PickEvent of its own") {
    proving::Hooks heard;

    ui::Root top;

    const auto body = std::make_shared<Box>(ui::Size{40, 8}, U' ');
    Cascade made{top, body};

    int done = 0;

    (void)proving::doing(heard, made.deeper, [&done] { ++done; });

    made.menu->open();
    made.deeper->pick();

    same(done, 0);
    ought(made.deeper->menu().opened());
}

TRIAL("Submenu: the right arrow in the list goes on to the next menu of the bar") {
    ui::Root top;

    const auto body = std::make_shared<Box>(ui::Size{40, 8}, U' ');
    Cascade made{top, body};

    const auto second = made.window->add("Widok");

    second->children().add(std::make_shared<ui::MenuItem>("Odśwież"));

    body->focus();
    made.menu->open();

    (void)top.render({{0, 0}, {40, 8}});

    ought(pressed(top, ui::KeyEvent{ui::Code::right}));
    ought(made.deeper->menu().opened());

    ought(pressed(top, ui::KeyEvent{ui::Code::right}));

    ought(!made.deeper->menu().opened());
    ought(!made.menu->opened());
    ought(second->opened());
    same(made.window->children().many(), std::size_t{2});

    // Esc then leaves the bar for the content, which had the focus before the bar was entered.
    ought(pressed(top, ui::KeyEvent{ui::Code::escape}));
    ought(body->focused());
}

TRIAL("Submenu: a letter and a click open the list") {
    ui::Root top;

    const auto body = std::make_shared<Box>(ui::Size{40, 8}, U' ');
    Cascade made{top, body};

    made.deeper->keys().add(U'b');

    body->focus();
    made.menu->open();
    made.menu->which(1);

    (void)top.render({{0, 0}, {40, 8}});

    ought(pressed(top, ui::KeyEvent{U'b'}));
    ought(made.deeper->menu().opened());
    ought(made.menu->opened());

    ought(pressed(top, ui::KeyEvent{ui::Code::escape}));
    ought(!made.deeper->menu().opened());

    made.menu->which(1);

    (void)top.render({{0, 0}, {40, 8}});

    ui::ClickEvent what{{3, 2}, ui::Mouse::left};

    ought(top.dispatch(what));
    ought(made.deeper->menu().opened());
    ought(made.settings->focused());
}

TRIAL("Submenu: another item of the menu closes the list") {
    ui::Root top;

    const auto body = std::make_shared<Box>(ui::Size{40, 8}, U' ');
    Cascade made{top, body};

    body->focus();
    made.menu->open();
    made.deeper->pick();

    (void)top.render({{0, 0}, {40, 8}});

    ought(made.deeper->menu().opened());

    ui::MouseEvent over{{3, 3}, ui::Press::move, ui::Mouse::none};

    (void)top.dispatch(over);

    ought(made.quit->focused());
    ought(!made.deeper->menu().opened());
    ought(made.menu->opened());
}

TRIAL("Submenu: while its list is open, the item stays highlighted") {
    ui::Root top;

    const auto body = std::make_shared<Box>(ui::Size{40, 8}, U' ');
    Cascade made{top, body};

    made.menu->open();
    made.deeper->pick();

    const View seen = top.render({{0, 0}, {40, 8}});

    ought(!made.deeper->focused());
    ought(seen.at({3, 2}).style == top.skin().style("talcite.menu.focus"));
    ought(seen.at({3, 3}).style == top.skin().style("talcite.menu"));
}

TRIAL("Submenu: closing the menu or leaving the bar closes the list") {
    ui::Root top;

    const auto body = std::make_shared<Box>(ui::Size{40, 8}, U' ');
    Cascade made{top, body};

    made.menu->open();
    made.deeper->pick();

    made.menu->close();

    ought(!made.deeper->menu().opened());
    same(made.window->children().many(), std::size_t{1});

    made.menu->open();
    made.deeper->pick();

    body->focus();

    ought(!made.deeper->menu().opened());
    ought(!made.menu->opened());
    same(made.window->children().many(), std::size_t{1});
}

TRIAL("Submenu: at the right edge the list opens on the left, its right border over the menu's left") {
    ui::Root top;

    const auto window = std::make_shared<ui::Menubar>();
    const auto first = window->add("Plik");
    const auto middle = window->add("Edycja");
    const auto second = window->add("Widok", ui::MenuShape{ui::Border::thin, false});
    const auto deeper = std::make_shared<ui::Submenu>("Biblioteka");

    first->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    middle->children().add(std::make_shared<ui::MenuItem>("Wytnij"));
    deeper->menu().children().add(std::make_shared<ui::MenuItem>("Ustawienia"));
    second->children().add(deeper);
    top.children().add(window);

    second->open();

    (void)top.render({{0, 0}, {40, 8}});

    deeper->pick();

    (void)top.render({{0, 0}, {40, 8}});

    const ui::Area menu = second->area();
    const ui::Area list = deeper->menu().area();

    ought(menu.at.x + menu.size.wide + list.size.wide > 40);
    same(list.at.x + list.size.wide - 1, menu.at.x);
    same(list.at.y, menu.at.y);
}

TRIAL("Submenu: a disabled one does not open, and the arrows pass over it") {
    ui::Root top;

    const auto body = std::make_shared<Box>(ui::Size{40, 8}, U' ');
    Cascade made{top, body};

    made.deeper->enabled(false);

    made.menu->open();

    ought(made.quit->focused());

    made.deeper->pick();

    ought(!made.deeper->menu().opened());
}

TRIAL("Submenu: the mark has a plain form, and the hint stands before it") {
    ui::Root top;

    const auto body = std::make_shared<Box>(ui::Size{40, 8}, U' ');
    Cascade made{top, body};

    top.unicode(false);
    made.menu->open();

    const View seen = top.render({{0, 0}, {40, 8}});

    same(read(seen, 2), std::string{"│ Biblioteka  Filmy > │"});
}

TRIAL("Submenu: a list whose item went away is closed by the bar") {
    ui::Root top;

    const auto body = std::make_shared<Box>(ui::Size{40, 8}, U' ');
    Cascade made{top, body};

    body->focus();
    made.menu->open();

    (void)top.render({{0, 0}, {40, 8}});

    ought(pressed(top, ui::KeyEvent{ui::Code::right}));

    std::weak_ptr<ui::Submenu> gone = made.deeper;

    (void)made.menu->children().remove(made.deeper.get());
    made.deeper.reset();

    ought(gone.expired());
    same(made.window->children().many(), std::size_t{3});

    ought(pressed(top, ui::KeyEvent{ui::Code::escape}));

    same(made.window->children().many(), std::size_t{1});
    ought(body->focused());
}

TRIAL("Submenu: a list inside a list closes from the deepest, and the menu takes all of them") {
    ui::Root top;

    const auto body = std::make_shared<Box>(ui::Size{60, 10}, U' ');
    Cascade made{top, body};

    const auto deepest = std::make_shared<ui::Submenu>("Więcej");
    const auto last = std::make_shared<ui::MenuItem>("Ostatnia");

    deepest->menu().children().add(last);
    made.deeper->menu().children().add(deepest);

    body->focus();
    made.menu->open();

    (void)top.render({{0, 0}, {60, 10}});

    ought(pressed(top, ui::KeyEvent{ui::Code::right}));
    ought(pressed(top, ui::KeyEvent{ui::Code::up}));
    ought(deepest->focused());

    (void)top.render({{0, 0}, {60, 10}});

    ought(pressed(top, ui::KeyEvent{ui::Code::right}));
    ought(last->focused());
    same(made.window->children().many(), std::size_t{4});

    ought(pressed(top, ui::KeyEvent{ui::Code::escape}));
    ought(deepest->focused());
    ought(made.deeper->menu().opened());

    deepest->pick();
    made.menu->close();

    same(made.window->children().many(), std::size_t{1});
    ought(!deepest->menu().opened());
}

TRIAL("Submenu: the bar does not take a menu that is not its own") {
    ui::Menubar window;

    bool threw = false;

    try {
        window.children().add(std::make_shared<ui::Menu>("Obce"));
    } catch (const std::logic_error&) {
        threw = true;
    }

    ought(threw);
    same(static_cast<int>(window.children().many()), 1);
}
