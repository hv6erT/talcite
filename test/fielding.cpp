#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/modals.hpp"
#include "talcite/encoding.hpp"
#include "talcite/field.hpp"
#include "talcite/frame.hpp"
#include "talcite/root.hpp"
#include "talcite/scroll.hpp"
#include "talcite/skin.hpp"

using namespace talcite;

#include "doing.hpp"

namespace {

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

// Typing goes through keys, because that is how the user types: the trial then checks the same path
// that really runs, not a shortcut next to it.
void typed(ui::Root& top, const std::string& what) {
    std::size_t at = 0;

    while (at < what.size()) {
        ui::KeyEvent one{utf8::decode(what, at)};

        (void)top.dispatch(one);
    }
}

// Dispatches one key press to the root.
void pressed(ui::Root& top, ui::Code code, ui::Mods mods = {}) {
    ui::KeyEvent one{ui::Key{code, mods}};

    (void)top.dispatch(one);
}

}

TRIAL("Field: typing puts characters at the cursor") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10);

    top.children().add(one);

    one->focus();

    typed(top, "abc");

    same(one->text(), std::string{"abc"});
    same(one->cursor(), static_cast<std::size_t>(3));

    pressed(top, ui::Code::left);
    pressed(top, ui::Code::left);

    typed(top, "X");

    same(one->text(), std::string{"aXbc"});
    same(one->cursor(), static_cast<std::size_t>(2));

    same(read(top.render({{0, 0}, {10, 1}}), 0), std::string{"aXbc"});
}

TRIAL("Field: Backspace deletes before the cursor, Delete under it") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10, "abcd");

    top.children().add(one);

    one->focus();

    pressed(top, ui::Code::backspace);

    same(one->text(), std::string{"abc"});

    pressed(top, ui::Code::home);
    pressed(top, ui::Code::remove);

    same(one->text(), std::string{"bc"});
    same(one->cursor(), static_cast<std::size_t>(0));

    // At the edges there is nothing to delete and nothing happens.
    pressed(top, ui::Code::backspace);

    same(one->text(), std::string{"bc"});

    pressed(top, ui::Code::end);
    pressed(top, ui::Code::remove);

    same(one->text(), std::string{"bc"});
}

TRIAL("Field: the cursor counts characters, not bytes") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10, "zażółć");

    top.children().add(one);

    one->focus();

    // Six characters, though more bytes.
    same(one->cursor(), static_cast<std::size_t>(6));

    pressed(top, ui::Code::left);

    same(one->cursor(), static_cast<std::size_t>(5));

    pressed(top, ui::Code::backspace);

    same(one->text(), std::string{"zażóć"});

    one->cursor(2);

    same(one->cursor(), static_cast<std::size_t>(2));

    typed(top, "X");

    same(one->text(), std::string{"zaXżóć"});
}

TRIAL("Field: a paste goes in at the cursor, with line ends as spaces") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(20, "ab");

    top.children().add(one);

    one->focus();

    ui::PasteEvent what{"raz\ndwa"};

    ought(top.dispatch(what));

    same(one->text(), std::string{"abraz dwa"});
    same(one->cursor(), static_cast<std::size_t>(9));
}

// A field scrolls rather than cuts: an ellipsis in the text being typed would be a lie about what
// it contains.
TRIAL("Field: scrolls so that the cursor is visible") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(5, "abcdefgh");

    top.children().add(one);

    one->focus();

    const View made = top.render({{0, 0}, {5, 1}});

    same(read(made, 0), std::string{"efgh"});

    ought(top.caret().has_value());
    same(*top.caret(), (ui::Spot{4, 0}));

    // Going back to the start scrolls back.
    pressed(top, ui::Code::home);

    const View other = top.render({{0, 0}, {5, 1}});

    same(read(other, 0), std::string{"abcde"});
    same(*top.caret(), (ui::Spot{0, 0}));
}

// At rest the field is not being typed in, so the ellipsis tells no lie there: a field without the
// focus that cannot hold its text shows the beginning and ends with the ellipsis. With the focus
// back, it scrolls to the cursor again.
TRIAL("Field: without the focus a text that does not fit ends with the ellipsis") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(5, "abcdefgh");
    const auto other = std::make_shared<ui::Field>(5, "ab");
    const auto both = std::make_shared<ui::Column>();

    both->children().add(one);
    both->children().add(other);
    top.children().add(both);

    one->focus();

    same(read(top.render({{0, 0}, {5, 2}}), 0), std::string{"efgh"});

    other->focus();

    const View rest = top.render({{0, 0}, {5, 2}});

    same(read(rest, 0), std::string{"abcd…"});
    same(read(rest, 1), std::string{"ab"});
    same(one->text(), std::string{"abcdefgh"});

    one->focus();

    same(read(top.render({{0, 0}, {5, 2}}), 0), std::string{"efgh"});
}

