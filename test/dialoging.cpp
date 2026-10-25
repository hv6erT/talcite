#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/modals.hpp"
#include "talcite/encoding.hpp"
#include "talcite/frame.hpp"
#include "talcite/keybaritem.hpp"
#include "talcite/keybar.hpp"
#include "talcite/root.hpp"
#include "talcite/span.hpp"

using namespace talcite;

#include "doing.hpp"

namespace {

// A focusable node of a fixed size, filled with one character, that counts the clicks it gets.
class Box : public ui::Node {
public:
    Box(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    // Always takes the focus.
    [[nodiscard]] bool focusable() const override { return true; }

    // Always the size it was made with.
    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

    int hit = 0;

protected:
    // Fills the given size with its character.
    View paint(ui::Size given, ui::Area seen) override {
        View made{seen.size};

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                made.put({x - seen.at.x, y - seen.at.y}, what_, Style{});

        return made;
    }

    // Counts a click.
    bool handle(ui::ClickEvent&) override {
        ++hit;

        return true;
    }

private:
    ui::Size wants_;
    char32_t what_ = U'?';
};

// A widget that takes the focus itself and has focusable nodes inside; a list with items has
// exactly this shape. The shape alone is enough here, without a list.
class Nest : public ui::Node {
public:
    // Takes the focus itself.
    [[nodiscard]] bool focusable() const override { return true; }

    // Takes any number of children.
    [[nodiscard]] int capacity() const override { return ui::plenty; }

    // Four cells by two.
    [[nodiscard]] ui::Size wants(ui::Size) const override { return {4, 2}; }
};

// The same, but it passes the focus to its inside instead of taking it itself.
class Deep : public Nest {
public:
    // Focuses the first child, or itself when it has none.
    void focus() override {
        if (children().empty()) {
            Node::focus();

            return;
        }

        children().front()->focus();
    }
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

TRIAL("Dialog: the box is centered, with room for the shadow") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto under = std::make_shared<Box>(ui::Size{ui::plenty, ui::plenty}, U'x');
    const auto one = std::make_shared<ui::Dialog>();

    one->children().add(std::make_shared<Box>(ui::Size{6, 3}, U'o'));

    window->content(under);
    top.children().add(window);
    window->open(one);

    (void)top.render({{0, 0}, {20, 10}});

    // The room for the shadow (two columns and a row) is subtracted before centering.
    same(one->children().front()->area(), (ui::Area{{6, 3}, {6, 3}}));
}

// The dialog is the size of the screen, but the content below shows through everything outside
// the box. A fresh sheet is filled with the drawer's spaces, so without starting from emptiness
// the dialog would paint over the whole screen.
TRIAL("Dialog: outside the box what lies below shows through") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto under = std::make_shared<Box>(ui::Size{ui::plenty, ui::plenty}, U'x');
    const auto one = std::make_shared<ui::Dialog>();

    one->children().add(std::make_shared<Box>(ui::Size{4, 1}, U'o'));

    window->content(under);
    top.children().add(window);
    window->open(one);

    const View made = top.render({{0, 0}, {10, 3}});

    // The screen is ten by three, the shadow takes two columns and a row, so the four by one box
    // lands in row zero, at column two. Everywhere else the content below shows.
    same(read(made, 0), std::string{"xxooooxxxx"});
    same(read(made, 1), std::string{"xxxxxxxxxx"});
    same(read(made, 2), std::string{"xxxxxxxxxx"});
}

// The shadow falls to the right and down, shifted by a row and two columns, the same rule as under
// a menu. The letters below stay, only dimmed.
TRIAL("Dialog: the shadow dims, it does not paint over") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto under = std::make_shared<Box>(ui::Size{ui::plenty, ui::plenty}, U'x');
    const auto one = std::make_shared<ui::Dialog>();

    one->children().add(std::make_shared<Box>(ui::Size{6, 3}, U'o'));

    window->content(under);
    top.children().add(window);
    window->open(one);

    const View made = top.render({{0, 0}, {20, 10}});

    // The box stands at {6,3} and is 6 by 3, so the shadow on the right takes columns 12–13, rows
    // 4–5.
    ought(made.at({12, 4}).style.faint);
    ought(made.at({13, 5}).style.faint);
    same(made.at({12, 4}).letter, U'x');

    // Below: row 6, columns 8–13.
    ought(made.at({8, 6}).style.faint);
    ought(made.at({13, 6}).style.faint);

    // Just above the right shadow and just beside the bottom shadow there is plain content.
    ought(!made.at({12, 3}).style.faint);
    ought(!made.at({7, 6}).style.faint);
}

