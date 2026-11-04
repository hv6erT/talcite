#include "proving.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <random>
#include <string>
#include <vector>

#include "talcite/encoding.hpp"
#include "talcite/list.hpp"
#include "talcite/listitem.hpp"
#include "talcite/root.hpp"
#include "talcite/span.hpp"

using namespace talcite;

#include "doing.hpp"

namespace {

// A root that counts keys, so that one can check what an item passes up and what it keeps.
class Sheet : public ui::Root {
public:
    int keys = 0;

protected:
    // Counts every key that reaches the root and takes it.
    bool handle(ui::KeyEvent&) override {
        ++keys;

        return true;
    }
};

// A container that lays its children out in rows, each with the full width. That is what the list
// does with its items, and here this piece alone is enough, without the list.
class Nest : public ui::Node {
public:
    // The nest can hold the focus itself.
    [[nodiscard]] bool focusable() const override { return true; }

    // Any number of children.
    [[nodiscard]] int capacity() const override { return ui::plenty; }

    // All the room offered.
    [[nodiscard]] ui::Size wants(ui::Size room) const override { return room; }

protected:
    // Lays each child out on its own row, one below the other.
    void layout(ui::Size given) override {
        int y = 0;

        for (const std::shared_ptr<ui::Node>& one : children()) one->arrange({{0, y++}, {given.wide, 1}});
    }

    // Paints the rows over a background of spaces, clipped to the visible part.
    void paint(Canvas& onto) const override {
        onto.fill({{0, 0}, onto.size()}, Style{});

        for (const std::shared_ptr<ui::Node>& one : children()) one->render(onto);
    }
};

// A willing leaf for checking where the focus goes out to and where it comes back from.
class Perch : public ui::Node {
public:
    // The perch can take the focus.
    [[nodiscard]] bool focusable() const override { return true; }

    // One cell.
    [[nodiscard]] ui::Size wants(ui::Size) const override { return {1, 1}; }
};

// A list of `much` items named "wiersz 0", "wiersz 1"…, asking for `wide` by `high`.
std::shared_ptr<ui::List> listed(int much, int wide = ui::plenty, int high = ui::plenty) {
    const auto made = std::make_shared<ui::List>(wide, high);

    for (int which = 0; which < much; ++which)
        made->children().add(std::make_shared<ui::ListItem>("wiersz " + std::to_string(which)));

    return made;
}

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

TRIAL("ListItem: wants as much as its text, and always one row") {
    const auto one = std::make_shared<ui::ListItem>("katalog");

    same(one->wants({20, 3}), (ui::Size{7, 1}));

    // Tight: it clips to what it got.
    same(one->wants({4, 3}), (ui::Size{4, 1}));

    // A list row that wraps is not a list row.
    same(one->wants({20, 0}), (ui::Size{7, 0}));
}

TRIAL("ListItem: the text starts at the first cell, without a gap") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto one = std::make_shared<ui::ListItem>("plik");

    nest->children().add(one);
    top.children().add(nest);

    const View made = top.render({{0, 0}, {8, 1}});

    same(read(made, 0), std::string{"plik"});
}

TRIAL("ListItem: a text longer than the row is clipped with an ellipsis on the right") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto one = std::make_shared<ui::ListItem>("dokumenty");

    nest->children().add(one);
    top.children().add(nest);

    const View made = top.render({{0, 0}, {5, 1}});

    same(read(made, 0), std::string{"doku…"});
}

// The highlight goes across the whole width of the row, not only under the text; otherwise the
// selection bar would break off halfway and look like a drawing bug.
TRIAL("ListItem: under focus the whole row is reversed") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto one = std::make_shared<ui::ListItem>("plik");

    nest->children().add(one);
    top.children().add(nest);

    View made = top.render({{0, 0}, {8, 1}});

    ought(!made.at({0, 0}).style.reverse);
    ought(!made.at({7, 0}).style.reverse);

    one->focus();

    made = top.render({{0, 0}, {8, 1}});

    ought(made.at({0, 0}).style.reverse);
    ought(made.at({7, 0}).style.reverse);
}

TRIAL("ListItem: a disabled one dims and does not take the focus") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto one = std::make_shared<ui::ListItem>("plik");

    nest->children().add(one);
    top.children().add(nest);

    ought(one->focusable());

    one->enabled(false);

    ought(!one->focusable());

    // Calling it on a node that does not take the focus does nothing, silently, as in the root.
    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(nullptr));

    const View made = top.render({{0, 0}, {8, 1}});

    ought(made.at({0, 0}).style.faint);
}