TRIAL("Field: a double-width character takes two cells") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(6, "日本ab");

    top.children().add(one);

    one->focus();

    const View made = top.render({{0, 0}, {6, 1}});

    // Six cells of content leave no room for the cursor after it, so the first character leaves the
    // view.
    same(read(made, 0), std::string{"本ab"});
    same(*top.caret(), (ui::Spot{4, 0}));
}

TRIAL("Field: the cursor is visible only with the focus") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(6, "ab");

    top.children().add(one);

    (void)top.render({{0, 0}, {6, 1}});

    ought(!top.caret().has_value());

    one->focus();

    (void)top.render({{0, 0}, {6, 1}});

    ought(top.caret().has_value());
}

// A mouse event carries a screen cell, not the node's own. Subtracting only `area().at` would count
// one level only, so a field in a frame in a frame (that is, in every dialog) would put the cursor
// as many cells off as the ancestors are offset.
TRIAL("Field: a click hits the right character also deep in the tree") {
    ui::Root top;

    const auto outer = std::make_shared<ui::Frame>("A");
    const auto inner = std::make_shared<ui::Frame>("B");
    const auto one = std::make_shared<ui::Field>(20, "abcdefgh");

    inner->children().add(one);
    outer->children().add(inner);
    top.children().add(outer);

    (void)top.render({{0, 0}, {30, 8}});

    // Two frames of one cell on each side, so character number 3 stands in screen column 5.
    same(one->area().at, (ui::Spot{1, 1}));

    ui::ClickEvent what{{5, 2}, ui::Mouse::left};

    ought(top.dispatch(what));
    same(one->cursor(), static_cast<std::size_t>(3));
}

TRIAL("Field: a click puts the cursor where it clicked") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10, "abcdef");

    top.children().add(one);

    (void)top.render({{0, 0}, {10, 1}});

    ui::ClickEvent what{{3, 0}, ui::Mouse::left};

    ought(top.dispatch(what));

    same(one->cursor(), static_cast<std::size_t>(3));
    same(top.focused_node(), static_cast<ui::Node*>(one.get()));

    // A click past the content puts the cursor at its end.
    ui::ClickEvent aside{{9, 0}, ui::Mouse::left};

    ought(top.dispatch(aside));

    same(one->cursor(), static_cast<std::size_t>(6));
}

TRIAL("Field: a disabled field takes neither the focus nor characters") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10, "ab");

    top.children().add(one);

    one->enabled(false);

    ought(!one->focusable());

    one->focus();

    same(top.focused_node(), static_cast<ui::Node*>(nullptr));

    typed(top, "c");

    same(one->text(), std::string{"ab"});
}

// A suggestion is a grayed-out addition after the content and does not belong to the content until
// someone accepts it.
TRIAL("Suggestion: drawn grayed out and not part of the content") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(12);

    one->guess([](const std::string& what) {
        return what.empty() ? std::string{} : std::string{"katalog"};
    });

    top.children().add(one);

    one->focus();

    typed(top, "ka");

    same(one->text(), std::string{"ka"});
    same(one->guessed(), std::string{"talog"});

    const View made = top.render({{0, 0}, {12, 1}});

    same(read(made, 0), std::string{"katalog"});

    // The content is normal, the addition faint.
    ought(!made.at({1, 0}).style.faint);
    ought(made.at({2, 0}).style.faint);
    ought(made.at({6, 0}).style.faint);

    // The cursor stands at the end of the content, not of the suggestion.
    same(*top.caret(), (ui::Spot{2, 0}));
}

TRIAL("Suggestion: the right arrow accepts one letter") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(12);

    one->guess([](const std::string&) { return std::string{"katalog"}; });

    top.children().add(one);

    one->focus();

    typed(top, "ka");

    pressed(top, ui::Code::right);

    same(one->text(), std::string{"kat"});
    same(one->guessed(), std::string{"alog"});

    pressed(top, ui::Code::right);

    same(one->text(), std::string{"kata"});
}

// A field does not take Tab. If it completed the suggestion only when there was one, the same key
// would sometimes move to the next widget and sometimes not, depending on the field's content. A
// navigation key must not disappear depending on the content.
TRIAL("Suggestion: Tab does not complete it and goes up") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(12);

    one->guess([](const std::string&) { return std::string{"katalog"}; });

    top.children().add(one);

    one->focus();

    typed(top, "ka");

    ui::KeyEvent tab{ui::Code::tab};

    ought(!top.dispatch(tab));

    same(one->text(), std::string{"ka"});
    same(one->guessed(), std::string{"talog"});
}

