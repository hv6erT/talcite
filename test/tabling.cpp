#include "proving.hpp"

#include <memory>
#include <string>
#include <vector>

#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/table.hpp"

using namespace talcite;

namespace {

// A root to hang the table on.
class Sheet : public ui::Root {};

// Reads row `y` of a drawn view as UTF-8 text, skipping the empty halves of wide cells and
// dropping trailing spaces.
std::string said(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter == 0) continue;

        utf8::encode(letter, made);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

// A row source: a name and a number. It returns at most as many rows as asked for, and none past
// the end, which is exactly the contract the table has to keep to.
ui::TableSource numbered(std::size_t much) {
    return [much](std::size_t which, std::size_t most) {
        std::vector<ui::TableRow> made;

        for (std::size_t one = which; one < much && made.size() < most; ++one)
            made.push_back(ui::TableRow{ui::TableCell{"plik" + std::to_string(one), {}},
                                     ui::TableCell{std::to_string(one * 10), {}}});

        return made;
    };
}

// Two columns: a name that takes the rest of the width and a right-aligned size eight cells wide.
std::vector<ui::TableColumn> twofold() {
    return {ui::TableColumn{"Nazwa", 0, ui::Justify::left},
            ui::TableColumn{"Rozmiar", 8, ui::Justify::right}};
}

// A table with the two columns and `much` numbered rows, hung on `sheet`.
std::shared_ptr<ui::Table> shown(Sheet& sheet, std::size_t much) {
    const auto one = std::make_shared<ui::Table>();

    one->columns(twofold());
    one->source(numbered(much), much);

    sheet.children().add(one);

    return one;
}

}

// --- What a table knows at the start -------------------------------------------------------------

TRIAL("Table: an empty one carries nothing") {
    const ui::Table one;

    ought(!one.sourced());
    same(one.rows(), std::size_t{0});
    ought(!one.which().has_value());
    same(one.first(), std::size_t{0});
}

TRIAL("Table: once given a source it stands on the first row") {
    Sheet sheet;

    const auto one = shown(sheet, 10);

    ought(one->sourced());
    same(one->rows(), std::size_t{10});
    same(*one->which(), std::size_t{0});
    same(one->first(), std::size_t{0});
}

TRIAL("Table: a source with no rows gives no current row") {
    Sheet sheet;

    const auto one = shown(sheet, 0);

    ought(!one->which().has_value());

    (void)sheet.render({{0, 0}, {30, 5}});

    same(one->shown(), std::size_t{0});
}

// --- Columns -------------------------------------------------------------------------------------

// A column of zero width takes the rest of the window, and one with a fixed width takes its own.
// There is one cell between them, so in a window of thirty cells the name is left with twenty-one.
TRIAL("Table: a column with zero takes the rest of the width") {
    Sheet sheet;

    const auto one = shown(sheet, 3);

    const View made = sheet.render({{0, 0}, {30, 4}});

    same(said(made, 0), std::string{"Nazwa                  Rozmiar"});
    same(said(made, 1), std::string{"plik0                        0"});
    same(said(made, 2), std::string{"plik1                       10"});
}

TRIAL("Table: right alignment pads from the left") {
    Sheet sheet;

    const auto one = std::make_shared<ui::Table>();

    one->heading(false);
    one->columns({ui::TableColumn{"", 6, ui::Justify::right}});
    one->source(
        [](std::size_t which, std::size_t most) {
            std::vector<ui::TableRow> made;

            if (which == 0 && most > 0) made.push_back(ui::TableRow{ui::TableCell{"ab", {}}});

            return made;
        },
        1);

    sheet.children().add(one);

    same(said(sheet.render({{0, 0}, {6, 1}}), 0), std::string{"    ab"});
}

// A window narrower than the sum of the columns clips from the right, since a panel does not scroll
// horizontally. With fourteen cells the name is left with exactly five, as many as "plik0" has.
TRIAL("Table: a narrower window gives the flexible column less") {
    Sheet sheet;

    const auto one = shown(sheet, 3);

    same(said(sheet.render({{0, 0}, {14, 3}}), 1), std::string{"plik0        0"});
}