// The same rule as in the button and in the field: the focus goes to the nearest willing ancestor,
// not into the void, because in the void the dialog stops swallowing keys.
TRIAL("ListItem: disabling with the focus hands it to the ancestor") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto one = std::make_shared<ui::ListItem>("plik");

    nest->children().add(one);
    top.children().add(nest);

    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(one.get()));

    one->enabled(false);

    same(top.focused_node(), static_cast<ui::Node*>(nest.get()));
}

TRIAL("ListItem: Enter calls the task") {
    proving::Hooks heard;

    Sheet top;

    const auto nest = std::make_shared<Nest>();

    int done = 0;

    const auto one = proving::doing(heard, std::make_shared<ui::ListItem>("plik"), [&done] { ++done; });

    nest->children().add(one);
    top.children().add(nest);

    one->focus();

    ui::KeyEvent what{ui::Code::enter};

    ought(top.dispatch(what));

    same(done, 1);
    same(top.keys, 0);
}

// Enter with Control does not belong to the item, just as in the field and in the key bar.
TRIAL("ListItem: Enter with Control goes up") {
    proving::Hooks heard;

    Sheet top;

    const auto nest = std::make_shared<Nest>();

    int done = 0;

    const auto one = proving::doing(heard, std::make_shared<ui::ListItem>("plik"), [&done] { ++done; });

    nest->children().add(one);
    top.children().add(nest);

    one->focus();

    ui::KeyEvent what{ui::Key{ui::Code::enter, ui::Mods{true, false, false}}};

    ought(top.dispatch(what));

    same(done, 0);
    same(top.keys, 1);
}

// The arrows belong to the list, not to the item, so the item does not stop them.
TRIAL("ListItem: an arrow goes up") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto one = std::make_shared<ui::ListItem>("plik");

    nest->children().add(one);
    top.children().add(nest);

    one->focus();

    ui::KeyEvent what{ui::Code::down};

    ought(top.dispatch(what));

    same(top.keys, 1);
}

// This is the whole reason the item takes the focus. A key starts at it, so an attached handler
// sees it before anyone above, and before the walk over the rows once the list is there.
//
// "Before" here means earlier, not "instead": true from the handler ends the path of handlers, but
// the ancestor's `handle` runs its own. The root's counter shows it: at the moment the handler
// speaks it is still zero, and after everything it is one.
TRIAL("ListItem: an attached handler sees the key before the ancestor") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto one = std::make_shared<ui::ListItem>("plik");

    nest->children().add(one);
    top.children().add(nest);

    int seen = 0;
    int meanwhile = -1;

    const ui::Hook heard = one->on<ui::KeyEvent>([&seen, &meanwhile, &top](ui::KeyEvent&) {
        ++seen;
        meanwhile = top.keys;

        return true;
    });

    one->focus();

    ui::KeyEvent what{ui::Code::down};

    ought(top.dispatch(what));

    same(seen, 1);
    same(meanwhile, 0);
    same(top.keys, 1);
}

TRIAL("ListItem: a click takes the focus") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto first = std::make_shared<ui::ListItem>("jeden");
    const auto other = std::make_shared<ui::ListItem>("dwa");

    nest->children().add(first);
    nest->children().add(other);
    top.children().add(nest);

    (void)top.render({{0, 0}, {8, 2}});

    ui::ClickEvent what{{2, 1}, ui::Mouse::left};

    ought(top.dispatch(what));

    same(top.focused_node(), static_cast<ui::Node*>(other.get()));
}

TRIAL("ListItem: a double click takes the focus and calls the task") {
    proving::Hooks heard;

    Sheet top;

    const auto nest = std::make_shared<Nest>();

    int done = 0;

    const auto first = std::make_shared<ui::ListItem>("jeden");
    const auto other = proving::doing(heard, std::make_shared<ui::ListItem>("dwa"), [&done] { ++done; });

    nest->children().add(first);
    nest->children().add(other);
    top.children().add(nest);

    first->focus();

    (void)top.render({{0, 0}, {8, 2}});

    ui::DoubleClickEvent what{{2, 1}, ui::Mouse::left};

    ought(top.dispatch(what));

    same(top.focused_node(), static_cast<ui::Node*>(other.get()));
    same(done, 1);
}