TRIAL("Suggestion: appears only with the cursor at the end") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(12);

    one->guess([](const std::string&) { return std::string{"katalog"}; });

    top.children().add(one);

    one->focus();

    typed(top, "ka");

    ought(!one->guessed().empty());

    pressed(top, ui::Code::left);

    ought(one->guessed().empty());

    pressed(top, ui::Code::end);

    ought(!one->guessed().empty());
}

TRIAL("Suggestion: one that does not match the content is dropped") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(12);

    // A suggestion that does not start with what was typed cannot be appended.
    one->guess([](const std::string&) { return std::string{"inne"}; });

    top.children().add(one);

    one->focus();

    typed(top, "ka");

    ought(one->guessed().empty());

    ui::KeyEvent tab{ui::Code::tab};

    // Without a suggestion, Tab does not belong to the field either.
    ought(!top.dispatch(tab));
}

namespace {

// The parts of the naming dialog that the trials reach into.
struct Asked {
    std::shared_ptr<ui::Modals> window;
    std::shared_ptr<ui::Dialog> dialog;
    std::shared_ptr<ui::Field> field;
    std::shared_ptr<ui::Button> yes;
    std::shared_ptr<ui::Button> no;
};

// Opens a dialog with a field and two buttons, "Tak" (primary, hot key T, counting its presses in
// `done`) and "Nie" (hot key N), puts the focus in the field and renders it once.
Asked naming(ui::Root& top, int* done) {
    Asked made;

    made.window = std::make_shared<ui::Modals>();
    made.dialog = std::make_shared<ui::Dialog>();
    made.field = std::make_shared<ui::Field>(12);
    made.yes = proving::doing(std::make_shared<ui::Button>("Tak"), [done] { ++*done; });
    made.no = std::make_shared<ui::Button>("Nie");

    made.yes->keys().add(U'T');
    made.no->keys().add(U'N');
    made.yes->primary(true);

    const auto column = std::make_shared<ui::Column>();

    column->children().add(made.field);
    column->children().add(made.yes);
    column->children().add(made.no);
    made.dialog->children().add(column);

    top.children().add(made.window);
    made.window->open(made.dialog);

    made.field->focus();

    (void)top.render({{0, 0}, {30, 10}});

    return made;
}

}

// This is the collision that matters most for a field. A letter in a dialog presses the button that
// has it among its hot keys, but a field with the focus must type it. Bubbling settles this by
// itself, because the field lies closer to the focus, but the rule has to be checked.
TRIAL("Dialog: a letter in a field is typed, not pressing a button") {
    ui::Root top;

    int done = 0;

    const Asked asked = naming(top, &done);

    typed(top, "t");

    same(asked.field->text(), std::string{"t"});
    same(done, 0);

    // Without the focus in the field, the same letter presses the button.
    asked.no->focus();

    typed(top, "t");

    same(asked.field->text(), std::string{"t"});
    same(done, 1);
}

// The field lets Enter through, and the window does not intercept it. So nobody confirms the form
// for the caller: whoever wants that listens for `SubmitEvent` and decides alone what "done" means.
// The veil swallows the key, because it is modal, but presses nothing with it.
TRIAL("Dialog: Enter in a field presses no button") {
    ui::Root top;

    int done = 0;

    const Asked asked = naming(top, &done);

    typed(top, "nazwa");

    ui::KeyEvent enter{ui::Code::enter};

    ought(top.dispatch(enter));

    same(done, 0);
    same(asked.field->text(), std::string{"nazwa"});
}

// Enter in a field is a `SubmitEvent`, and the event goes up: one hook on the window hears every
// field and presses whatever the caller deems OK. The field does not know the button. Enter on a
// button with the focus presses that button and no `SubmitEvent` arises; otherwise Enter on "Nie"
// would confirm the window.
TRIAL("Dialog: Enter in a field announces a SubmitEvent that the window hears") {
    ui::Root top;

    int done = 0;

    const Asked asked = naming(top, &done);

    int submitted = 0;

    (void)asked.dialog->on<ui::SubmitEvent>([&submitted, yes = asked.yes](ui::SubmitEvent&) {
        ++submitted;

        yes->pick();

        return true;
    });

    typed(top, "nazwa");

    ui::KeyEvent enter{ui::Code::enter};

    ought(top.dispatch(enter));

    same(submitted, 1);
    same(done, 1);
    same(asked.field->text(), std::string{"nazwa"});

    int refused = 0;

    (void)asked.no->on<ui::PickEvent>([&refused](ui::PickEvent&) {
        ++refused;

        return true;
    });

    asked.no->focus();

    ui::KeyEvent again{ui::Code::enter};

    ought(top.dispatch(again));

    same(refused, 1);
    same(submitted, 1);
    same(done, 1);
}