TRIAL("Dialog: without a shadow the box is centered on the whole screen") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Dialog>(ui::ModalShape{false});

    one->children().add(std::make_shared<Box>(ui::Size{6, 4}, U'o'));

    top.children().add(window);
    window->open(one);

    (void)top.render({{0, 0}, {20, 10}});

    same(one->children().front()->area(), (ui::Area{{7, 3}, {6, 4}}));
}

// Mouse modality: the dialog is the size of the screen and lies one layer up, so a click beside
// the box hits the dialog, not the content below.
TRIAL("Dialog: a click beside the box does not reach below") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto under = std::make_shared<Box>(ui::Size{ui::plenty, ui::plenty}, U'x');
    const auto inside = std::make_shared<Box>(ui::Size{6, 3}, U'o');
    const auto one = std::make_shared<ui::Dialog>();

    one->children().add(inside);
    window->content(under);
    top.children().add(window);
    window->open(one);

    (void)top.render({{0, 0}, {20, 10}});

    ui::ClickEvent aside{{0, 0}, ui::Mouse::left};

    ought(top.dispatch(aside));
    same(under->hit, 0);

    // A click on the box reaches its content.
    ui::ClickEvent within{{7, 4}, ui::Mouse::left};

    ought(top.dispatch(within));
    same(inside->hit, 1);
    same(under->hit, 0);
}

// Keyboard modality: the focus sits in the dialog, so a key has no way to reach the key bar below.
// This is the same way an open menu swallows F5.
TRIAL("Dialog: a key does not reach the key bar below") {
    proving::Hooks heard;

    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto bar = std::make_shared<ui::KeyBar>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');

    int done = 0;

    bar->content(body);
    (void)proving::doing(heard, bar->add(ui::Code::f5, "Kopiuj"), [&done] { ++done; });
    window->content(bar);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {20, 10}});

    // Without a dialog the key bar works.
    ui::KeyEvent first{ui::Code::f5};

    ought(top.dispatch(first));
    same(done, 1);

    const auto one = std::make_shared<ui::Dialog>();

    one->children().add(std::make_shared<Box>(ui::Size{6, 3}, U'o'));

    window->open(one);

    (void)top.render({{0, 0}, {20, 10}});

    ui::KeyEvent second{ui::Code::f5};

    ought(top.dispatch(second));
    same(done, 1);
}

// Escape does not close a dialog. If the modal took it, the window would perform an action the
// caller could not cancel. Whoever wants such closing hangs a handler; this trial shows both halves
// at once, because without a handler Esc is only swallowed here.
TRIAL("Dialog: Esc closes only with the caller's handler") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');
    const auto one = std::make_shared<ui::Dialog>();

    const auto inside = std::make_shared<Box>(ui::Size{6, 3}, U'o');

    one->children().add(inside);

    window->content(body);
    top.children().add(window);

    body->focus();

    window->open(one);

    // The focus goes into the dialog, to the first focusable node, not to the dialog itself.
    same(top.focused_node(), static_cast<ui::Node*>(inside.get()));
    same(window->current(), static_cast<ui::Modal*>(one.get()));
    same(window->capacity(), 2);

    (void)top.render({{0, 0}, {20, 10}});

    ui::KeyEvent what{ui::Code::escape};

    ought(top.dispatch(what));

    // Esc alone closed nothing; modality swallowed it, and that is all.
    ought(!window->empty());

    const std::weak_ptr<ui::Dialog> weak = one;

    const ui::Hook heard_key = one->on<ui::KeyEvent>([weak](ui::KeyEvent& said) {
        if (said.key().code() != ui::Code::escape) return false;

        if (const auto mine = weak.lock()) mine->close();

        return true;
    });

    ought(top.dispatch(what));

    ought(window->empty());
    same(window->current(), static_cast<ui::Modal*>(nullptr));
    same(window->capacity(), 1);
    same(top.focused_node(), static_cast<ui::Node*>(body.get()));
}