// A fixed-width column that does not fit in the window by itself must not take everything: the
// name is the content in a panel, so it keeps at least one cell.
TRIAL("Table: the flexible column does not vanish in an extremely narrow window") {
    Sheet sheet;

    const auto one = shown(sheet, 3);

    const View made = sheet.render({{0, 0}, {8, 3}});

    same(made.at({0, 1}).letter, U'p');
}

TRIAL("Table: without a heading the first row stands at the top") {
    Sheet sheet;

    const auto one = shown(sheet, 3);

    one->heading(false);

    same(said(sheet.render({{0, 0}, {30, 3}}), 0), std::string{"plik0                        0"});
}

// --- Movement ------------------------------------------------------------------------------------

TRIAL("Table: a step down and up moves through the rows") {
    Sheet sheet;

    const auto one = shown(sheet, 10);

    (void)sheet.render({{0, 0}, {30, 5}});

    one->step(1);
    same(*one->which(), std::size_t{1});

    one->step(3);
    same(*one->which(), std::size_t{4});

    one->step(-2);
    same(*one->which(), std::size_t{2});

    one->step(-100);
    same(*one->which(), std::size_t{0});

    one->step(100);
    same(*one->which(), std::size_t{9});
}

// The view follows the current row, not the other way round.
TRIAL("Table: going below the screen scrolls the view") {
    Sheet sheet;

    const auto one = shown(sheet, 20);

    (void)sheet.render({{0, 0}, {30, 4}});

    same(one->shown(), std::size_t{3});

    one->step(2);
    same(one->first(), std::size_t{0});

    one->step(1);
    same(one->first(), std::size_t{1});

    (void)sheet.render({{0, 0}, {30, 4}});

    same(said(sheet.render({{0, 0}, {30, 4}}), 1), std::string{"plik1                       10"});
}

TRIAL("Table: end stands on the last row") {
    Sheet sheet;

    const auto one = shown(sheet, 20);

    (void)sheet.render({{0, 0}, {30, 4}});

    one->end();

    same(*one->which(), std::size_t{19});
    same(one->first(), std::size_t{17});
}

TRIAL("Table: a page moves by one screen") {
    Sheet sheet;

    const auto one = shown(sheet, 30);

    one->focus();

    (void)sheet.render({{0, 0}, {30, 6}});

    ui::KeyEvent down{ui::Code::pagedown};

    ought(sheet.dispatch(down));
    same(*one->which(), std::size_t{5});

    ought(sheet.dispatch(down));
    same(*one->which(), std::size_t{10});

    ui::KeyEvent up{ui::Code::pageup};

    ought(sheet.dispatch(up));
    same(*one->which(), std::size_t{5});
}

// The wheel moves only the view and leaves the current row alone, just as in a list.
TRIAL("Table: the wheel scrolls the view and leaves the row alone") {
    Sheet sheet;

    const auto one = shown(sheet, 30);

    (void)sheet.render({{0, 0}, {30, 6}});

    ui::WheelEvent what{{2, 2}, -1};

    ought(sheet.dispatch(what));

    same(one->first(), std::size_t{3});
    same(*one->which(), std::size_t{0});
}

TRIAL("Table: the wheel at the end of scrolling passes the event up") {
    Sheet sheet;

    const auto one = shown(sheet, 30);

    (void)sheet.render({{0, 0}, {30, 6}});

    ui::WheelEvent up{{2, 2}, 1};

    ought(!sheet.dispatch(up));
}

TRIAL("Table: movement notifies through an event, not a callback") {
    Sheet sheet;

    const auto one = shown(sheet, 10);

    int moves = 0;

    (void)one->on<ui::ScrollEvent>([&moves](ui::ScrollEvent&) {
        ++moves;

        return false;
    });

    (void)sheet.render({{0, 0}, {30, 5}});

    one->step(1);
    same(moves, 1);

    one->step(0);
    same(moves, 1);

    one->first(3);
    same(moves, 2);
}

// --- Clicking ------------------------------------------------------------------------------------