// An unavailable item swallows the click, as every disabled control does: the click was aimed at
// it, so nothing underneath acts on it, and the item itself neither takes the focus nor picks.
TRIAL("ListItem: a disabled one swallows the click and does not take the focus") {
    proving::Hooks heard;

    Sheet top;

    const auto nest = std::make_shared<Nest>();

    int done = 0;

    const auto one = proving::doing(heard, std::make_shared<ui::ListItem>("plik"), [&done] { ++done; });

    one->enabled(false);

    nest->children().add(one);
    top.children().add(nest);

    (void)top.render({{0, 0}, {8, 1}});

    ui::ClickEvent what{{2, 0}, ui::Mouse::left};

    ought(top.dispatch(what));

    same(top.focused_node(), static_cast<ui::Node*>(nullptr));

    ui::DoubleClickEvent twice{{2, 0}, ui::Mouse::left};

    ought(top.dispatch(twice));

    same(done, 0);
}

TRIAL("ListItem: pick on a disabled one does nothing") {
    proving::Hooks heard;

    int done = 0;

    const auto one = proving::doing(heard, std::make_shared<ui::ListItem>("plik"), [&done] { ++done; });

    one->pick();

    same(done, 1);

    one->enabled(false);
    one->pick();

    same(done, 1);
}

TRIAL("ListItem: the text and the task can be replaced along the way") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto one = std::make_shared<ui::ListItem>("stary");

    nest->children().add(one);
    top.children().add(nest);

    one->text("nowy");

    same(one->text(), std::string{"nowy"});

    const View made = top.render({{0, 0}, {8, 1}});

    same(read(made, 0), std::string{"nowy"});

    int done = 0;

    const ui::Hook heard_pick = one->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });
    one->pick();

    same(done, 1);
}

TRIAL("ListItem: does not accept children") {
    const auto one = std::make_shared<ui::ListItem>("plik");

    same(one->capacity(), 0);

    threw(one->children().add(std::make_shared<ui::ListItem>("inne")));
}

TRIAL("List: the focus goes to the item, not to the list") {
    Sheet top;

    const auto one = listed(3);

    top.children().add(one);

    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(one->children().front().get()));
    same(one->which(), std::optional<std::size_t>{0});
}

TRIAL("List: an empty one takes the focus itself") {
    Sheet top;

    const auto one = std::make_shared<ui::List>();

    top.children().add(one);

    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(one.get()));
    ought(one->current() == nullptr);
}

// A list keeps its keys at its ends: in a dialog, an arrow pressed once too often must not carry
// the focus off to a button, where the Enter or Space that follows would press it instead of the
// row.
TRIAL("List: the arrows walk the rows, do not wrap, and keep the key at the end") {
    Sheet top;

    const auto one = listed(3);

    top.children().add(one);

    one->focus();

    ui::KeyEvent down{ui::Code::down};

    ought(top.dispatch(down));
    same(one->which(), std::optional<std::size_t>{1});

    ui::KeyEvent again{ui::Code::down};

    ought(top.dispatch(again));
    same(one->which(), std::optional<std::size_t>{2});

    same(top.keys, 0);

    // On the last row down moves nothing and does not go back to the first, and the key stays.
    ui::KeyEvent past{ui::Code::down};

    ought(top.dispatch(past));
    same(one->which(), std::optional<std::size_t>{2});
    same(top.keys, 0);

    ui::KeyEvent beyond{ui::Code::pagedown};

    ought(top.dispatch(beyond));
    same(one->which(), std::optional<std::size_t>{2});
    same(top.keys, 0);

    ui::KeyEvent up{ui::Code::up};

    ought(top.dispatch(up));
    same(one->which(), std::optional<std::size_t>{1});

    ui::KeyEvent above{ui::Code::up};

    ought(top.dispatch(above));
    same(one->which(), std::optional<std::size_t>{0});
    same(top.keys, 0);

    ui::KeyEvent before{ui::Code::up};

    ought(top.dispatch(before));
    same(one->which(), std::optional<std::size_t>{0});
    same(top.keys, 0);

    ui::KeyEvent sooner{ui::Code::pageup};

    ought(top.dispatch(sooner));
    same(one->which(), std::optional<std::size_t>{0});
    same(top.keys, 0);
}