// Esc in a field does not close the window: the field does not take it, and the veil only swallows
// it. Whoever wants closing attaches a hook to their own window.
TRIAL("Dialog: Esc in a field does not close the window") {
    ui::Root top;

    int done = 0;

    const Asked asked = naming(top, &done);

    ui::KeyEvent what{ui::Code::escape};

    ought(top.dispatch(what));

    ought(!asked.window->empty());
}

// Tab belongs to focus navigation and only to it: the field does not take it, so there is no place
// where the two meanings of Tab meet.
TRIAL("Dialog: Tab in a field without a suggestion moves to the button") {
    ui::Root top;

    int done = 0;

    const Asked asked = naming(top, &done);

    ui::KeyEvent tab{ui::Code::tab};

    ought(top.dispatch(tab));

    same(top.focused_node(), static_cast<ui::Node*>(asked.yes.get()));
}

// Tab always moves the focus, also when the field has something to complete. Otherwise navigation
// through the window would depend on the field's content.
TRIAL("Dialog: Tab in a field with a suggestion moves the focus at once") {
    ui::Root top;

    int done = 0;

    const Asked asked = naming(top, &done);

    asked.field->guess([](const std::string&) { return std::string{"nazwa"}; });

    typed(top, "na");

    ui::KeyEvent tab{ui::Code::tab};

    ought(top.dispatch(tab));

    same(asked.field->text(), std::string{"na"});
    same(top.focused_node(), static_cast<ui::Node*>(asked.yes.get()));
}

// A numeric field is one lambda, not a separate widget. Midnight Commander has no numeric widget
// (numbers are typed into its ordinary input widget), so there is nothing to imitate here.
TRIAL("Field: the filter drops characters while typing") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10);

    one->allows([](char32_t which) { return which >= U'0' && which <= U'9'; });

    top.children().add(one);

    one->focus();

    typed(top, "a1b2c3");

    same(one->text(), std::string{"123"});
    same(one->cursor(), static_cast<std::size_t>(3));
}

TRIAL("Field: the filter drops characters on paste and lets the rest in") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10);

    one->allows([](char32_t which) { return which >= U'0' && which <= U'9'; });

    top.children().add(one);

    one->focus();

    ui::PasteEvent what{"12ab34"};

    ought(top.dispatch(what));

    // One bad character in the middle does not invalidate the whole paste.
    same(one->text(), std::string{"1234"});
}

// Without a filter the field accepts everything: the filter is an addition, not a condition for
// working.
TRIAL("Field: without a filter everything goes in") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10);

    top.children().add(one);

    one->focus();

    typed(top, "a1b2");

    same(one->text(), std::string{"a1b2"});
}

// Setting the content from the program does not ask the filter: the caller knows what it does, and
// the filtering is there to guard what the user types.
TRIAL("Field: the filter leaves content set by the program alone") {
    const auto one = std::make_shared<ui::Field>(10);

    one->allows([](char32_t which) { return which >= U'0' && which <= U'9'; });

    one->text("abc");

    same(one->text(), std::string{"abc"});
}

// A filtered-out character does not go up. Otherwise a "t" rejected in a digits-only field would
// press the dialog's button that has this letter among its hot keys.
TRIAL("Dialog: a character dropped by the filter does not press a button") {
    ui::Root top;

    int done = 0;

    const Asked asked = naming(top, &done);

    asked.field->allows([](char32_t which) { return which >= U'0' && which <= U'9'; });

    typed(top, "t");

    ought(asked.field->text().empty());
    same(done, 0);

    typed(top, "7");

    same(asked.field->text(), std::string{"7"});
}

// --- Caret ---------------------------------------------------------------------------------------
//
// The caret is not part of the sheet; it belongs to the root: the widget says where it wants it, in
// its own coordinates, and the root composes the position by walking up through the areas. The root
// also clips it, and that is exactly what the cases below check.

TRIAL("Caret: a nested field composes its position from the areas") {
    ui::Root top;

    const auto frame = std::make_shared<ui::Frame>();
    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<ui::Field>(8, "ab");
    const auto second = std::make_shared<ui::Field>(8, "abc");

    column->children().add(first);
    column->children().add(second);
    frame->children().add(column);
    top.children().add(frame);

    second->focus();

    (void)top.render({{0, 0}, {12, 5}});

    // The frame moves the content one cell in from every side, the column puts the second field in
    // the next row, and the caret stands after the third letter.
    ought(top.caret().has_value());
    same(*top.caret(), (ui::Spot{4, 2}));
}