TRIAL("Table: a click sets the current row below the heading") {
    Sheet sheet;

    const auto one = shown(sheet, 10);

    (void)sheet.render({{0, 0}, {30, 6}});

    ui::ClickEvent what{{4, 3}, ui::Mouse::left};

    ought(sheet.dispatch(what));
    same(*one->which(), std::size_t{2});
}

// A click on the heading itself is not a click on a row.
TRIAL("Table: a click on the heading does not move the row") {
    Sheet sheet;

    const auto one = shown(sheet, 10);

    (void)sheet.render({{0, 0}, {30, 6}});

    one->step(4);

    ui::ClickEvent what{{4, 0}, ui::Mouse::left};

    ought(sheet.dispatch(what));
    same(*one->which(), std::size_t{4});
}

TRIAL("Table: a click below the last row changes nothing") {
    Sheet sheet;

    const auto one = shown(sheet, 2);

    (void)sheet.render({{0, 0}, {30, 8}});

    ui::ClickEvent what{{4, 6}, ui::Mouse::left};

    ought(sheet.dispatch(what));
    same(*one->which(), std::size_t{0});
}

// A double click on a row makes it current and goes up the widget path; what it means is up to the
// application. On the heading and below the last row the table takes it and it goes no further.
TRIAL("Table: a double click on a row goes up, on the heading and empty space it does not") {
    struct Heard : ui::Root {
        int many = 0;

        // Counts the double clicks that reach the root.
        bool handle(ui::DoubleClickEvent&) override {
            ++many;

            return true;
        }
    };

    Heard sheet;

    const auto one = std::make_shared<ui::Table>();

    one->columns(twofold());
    one->source(numbered(3), 3);

    sheet.children().add(one);

    (void)sheet.render({{0, 0}, {30, 8}});

    ui::DoubleClickEvent row{{4, 3}, ui::Mouse::left};

    ought(sheet.dispatch(row));
    same(*one->which(), std::size_t{2});
    same(sheet.many, 1);

    ui::DoubleClickEvent head{{4, 0}, ui::Mouse::left};
    ui::DoubleClickEvent below{{4, 6}, ui::Mouse::left};

    ought(sheet.dispatch(head));
    ought(sheet.dispatch(below));
    same(sheet.many, 1);
    same(*one->which(), std::size_t{2});
}

// --- Selection -----------------------------------------------------------------------------------

TRIAL("Table: selection is something other than the current row") {
    Sheet sheet;

    const auto one = shown(sheet, 5);

    ought(!one->chosen().contains(0));

    one->chosen().add(2);
    one->chosen().add(4);

    ought(one->chosen().contains(2));
    ought(one->chosen().contains(4));
    ought(!one->chosen().contains(3));
    same(*one->which(), std::size_t{0});

    const std::vector<std::size_t> want{2, 4};

    same(one->chosen().all(), want);

    one->chosen().clear();

    ought(one->chosen().all().empty());
}

TRIAL("Table: a selection out of range waits, and drawing clips it") {
    Sheet sheet;

    const auto one = shown(sheet, 3);

    one->chosen().add(9);

    // The set does not know the row count, so it remembers, and that is intended: the selection
    // survives until the table grows enough for that row to exist.
    ought(one->chosen().contains(9));

    // The table clips on the next drawing, the same lazy way the history clips its list and a page
    // drops the cursor of the walk over tags.
    (void)sheet.render({{0, 0}, {30, 5}});

    ought(!one->chosen().contains(9));
    ought(one->chosen().all().empty());
}

// The count of selected rows goes along with the bits: adding the same row twice and removing an
// unselected one do not change it, clipping on drawing removes only what it cut off, and `clear`
// sets it to zero.
TRIAL("Table: the selected count keeps up with adding, removing and clipping") {
    Sheet sheet;

    const auto one = shown(sheet, 3);

    ought(one->chosen().empty());

    one->chosen().add(1);
    one->chosen().add(1);
    one->chosen().add(2);
    one->chosen().add(7);
    one->chosen().add(9);

    same(one->chosen().many(), std::size_t{4});

    one->chosen().remove(0);
    one->chosen().remove(20);
    one->chosen().remove(2);
    one->chosen().remove(2);

    same(one->chosen().many(), std::size_t{3});

    (void)sheet.render({{0, 0}, {30, 5}});

    same(one->chosen().many(), std::size_t{1});
    ought(!one->chosen().empty());

    one->chosen().clear();

    same(one->chosen().many(), std::size_t{0});
    ought(one->chosen().empty());
}