// A stack: every dialog remembers its own focus, so closing the top one returns to the one below
// it, and closing the last one returns to the application. A single field in the window would have
// to choose which one to remember.
TRIAL("Dialog: the stack returns the focus layer by layer") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');
    const auto one = std::make_shared<ui::Dialog>();
    const auto two = std::make_shared<ui::Dialog>();

    const auto first = std::make_shared<Box>(ui::Size{6, 3}, U'o');
    const auto second = std::make_shared<Box>(ui::Size{4, 1}, U'p');

    one->children().add(first);
    two->children().add(second);

    window->content(body);
    top.children().add(window);

    body->focus();

    window->open(one);
    window->open(two);

    same(window->current(), static_cast<ui::Modal*>(two.get()));
    same(top.focused_node(), static_cast<ui::Node*>(second.get()));
    same(static_cast<int>(window->many()), 2);

    (void)top.render({{0, 0}, {20, 10}});

    // The later one lies on top.
    same(second->area(), (ui::Area{{7, 4}, {4, 1}}));

    window->close();

    same(window->current(), static_cast<ui::Modal*>(one.get()));
    same(top.focused_node(), static_cast<ui::Node*>(first.get()));

    window->close();

    ought(window->empty());
    same(top.focused_node(), static_cast<ui::Node*>(body.get()));
}

TRIAL("Dialog: closing by handle removes the one meant") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Dialog>();
    const auto two = std::make_shared<ui::Dialog>();

    one->children().add(std::make_shared<Box>(ui::Size{6, 3}, U'o'));
    two->children().add(std::make_shared<Box>(ui::Size{4, 1}, U'p'));

    top.children().add(window);

    window->open(one);
    window->open(two);

    // The lower one is removed, not the top one.
    window->close(*one);

    same(static_cast<int>(window->many()), 1);
    same(window->current(), static_cast<ui::Modal*>(two.get()));

    // Closing it by its own hand does the same.
    two->close();

    ought(window->empty());
}

TRIAL("Dialog: content larger than the screen is clipped to it") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Dialog>();

    one->children().add(std::make_shared<Box>(ui::Size{ui::plenty, ui::plenty}, U'o'));

    top.children().add(window);
    window->open(one);

    (void)top.render({{0, 0}, {10, 4}});

    // The shadow takes two columns and a row, so eight by three is left for the box.
    same(one->children().front()->area(), (ui::Area{{0, 0}, {8, 3}}));
}

TRIAL("Dialog: the box is an ordinary frame with a title") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Dialog>();
    const auto frame = std::make_shared<ui::Frame>("Uwaga");

    frame->children().add(std::make_shared<Box>(ui::Size{7, 1}, U'o'));
    one->children().add(frame);

    top.children().add(window);
    window->open(one);

    const View made = top.render({{0, 0}, {21, 6}});

    // The frame adds a cell on each side and fits its title, which is longer than the content
    // (five letters and four cells around them): eleven by three.
    same(frame->area(), (ui::Area{{4, 1}, {11, 3}}));

    ought(read(made, 1).find("Uwaga") != std::string::npos);
    ought(read(made, 2).find("ooooooo") != std::string::npos);
}

// A focusable widget is a leaf for the walk: Tab stops on it, not on each of its focusable
// children separately. Without this rule a window with a list of a hundred files would have a
// hundred stops.
TRIAL("Dialog: a focusable widget is one stop, not as many as it has children") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();

    const auto first = std::make_shared<Box>(ui::Size{4, 1}, U'a');
    const auto nest = std::make_shared<Nest>();
    const auto inner = std::make_shared<Box>(ui::Size{4, 1}, U'i');
    const auto other = std::make_shared<Box>(ui::Size{4, 1}, U'j');
    const auto last = std::make_shared<Box>(ui::Size{4, 1}, U'z');

    nest->children().add(inner);
    nest->children().add(other);

    column->children().add(first);
    column->children().add(nest);
    column->children().add(last);

    one->children().add(column);

    top.children().add(window);
    window->open(one);

    same(top.focused_node(), static_cast<ui::Node*>(first.get()));

    ui::KeyEvent step{ui::Code::tab};

    ought(top.dispatch(step));
    same(top.focused_node(), static_cast<ui::Node*>(nest.get()));

    ui::KeyEvent again{ui::Code::tab};

    ought(top.dispatch(again));
    same(top.focused_node(), static_cast<ui::Node*>(last.get()));

    // Wrapping around at the end, and not once on the way did we stop inside the nest.
    ui::KeyEvent round{ui::Code::tab};

    ought(top.dispatch(round));
    same(top.focused_node(), static_cast<ui::Node*>(first.get()));
}