TRIAL("Caret: a field scrolled out of the viewport reports no caret") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::both, ui::ScrollShape{false});
    const auto column = std::make_shared<ui::Column>();

    for (int at = 0; at < 10; ++at) column->children().add(std::make_shared<ui::Field>(6, "ab"));

    const auto away = std::make_shared<ui::Field>(6, "ab");

    column->children().add(away);
    scroll->children().add(column);
    top.children().add(scroll);

    away->focus();

    // The first frame after the focus changes follows it: the scroller brings the field to the
    // bottom edge and the caret stands on it. Then the scroll position moves away (as with the
    // wheel), the field keeps the focus outside the viewport and there is no caret.
    (void)top.render({{0, 0}, {8, 3}});

    ought(top.caret().has_value());
    same(*top.caret(), (ui::Spot{2, 2}));

    scroll->shift({0, 0});

    (void)top.render({{0, 0}, {8, 3}});

    ought(!top.caret().has_value());

    // Scrolling to it brings the caret back, at the edge of the viewport and not somewhere further.
    scroll->shift({0, 8});

    (void)top.render({{0, 0}, {8, 3}});

    ought(top.caret().has_value());
    same(*top.caret(), (ui::Spot{2, 2}));
}

TRIAL("Caret: a field without the focus reports none, even when visible") {
    ui::Root top;

    const auto column = std::make_shared<ui::Column>();
    const auto one = std::make_shared<ui::Field>(6, "ab");
    const auto two = std::make_shared<ui::Field>(6, "cd");

    column->children().add(one);
    column->children().add(two);
    top.children().add(column);

    two->focus();

    (void)top.render({{0, 0}, {8, 2}});

    same(*top.caret(), (ui::Spot{2, 1}));

    one->focus();

    (void)top.render({{0, 0}, {8, 2}});

    same(*top.caret(), (ui::Spot{2, 0}));
}

// The role root can be replaced, just as in a button, a menu item and a legend item.
TRIAL("Field: the role root can be replaced") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(6, "abc");

    same(one->role(), std::string{"talcite.input"});

    top.children().add(one);

    ui::Styles these;

    Style look{};

    look.foreground = Color{false, 3, 3, 3};

    these["moje.pole"] = look;

    top.skin(ui::Skin{these});
    one->role("moje.pole");

    const View made = top.render({{0, 0}, {6, 1}});

    same(made.at({0, 0}).style.foreground, (Color{false, 3, 3, 3}));
}

TRIAL("Hidden field: dots instead of characters, content unchanged") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10, "tajne");

    one->concealed(true);

    top.children().add(one);

    one->focus();

    typed(top, "!");

    same(one->text(), std::string{"tajne!"});
    same(read(top.render({{0, 0}, {10, 1}}), 0), std::string{"••••••"});
    ought(one->concealed());

    one->concealed(false);

    same(read(top.render({{0, 0}, {10, 1}}), 0), std::string{"tajne!"});
}

TRIAL("Hidden field: a wide character is one dot") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10, "日本ab");

    one->concealed(true);

    top.children().add(one);

    one->focus();

    same(read(top.render({{0, 0}, {10, 1}}), 0), std::string{"••••"});
    same(*top.caret(), (ui::Spot{4, 0}));
}

TRIAL("Hidden field: scrolls by dots") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(4, "日本日本");

    one->concealed(true);

    top.children().add(one);

    one->focus();

    same(read(top.render({{0, 0}, {4, 1}}), 0), std::string{"•••"});
    same(*top.caret(), (ui::Spot{3, 0}));
}

TRIAL("Hidden field: a click counts cells in dots") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10, "日本日本");

    one->concealed(true);

    top.children().add(one);

    (void)top.render({{0, 0}, {10, 1}});

    ui::ClickEvent what{{2, 0}, ui::Mouse::left};

    ought(top.dispatch(what));
    same(one->cursor(), static_cast<std::size_t>(2));
}

TRIAL("Hidden field: no suggestion") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10, "ka");

    one->guess([](const std::string&) { return std::string{"katalog"}; });

    same(one->guessed(), std::string{"talog"});

    one->concealed(true);

    ought(one->guessed().empty());

    top.children().add(one);

    one->focus();

    typed(top, "t");

    ought(one->guessed().empty());
    same(read(top.render({{0, 0}, {10, 1}}), 0), std::string{"•••"});

    one->concealed(false);

    same(one->guessed(), std::string{"alog"});
}