TRIAL("List: Home and End go to the first and the last") {
    Sheet top;

    const auto one = listed(5);

    top.children().add(one);

    one->focus();

    ui::KeyEvent last{ui::Code::end};

    ought(top.dispatch(last));
    same(one->which(), std::optional<std::size_t>{4});

    ui::KeyEvent first{ui::Code::home};

    ought(top.dispatch(first));
    same(one->which(), std::optional<std::size_t>{0});
    same(top.keys, 0);
}

// Home on the first row and End on the last move nothing, and the list keeps them, as it does the
// arrows at its ends.
TRIAL("List: Home on the first row and End on the last keep the key") {
    Sheet top;

    const auto one = listed(3);

    top.children().add(one);

    one->focus();

    ui::KeyEvent first{ui::Code::home};

    ought(top.dispatch(first));
    same(one->which(), std::optional<std::size_t>{0});
    same(top.keys, 0);

    ui::KeyEvent last{ui::Code::end};

    ought(top.dispatch(last));
    same(one->which(), std::optional<std::size_t>{2});

    ui::KeyEvent again{ui::Code::end};

    ought(top.dispatch(again));
    same(one->which(), std::optional<std::size_t>{2});
    same(top.keys, 0);
}

// A page is computed from the real height that the list got in the previous frame.
TRIAL("List: a page is as many rows as are visible") {
    Sheet top;

    const auto one = listed(20);

    top.children().add(one);

    one->focus();

    (void)top.render({{0, 0}, {12, 5}});

    ui::KeyEvent down{ui::Code::pagedown};

    ought(top.dispatch(down));
    same(one->which(), std::optional<std::size_t>{5});

    ui::KeyEvent up{ui::Code::pageup};

    ought(top.dispatch(up));
    same(one->which(), std::optional<std::size_t>{0});

    // A page near the end stops at the last row instead of going past it.
    ui::KeyEvent last{ui::Code::end};

    (void)top.dispatch(last);

    ui::KeyEvent over{ui::Code::pagedown};

    ought(top.dispatch(over));
    same(one->which(), std::optional<std::size_t>{19});
}

TRIAL("List: the walk skips unavailable items") {
    Sheet top;

    const auto one = listed(4);

    top.children().add(one);

    dynamic_cast<ui::ListItem*>(one->children()[1].get())->enabled(false);
    dynamic_cast<ui::ListItem*>(one->children()[2].get())->enabled(false);

    one->focus();

    same(one->which(), std::optional<std::size_t>{0});

    ui::KeyEvent down{ui::Code::down};

    ought(top.dispatch(down));
    same(one->which(), std::optional<std::size_t>{3});

    ui::KeyEvent up{ui::Code::up};

    ought(top.dispatch(up));
    same(one->which(), std::optional<std::size_t>{0});
}

TRIAL("List: only unavailable items, the focus stays on the list") {
    Sheet top;

    const auto one = listed(2);

    top.children().add(one);

    dynamic_cast<ui::ListItem*>(one->children()[0].get())->enabled(false);
    dynamic_cast<ui::ListItem*>(one->children()[1].get())->enabled(false);

    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(one.get()));
}

// Scrolling is computed in painting, because only there is the height known; the same rule as in
// the field.
TRIAL("List: the view catches up with the current row") {
    Sheet top;

    const auto one = listed(10);

    top.children().add(one);

    one->focus();

    (void)top.render({{0, 0}, {12, 3}});

    same(one->first(), std::size_t{0});

    ui::KeyEvent last{ui::Code::end};

    (void)top.dispatch(last);
    (void)top.render({{0, 0}, {12, 3}});

    // Ten rows, three visible, we stand on the ninth, so the view starts at the seventh.
    same(one->which(), std::optional<std::size_t>{9});
    same(one->first(), std::size_t{7});

    ui::KeyEvent first{ui::Code::home};

    (void)top.dispatch(first);
    (void)top.render({{0, 0}, {12, 3}});

    same(one->first(), std::size_t{0});
}

// The wheel moves only the view: the current row stays, and `moved` stays silent.
TRIAL("List: the wheel scrolls the view and does not move the row") {
    Sheet top;

    const auto one = listed(10);

    int moves = 0;

    const ui::Hook heard_scroll = one->on<ui::ScrollEvent>([&moves](ui::ScrollEvent&) {
        ++moves;

        return false;
    });

    top.children().add(one);

    one->focus();

    same(moves, 1);

    (void)top.render({{0, 0}, {12, 3}});

    ui::WheelEvent what{{2, 1}, -1};

    ought(top.dispatch(what));

    (void)top.render({{0, 0}, {12, 3}});

    // The view moved, so the event goes out, but the current row stayed where it was.
    same(one->first(), std::size_t{3});
    same(one->which(), std::optional<std::size_t>{0});
    same(moves, 2);
}