// A focus that went deeper than the stop still belongs to the stop: a Tab step counts from the
// widget, not from where the focus really sits.
TRIAL("Dialog: the focus inside a widget counts as the focus on it") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();

    const auto first = std::make_shared<Box>(ui::Size{4, 1}, U'a');
    const auto deep = std::make_shared<Deep>();
    const auto inner = std::make_shared<Box>(ui::Size{4, 1}, U'i');
    const auto last = std::make_shared<Box>(ui::Size{4, 1}, U'z');

    deep->children().add(inner);

    column->children().add(first);
    column->children().add(deep);
    column->children().add(last);

    one->children().add(column);

    top.children().add(window);
    window->open(one);

    same(top.focused_node(), static_cast<ui::Node*>(first.get()));

    // A step onto the nest puts the focus inside it, because the nest itself decides so.
    ui::KeyEvent step{ui::Code::tab};

    ought(top.dispatch(step));
    same(top.focused_node(), static_cast<ui::Node*>(inner.get()));

    // The next step leaves the nest instead of circling inside it.
    ui::KeyEvent again{ui::Code::tab};

    ought(top.dispatch(again));
    same(top.focused_node(), static_cast<ui::Node*>(last.get()));

    // Backwards returns to the inside of the nest, again through the nest itself.
    ui::KeyEvent back{ui::Key{ui::Code::tab, ui::Mods{false, false, true}}};

    ought(top.dispatch(back));
    same(top.focused_node(), static_cast<ui::Node*>(inner.get()));
}

// The same limit as in the walk, only for shortcut keys and the default button: a button hidden
// inside a focusable widget does not answer from the whole window. If it did, a letter typed inside
// such a widget would press something that cannot be seen from it.
TRIAL("Dialog: a shortcut key does not reach inside a focusable widget") {
    proving::Hooks heard;

    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto nest = std::make_shared<Nest>();

    int hidden = 0;
    int plain = 0;

    const auto inside =
        proving::doing(heard, std::make_shared<ui::Button>("Tak"), [&hidden] { ++hidden; });
    const auto outside = proving::doing(heard, std::make_shared<ui::Button>("Nie"), [&plain] { ++plain; });

    inside->keys().add(U'T');
    inside->primary(true);

    outside->keys().add(U'N');

    nest->children().add(inside);
    column->children().add(nest);
    column->children().add(outside);

    one->children().add(column);

    top.children().add(window);
    window->open(one);

    ui::KeyEvent held{U'T'};

    ought(top.dispatch(held));
    same(hidden, 0);

    ui::KeyEvent free{U'N'};

    ought(top.dispatch(free));
    same(plain, 1);

    // The default button has the same limit: Enter does not find it inside the nest.
    ui::KeyEvent what{ui::Code::enter};

    ought(top.dispatch(what));
    same(hidden, 0);
}

TRIAL("Dialog: an empty one draws nothing and covers nothing") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto under = std::make_shared<Box>(ui::Size{ui::plenty, ui::plenty}, U'x');
    const auto one = std::make_shared<ui::Dialog>();

    window->content(under);
    top.children().add(window);
    window->open(one);

    const View made = top.render({{0, 0}, {8, 2}});

    same(read(made, 0), std::string{"xxxxxxxx"});
    same(read(made, 1), std::string{"xxxxxxxx"});
}

// A shortcut key is an event, not a question about a type. The window delivers it downwards, so a
// handler hung by the application takes it on equal terms with the button's built-in behavior:
// one mechanism, not two.
TRIAL("Dialog: a handler for a shortcut key works like the built-in one") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto span = std::make_shared<ui::Span>("nie jestem przyciskiem");

    column->children().add(span);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    int done = 0;

    const ui::Hook heard_shortcut = span->on<ui::ShortcutEvent>([&done](ui::ShortcutEvent& what) {
        if (what.key() != ui::Key{U'z'}) return false;

        ++done;

        return true;
    });

    ui::KeyEvent other{ui::Key{U'x'}};

    ought(top.dispatch(other));
    same(done, 0);

    ui::KeyEvent mine{ui::Key{U'z'}};

    ought(top.dispatch(mine));
    same(done, 1);
}

// A directed event does not travel up: if it did, it would come back from the child to the
// window, enter the delivery a second time, and so on forever.
TRIAL("Dialog: a delivered key does not come back up") {
    ui::Root top;

    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto span = std::make_shared<ui::Span>("napis");

    column->children().add(span);
    dialog->children().add(column);

    top.children().add(window);
    window->open(dialog);

    int above = 0;

    const ui::Hook heard_shortcut_1 = column->on<ui::ShortcutEvent>([&above](ui::ShortcutEvent&) {
        ++above;

        return false;
    });

    int under = 0;

    const ui::Hook heard_shortcut_2 = span->on<ui::ShortcutEvent>([&under](ui::ShortcutEvent&) {
        ++under;

        return false;
    });

    ui::KeyEvent what{ui::Key{U'z'}};

    (void)top.dispatch(what);

    // The column gets it once as itself, not a second time bubbling up from the text.
    same(above, 1);
    same(under, 1);
}