// The table does not take Insert. Marking with a step down would be the only place in the library
// where `handle` changed data rather than just moving the cursor, and the caller would have no way
// to undo it: intercepting it higher up does not work, because the table lies closer to focus.
TRIAL("Table: Insert passes up and leaves the selection alone") {
    Sheet sheet;

    const auto one = shown(sheet, 5);

    one->focus();

    (void)sheet.render({{0, 0}, {30, 6}});

    ui::KeyEvent what{ui::Code::insert};

    ought(!sheet.dispatch(what));

    ought(!one->chosen().contains(0));
    same(*one->which(), std::size_t{0});
}

// Whoever wants that habit from file panels hangs a handler and calls `mark` themselves, and then it
// works, because the handler sees the key regardless of what `handle` does.
TRIAL("Table: marking with Insert can be added through a handler") {
    Sheet sheet;

    const auto one = shown(sheet, 5);

    one->focus();

    (void)sheet.render({{0, 0}, {30, 6}});

    const std::weak_ptr<ui::Table> weak = one;

    (void)one->on<ui::KeyEvent>([weak](ui::KeyEvent& what) {
        if (what.key().code() != ui::Code::insert) return false;

        const auto mine = weak.lock();

        if (!mine) return false;

        const std::size_t at = mine->which().value_or(0);

        if (mine->chosen().contains(at))
            mine->chosen().remove(at);
        else
            mine->chosen().add(at);

        mine->step(1);

        return true;
    });

    ui::KeyEvent what{ui::Code::insert};

    ought(sheet.dispatch(what));

    ought(one->chosen().contains(0));
    same(*one->which(), std::size_t{1});
}

// --- Roles ---------------------------------------------------------------------------------------

TRIAL("Table: the current, selected and plain rows have different roles") {
    Sheet sheet;

    const auto one = shown(sheet, 4);

    one->chosen().add(1);

    const View made = sheet.render({{0, 0}, {30, 5}});

    // The heading and the current row in reverse video, the selected row bold, the rest plain.
    ought(made.at({0, 0}).style.reverse);
    ought(made.at({0, 1}).style.reverse);
    ought(made.at({0, 2}).style.bold);
    ought(!made.at({0, 2}).style.reverse);
    ought(!made.at({0, 3}).style.bold);
}

TRIAL("Table: a cell's role is laid over the row's role") {
    Sheet sheet;

    const auto one = std::make_shared<ui::Table>();

    one->heading(false);
    one->columns({ui::TableColumn{"", 4, ui::Justify::left}, ui::TableColumn{"", 4, ui::Justify::left}});
    one->source(
        [](std::size_t which, std::size_t most) {
            std::vector<ui::TableRow> made;

            if (which == 0 && most > 0)
                made.push_back(ui::TableRow{ui::TableCell{"aa", "talcite.table.mark"}, ui::TableCell{"bb", {}}});

            return made;
        },
        1);

    sheet.children().add(one);

    const View made = sheet.render({{0, 0}, {9, 1}});

    // The row is current, so it is in reverse video; the first cell adds bold to that.
    ought(made.at({0, 0}).style.reverse);
    ought(made.at({0, 0}).style.bold);
    ought(made.at({5, 0}).style.reverse);
    ought(!made.at({5, 0}).style.bold);
}

// --- Refreshing ----------------------------------------------------------------------------------