// On the first row with the view turned away by the wheel, Up brings the row back into view.
TRIAL("List: a key on the current row out of view brings the view back") {
    Sheet top;

    const auto one = listed(10);

    top.children().add(one);

    one->focus();

    (void)top.render({{0, 0}, {12, 3}});

    ui::WheelEvent what{{2, 1}, -1};

    ought(top.dispatch(what));

    (void)top.render({{0, 0}, {12, 3}});

    same(one->first(), std::size_t{3});

    ui::KeyEvent up{ui::Code::up};

    ought(top.dispatch(up));

    (void)top.render({{0, 0}, {12, 3}});

    same(one->first(), std::size_t{0});
    same(one->which(), std::optional<std::size_t>{0});
    same(top.keys, 0);
}

// The list's own keys, the wheel and a click on an item move it by hand; a call from the program
// (`first`) and a focus put on an item from elsewhere do not. The event comes once per move: the
// focus that follows a key finds the row current already.
TRIAL("List: a move says whether it came by hand") {
    Sheet top;

    const auto one = listed(10);

    std::vector<bool> heard;

    const ui::Hook heard_scroll = one->on<ui::ScrollEvent>([&heard](ui::ScrollEvent& what) {
        heard.push_back(what.by_hand());

        return false;
    });

    top.children().add(one);

    one->focus();

    (void)top.render({{0, 0}, {12, 3}});

    heard.clear();

    ui::KeyEvent down{ui::Code::down};

    ought(top.dispatch(down));
    same(one->which(), std::optional<std::size_t>{1});

    ui::WheelEvent wheel{{2, 1}, -1};

    ought(top.dispatch(wheel));

    (void)top.render({{0, 0}, {12, 3}});

    ui::ClickEvent click{{2, 1}, ui::Mouse::left, {}};

    ought(top.dispatch(click));

    same(heard, (std::vector<bool>{true, true, true}));

    heard.clear();

    one->first(0);
    one->children()[6]->focus();

    same(heard, (std::vector<bool>{false, false}));
    same(one->which(), std::optional<std::size_t>{6});
}

TRIAL("List: the wheel at the end of scrolling passes the event up") {
    Sheet top;

    const auto one = listed(10);

    top.children().add(one);

    (void)top.render({{0, 0}, {12, 3}});

    // Up from zero there is nowhere to go; the event goes on, so that a scroller above the list
    // gets it.
    ui::WheelEvent up{{2, 1}, 1};

    ought(!top.dispatch(up));

    ui::WheelEvent down{{2, 1}, -1};

    ought(top.dispatch(down));
}

// The list does not scroll sideways, so the tilted wheel passes on instead of scrolling down.
TRIAL("List: the tilted wheel passes the event up") {
    Sheet top;

    const auto one = listed(10);

    top.children().add(one);

    (void)top.render({{0, 0}, {12, 3}});

    ui::WheelEvent tilted{{2, 1}, -1, {}, ui::Axis::horizontal};

    ought(!top.dispatch(tilted));
    same(one->first(), std::size_t{0});
}

TRIAL("List: the event speaks once per change of row") {
    Sheet top;

    const auto one = listed(4);

    int moves = 0;
    std::optional<std::size_t> last;

    // The event carries no row number; the listener reads it from `which()`. One way of notifying
    // instead of two: `on` and the hook it gives, as everywhere else.
    const std::weak_ptr<ui::List> weakly = one;

    const ui::Hook heard_scroll = one->on<ui::ScrollEvent>([&moves, &last, weakly](ui::ScrollEvent&) {
        ++moves;

        if (const auto got = weakly.lock()) last = got->which();

        return false;
    });

    top.children().add(one);

    one->focus();

    same(moves, 1);
    same(last, std::optional<std::size_t>{0});

    ui::KeyEvent down{ui::Code::down};

    ought(top.dispatch(down));

    same(moves, 2);
    same(last, std::optional<std::size_t>{1});

    // A move down from the last row is not a change.
    ui::KeyEvent end{ui::Code::end};

    (void)top.dispatch(end);

    same(moves, 3);

    ui::KeyEvent past{ui::Code::down};

    ought(top.dispatch(past));

    same(moves, 3);
}

TRIAL("List: leaving the list and coming back return to the same row") {
    Sheet top;

    const auto nest = std::make_shared<Nest>();
    const auto one = listed(5);
    const auto perch = std::make_shared<Perch>();

    nest->children().add(one);
    nest->children().add(perch);
    top.children().add(nest);

    one->focus();

    ui::KeyEvent down{ui::Code::down};

    (void)top.dispatch(down);
    (void)top.dispatch(down);

    same(one->which(), std::optional<std::size_t>{2});

    perch->focus();

    same(top.focused_node(), static_cast<ui::Node*>(perch.get()));
    same(one->which(), std::optional<std::size_t>{2});

    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(one->children()[2].get()));
    same(one->which(), std::optional<std::size_t>{2});
}

TRIAL("List: a letter, Tab and Esc go up") {
    Sheet top;

    const auto one = listed(3);

    top.children().add(one);

    one->focus();

    ui::KeyEvent letter{U'a'};

    ought(top.dispatch(letter));
    same(top.keys, 1);

    ui::KeyEvent tab{ui::Code::tab};

    ought(top.dispatch(tab));
    same(top.keys, 2);

    ui::KeyEvent away{ui::Code::escape};

    ought(top.dispatch(away));
    same(top.keys, 3);

    // An arrow with Control does not belong to the list either.
    ui::KeyEvent held{ui::Key{ui::Code::down, ui::Mods{true, false, false}}};

    ought(top.dispatch(held));
    same(top.keys, 4);
    same(one->which(), std::optional<std::size_t>{0});
}

TRIAL("List: Enter calls the task of the current row") {
    Sheet top;

    const auto one = listed(3);

    int done = 0;

    const ui::Hook heard_pick = one->children()[1]->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });

    top.children().add(one);

    one->focus();

    ui::KeyEvent down{ui::Code::down};

    (void)top.dispatch(down);

    ui::KeyEvent what{ui::Code::enter};

    ought(top.dispatch(what));

    same(done, 1);
    same(top.keys, 0);
}

TRIAL("List: which sets the row from the program and moves the focus") {
    Sheet top;

    const auto one = listed(6);

    top.children().add(one);

    // Without the focus the number is only remembered.
    one->which(4);

    same(one->which(), std::optional<std::size_t>{4});
    same(top.focused_node(), static_cast<ui::Node*>(nullptr));

    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(one->children()[4].get()));

    // With the focus it moves it at once, and a number out of range is clipped.
    one->which(99);

    same(one->which(), std::optional<std::size_t>{5});
    same(top.focused_node(), static_cast<ui::Node*>(one->children()[5].get()));

    one->current(*dynamic_cast<ui::ListItem*>(one->children()[1].get()));

    same(one->which(), std::optional<std::size_t>{1});
    same(top.focused_node(), static_cast<ui::Node*>(one->children()[1].get()));
}

// `which()` is the number, `current()` is the node: the same row under two names. Whoever asks for
// the node gets a row to press or read; whoever asks for the number gets an index into their own
// table next to it. An empty list gives nothing in both; a foreign row in the setter does nothing.
TRIAL("List: which is the number, current the node, and a foreign row sets nothing") {
    Sheet top;

    const auto one = listed(3);
    const auto other = listed(2);

    top.children().add(one);

    ought(one->current() == nullptr);

    ought(!one->which().has_value());

    one->which(2);

    same(one->which(), std::optional<std::size_t>{2});
    same(static_cast<ui::Node*>(one->current()), one->children()[2].get());
    same(one->current()->text(), std::string{"wiersz 2"});

    one->current(*dynamic_cast<ui::ListItem*>(other->children()[0].get()));

    same(one->which(), std::optional<std::size_t>{2});
    same(static_cast<ui::Node*>(one->current()), one->children()[2].get());

    one->current(*dynamic_cast<ui::ListItem*>(one->children()[0].get()));

    same(one->which(), std::optional<std::size_t>{0});

    const auto empty = std::make_shared<ui::List>();

    ought(empty->current() == nullptr);
    ought(!empty->which().has_value());

    empty->which(0);

    ought(empty->current() == nullptr);
    ought(!empty->which().has_value());

    // A row removed from under the number: the number points at nothing, so there is none, in
    // agreement with the node.
    one->which(2);
    (void)one->children().remove(one->children()[2].get());

    same(one->which().has_value(), one->current() != nullptr);
}