// `refresh` throws away the remembered rows and asks the source again, but does not clear the
// selection: only the caller knows whether the row numbers still mean the same.
TRIAL("Table: refresh asks the source again and keeps the selection") {
    Sheet sheet;

    const auto said = std::make_shared<std::string>("stare");

    const auto one = std::make_shared<ui::Table>();

    one->heading(false);
    one->columns({ui::TableColumn{"", 0, ui::Justify::left}});
    one->source(
        [said](std::size_t which, std::size_t most) {
            std::vector<ui::TableRow> made;

            if (which == 0 && most > 0) made.push_back(ui::TableRow{ui::TableCell{*said, {}}});

            return made;
        },
        1);

    sheet.children().add(one);

    (void)sheet.render({{0, 0}, {10, 1}});

    one->chosen().add(0);

    *said = "nowe";

    one->refresh();

    const View made = sheet.render({{0, 0}, {10, 1}});

    ought(one->chosen().contains(0));

    std::string got;

    for (int x = 0; x < 4; ++x) utf8::encode(made.at({x, 0}).letter, got);

    same(got, std::string{"nowe"});
}

// Giving a new source starts from zero: a new document means a new selection and a new position.
TRIAL("Table: a new source resets the position and the selection") {
    Sheet sheet;

    const auto one = shown(sheet, 10);

    (void)sheet.render({{0, 0}, {30, 5}});

    one->end();
    one->chosen().add(3);

    one->source(numbered(4), 4);

    same(*one->which(), std::size_t{0});
    same(one->first(), std::size_t{0});
    ought(one->chosen().all().empty());
    same(one->rows(), std::size_t{4});
}

// --- Scrollbar -----------------------------------------------------------------------------------

TRIAL("Table: the scrollbar takes a column from the content") {
    Sheet sheet;

    const auto one = shown(sheet, 40);

    one->heading(false);

    // Without the scrollbar, a right-aligned number ends in the last column of the window.
    same(sheet.render({{0, 0}, {30, 3}}).at({29, 0}).letter, U'0');

    one->scrollbar(true);

    const View made = sheet.render({{0, 0}, {30, 3}});

    // With the scrollbar the content narrows by one cell, and the scrollbar stands in its place.
    same(made.at({28, 0}).letter, U'0');
    ought(made.at({29, 0}).letter != U' ');
}

// The scrollbar stands only when there are more rows than are visible; the column for it stays, so
// that the table's columns do not jump when the list grows.
TRIAL("Table: the scrollbar only with rows that are not visible") {
    Sheet sheet;

    const auto one = shown(sheet, 3);

    one->heading(false);
    one->scrollbar(true);

    const View fits = sheet.render({{0, 0}, {30, 3}});

    same(fits.at({29, 0}).letter, U' ');
    same(fits.at({28, 0}).letter, U'0');

    Sheet other;

    const auto more = shown(other, 4);

    more->heading(false);
    more->scrollbar(true);

    ought(other.render({{0, 0}, {30, 3}}).at({29, 0}).letter != U' ');

    // The heading takes a row: three rows under a heading no longer fit in a window of three.
    Sheet headed;

    const auto titled = shown(headed, 3);

    titled->scrollbar(true);

    ought(headed.render({{0, 0}, {30, 3}}).at({29, 0}).letter != U' ');
}

// --- What goes up --------------------------------------------------------------------------------

// The table has no notion of "a row was chosen": in a panel Enter enters a directory, and in a
// dialog it presses the default button. Letters go up too, for quick search.
TRIAL("Table: Enter and letters pass up") {
    Sheet sheet;

    const auto one = shown(sheet, 5);

    one->focus();

    ui::KeyEvent enter{ui::Code::enter};

    ought(!sheet.dispatch(enter));

    ui::KeyEvent letter{U'a'};

    ought(!sheet.dispatch(letter));

    ui::KeyEvent f2{ui::Code::f2};

    ought(!sheet.dispatch(f2));
}

TRIAL("Table: takes no children") {
    ui::Table one;

    same(one.capacity(), 0);
}

// The rest of the selection's vocabulary: the count, removing one, and emptiness. The table holds
// the set directly, and this trial checks it on the table.
TRIAL("Table: the selection counts, removes one at a time and knows it is empty") {
    Sheet sheet;

    const auto one = shown(sheet, 10);

    ought(one->chosen().empty());

    one->chosen().add(1);
    one->chosen().add(4);
    one->chosen().add(9);

    same(one->chosen().many(), std::size_t{3});
    ought(!one->chosen().empty());

    one->chosen().remove(4);

    ought(!one->chosen().contains(4));
    same(one->chosen().all(), (std::vector<std::size_t>{1, 9}));
}