TRIAL("List: the scrollbar takes a column on the right") {
    Sheet top;

    const auto one = listed(10);

    top.children().add(one);

    (void)top.render({{0, 0}, {12, 3}});

    same(one->children().front()->area(), (ui::Area{{0, 0}, {12, 1}}));

    one->scrollbar(true);

    (void)top.render({{0, 0}, {12, 3}});

    same(one->children().front()->area(), (ui::Area{{0, 0}, {11, 1}}));
}

// The bar shows where the view is: scrolled to the last rows, the thumb stands at the bottom of
// the track and the top of the track is empty.
TRIAL("List: the scrollbar thumb follows the view to the end") {
    Sheet top;

    const auto one = listed(10);

    one->scrollbar(true);

    top.children().add(one);

    (void)top.render({{0, 0}, {12, 5}});

    View made = top.render({{0, 0}, {12, 5}});

    same(made.at({11, 0}).letter, U'█');
    same(made.at({11, 4}).letter, U'│');

    one->which(9);

    (void)top.render({{0, 0}, {12, 5}});

    made = top.render({{0, 0}, {12, 5}});

    same(one->first(), std::size_t{5});
    same(made.at({11, 0}).letter, U'│');
    same(made.at({11, 4}).letter, U'█');
}

// The scrollbar "when needed": a list shorter than the window gives its column back to the rows.
TRIAL("List: the scrollbar when needed only with a list longer than the window") {
    Sheet top;

    const auto one = std::make_shared<ui::List>(12, 3, ui::ScrollShape{true, true});

    for (int at = 0; at < 3; ++at) one->children().add(std::make_shared<ui::ListItem>("pozycja " + std::to_string(at)));

    top.children().add(one);

    (void)top.render({{0, 0}, {12, 3}});

    same(one->children().front()->area(), (ui::Area{{0, 0}, {12, 1}}));

    one->children().add(std::make_shared<ui::ListItem>("pozycja 3"));

    (void)top.render({{0, 0}, {12, 3}});

    same(one->children().front()->area(), (ui::Area{{0, 0}, {11, 1}}));
}

// A row that has moved out of view must not catch the mouse where it stood before.
TRIAL("List: a row out of view does not catch the mouse") {
    Sheet top;

    const auto one = listed(5);

    top.children().add(one);

    (void)top.render({{0, 0}, {12, 2}});

    same(one->children()[0]->area(), (ui::Area{{0, 0}, {12, 1}}));

    one->first(3);

    (void)top.render({{0, 0}, {12, 2}});

    same(one->children()[0]->area().size.high, 0);

    ui::ClickEvent what{{2, 0}, ui::Mouse::left};

    ought(top.dispatch(what));

    same(top.focused_node(), static_cast<ui::Node*>(one->children()[3].get()));
    same(one->which(), std::optional<std::size_t>{3});
}

// A row brought from another parent carries the rectangle it had there, and this list never laid
// it. Out of view here, it must not catch the mouse with that old place: being later among the
// children, it would win over the row really standing there.
TRIAL("List: a row brought from another list does not catch the mouse with its old place") {
    const auto other = listed(3);

    (void)other->render({{0, 0}, {12, 3}});

    const std::shared_ptr<ui::Node> moved = other->children().remove(other->children()[1].get());

    same(moved->area(), (ui::Area{{0, 1}, {12, 1}}));

    Sheet top;

    const auto one = listed(10);

    top.children().add(one);

    (void)top.render({{0, 0}, {12, 3}});

    one->children().add(moved);

    (void)top.render({{0, 0}, {12, 3}});

    same(moved->area().size.high, 0);

    ui::ClickEvent what{{2, 1}, ui::Mouse::left};

    ought(top.dispatch(what));

    same(one->which(), std::optional<std::size_t>{1});
}

// After every frame, whatever happened in between (scrolling, rows added at the front, removed,
// replaced one for one, brought from another list), exactly the rows in view have a rectangle,
// each where it is drawn, and every other row has none. Checked against the definition, row by
// row, over a long random run.
TRIAL("List: after every frame exactly the rows in view have a rectangle") {
    std::mt19937 dice{1234};

    const auto other = listed(4);
    const auto one = listed(30);

    constexpr int high = 5;

    for (int round = 0; round < 400; ++round) {
        const int much = static_cast<int>(one->children().many());

        switch (dice() % 6) {
            case 0:
            case 1: one->first(dice() % static_cast<unsigned>(much + 1)); break;

            case 2:
                if (much > 1) (void)one->children().remove(one->children()[dice() % static_cast<unsigned>(much)].get());
                break;

            case 3: {
                const auto made = std::make_shared<ui::ListItem>("nowy " + std::to_string(round));

                one->children().add(made);
                break;
            }

            case 4: {
                if (much > 1) (void)one->children().remove(one->children()[dice() % static_cast<unsigned>(much)].get());

                one->children().add(std::make_shared<ui::ListItem>("zamiana " + std::to_string(round)));
                break;
            }

            default: {
                (void)other->render({{0, 0}, {12, static_cast<int>(other->children().many())}});

                if (other->children().many() > 1) {
                    const std::shared_ptr<ui::Node> moved = other->children().remove(other->children()[1].get());

                    one->children().add(moved);
                }

                other->children().add(std::make_shared<ui::ListItem>("obcy " + std::to_string(round)));
                break;
            }
        }

        (void)one->render({{0, 0}, {12, high}});

        const int shift = static_cast<int>(one->first());
        const int total = static_cast<int>(one->children().many());

        for (int at = 0; at < total; ++at) {
            const ui::Area& box = one->children()[static_cast<std::size_t>(at)]->area();

            if (at >= shift && at < shift + high) {
                same(box, (ui::Area{{0, at - shift}, {12, 1}}));
            } else {
                same(box.size.high, 0);
            }
        }
    }
}

// A frame of a long list costs its window, not its length: a thousand frames of a list of two
// hundred thousand rows take a fraction of a second, where walking all the rows in every frame
// took several seconds.
TRIAL("List: a frame of a long list costs its window") {
    const auto one = listed(200000);

    (void)one->render({{0, 0}, {40, 20}});

    const auto began = std::chrono::steady_clock::now();

    for (int frame = 0; frame < 1000; ++frame) {
        one->first(static_cast<std::size_t>(frame % 2));

        (void)one->render({{0, 0}, {40, 20}});
    }

    ought(std::chrono::steady_clock::now() - began < std::chrono::seconds{1});
}

TRIAL("List: a click on a row makes it current") {
    Sheet top;

    const auto one = listed(4);

    int moves = 0;

    const ui::Hook heard_scroll = one->on<ui::ScrollEvent>([&moves](ui::ScrollEvent&) {
        ++moves;

        return false;
    });

    top.children().add(one);

    (void)top.render({{0, 0}, {12, 4}});

    ui::ClickEvent what{{3, 2}, ui::Mouse::left};

    ought(top.dispatch(what));

    same(one->which(), std::optional<std::size_t>{2});
    same(moves, 1);
}

TRIAL("List: wants as much as it was told") {
    const auto one = listed(3, 20, 6);

    same(one->wants({40, 10}), (ui::Size{20, 6}));
    same(one->wants({10, 4}), (ui::Size{10, 4}));

    const auto other = listed(3);

    same(other->wants({40, 10}), (ui::Size{40, 10}));
}

// --- What the list accepts -----------------------------------------------------------------------

// The number of the current row counts among the children, so a foreign child would throw the
// numbering out of step with what is on the screen. We refuse once, at attaching.
TRIAL("List: accepts rows, not a foreign child") {
    const auto one = std::make_shared<ui::List>();

    one->children().add(std::make_shared<ui::ListItem>("plik"));

    same(one->children().many(), std::size_t{1});

    bool threw = false;

    try {
        one->children().add(std::make_shared<ui::Span>("obce"));
    } catch (const std::logic_error&) {
        threw = true;
    }

    ought(threw);
    same(one->children().many(), std::size_t{1});
}

// The number of the current row is found from the row the list stood on, not from the first one:
// walking with the arrow deep in a long list costs a step per key, so holding the key does not grow
// quadratic. Ten thousand steps from row 150 000 of 200 000 take a fraction of a second, where
// counting from the top took seconds.
TRIAL("List: an arrow deep in a long list finds its row at once") {
    Sheet top;

    const auto one = listed(200000);

    top.children().add(one);

    (void)top.render({{0, 0}, {20, 10}});

    one->focus();
    one->which(150000);

    const auto began = std::chrono::steady_clock::now();

    for (int step = 0; step < 10000; ++step) {
        ui::KeyEvent down{ui::Code::down};

        ought(top.dispatch(down));
    }

    ought(std::chrono::steady_clock::now() - began < std::chrono::milliseconds{500});
    same(one->which(), std::optional<std::size_t>{160000});
}
