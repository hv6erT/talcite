#include "proving.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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

        made += spelled(letter);
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

// Flexible columns share the room by their weights: in twenty-six cells, one taken by the gap,
// shares of 2 and 3 give ten and fifteen. A share below one counts as one, so 0 and 1 split
// equally.
TRIAL("Table: flexible columns share the room by their weights") {
    const auto lettered = [](std::size_t which, std::size_t most) {
        std::vector<ui::TableRow> made;

        if (which == 0 && most > 0)
            made.push_back(ui::TableRow{ui::TableCell{"abcdefghijklmnopqrstuvwxyz", {}}, ui::TableCell{"ABCDEFGHIJKLMNOPQRSTUVWXYZ", {}}});

        return made;
    };

    Sheet weighed;

    const auto one = std::make_shared<ui::Table>();

    one->heading(false);
    one->columns({ui::TableColumn{"", 0, ui::Justify::left, 2}, ui::TableColumn{"", 0, ui::Justify::left, 3}});
    one->source(lettered, 1);

    weighed.children().add(one);

    same(said(weighed.render({{0, 0}, {26, 1}}), 0), std::string{"abcdefghij ABCDEFGHIJKLMNO"});

    Sheet even;

    const auto other = std::make_shared<ui::Table>();

    other->heading(false);
    other->columns({ui::TableColumn{"", 0, ui::Justify::left, 0}, ui::TableColumn{"", 0, ui::Justify::left, 1}});
    other->source(lettered, 1);

    even.children().add(other);

    same(said(even.render({{0, 0}, {21, 1}}), 0), std::string{"abcdefghij ABCDEFGHIJ"});
}

// A fixed-width column that does not fit in the window by itself must not take everything: the
// name is the content in a panel, so it keeps at least one cell.
TRIAL("Table: the flexible column does not vanish in an extremely narrow window") {
    Sheet sheet;

    const auto one = shown(sheet, 3);

    const View made = sheet.render({{0, 0}, {8, 3}});

    same(made.at({0, 1}).letter, U'p');
}

// A spanning cell takes the rest of its row, the gaps included, so a heading among the rows is not
// cut at its column; the cells after it are not drawn. A cell that does not span keeps to its
// column.
TRIAL("Table: a spanning cell takes the rest of the row") {
    Sheet sheet;

    const auto one = std::make_shared<ui::Table>();

    one->heading(false);
    one->columns({ui::TableColumn{"", 3, ui::Justify::left}, ui::TableColumn{"", 0, ui::Justify::left},
                  ui::TableColumn{"", 0, ui::Justify::left}});
    one->source(
        [](std::size_t which, std::size_t most) {
            std::vector<ui::TableRow> made;

            const ui::TableRow rows[] = {
                {ui::TableCell{}, ui::TableCell{"Uporządkowane i zatwierdzone: 12", {}, true}, ui::TableCell{"ukryte", {}}},
                {ui::TableCell{"[ ]", {}}, ui::TableCell{"Uporządkowane i zatwierdzone: 12", {}}, ui::TableCell{"x", {}}}};

            for (std::size_t at = which; at < 2 && made.size() < most; ++at) made.push_back(rows[at]);

            return made;
        },
        2);

    sheet.children().add(one);

    const View made = sheet.render({{0, 0}, {40, 2}});

    same(said(made, 0), std::string{"    Uporządkowane i zatwierdzone: 12"});
    same(said(made, 1), std::string{"[ ] Uporządkowane i z x"});
}

// A cell elides only when it asks: from the start it keeps the end of the text, a path's file name,
// behind the skin's mark; in the middle it keeps both ends; without asking it is cut at the right,
// as before. Text that fits is never touched.
TRIAL("Table: a cell elides at the end it asks for, and only then") {
    Sheet sheet;

    const auto one = std::make_shared<ui::Table>();

    one->heading(false);
    one->columns({ui::TableColumn{"", 12, ui::Justify::left}, ui::TableColumn{"", 6, ui::Justify::left}});
    one->source(
        [](std::size_t which, std::size_t most) {
            std::vector<ui::TableRow> made;

            const ui::TableRow rows[] = {
                {ui::TableCell{"Alien.1979.BluRay.en.srt", {}, false, ui::Elide::left}, ui::TableCell{"a.srt", {}, false, ui::Elide::left}},
                {ui::TableCell{"Alien.1979.BluRay.en.srt", {}, false, ui::Elide::middle}, ui::TableCell{}},
                {ui::TableCell{"Alien.1979.BluRay.en.srt", {}}, ui::TableCell{}}};

            for (std::size_t at = which; at < 3 && made.size() < most; ++at) made.push_back(rows[at]);

            return made;
        },
        3);

    sheet.children().add(one);

    const View made = sheet.render({{0, 0}, {19, 3}});

    same(said(made, 0), std::string{"…uRay.en.srt a.srt"});
    same(said(made, 1), std::string{"Alien…en.srt"});
    same(said(made, 2), std::string{"Alien.1979.B"});
}

// A spanning cell reaches the edge of the table, also past columns that together are narrower than
// the window.
TRIAL("Table: a spanning cell reaches the edge past narrow columns") {
    Sheet sheet;

    const auto one = std::make_shared<ui::Table>();

    one->heading(false);
    one->columns({ui::TableColumn{"", 2, ui::Justify::left}, ui::TableColumn{"", 4, ui::Justify::left},
                  ui::TableColumn{"", 4, ui::Justify::left}});
    one->source(
        [](std::size_t which, std::size_t most) {
            std::vector<ui::TableRow> made;

            if (which == 0 && most > 0)
                made.push_back(ui::TableRow{ui::TableCell{"ab", {}}, ui::TableCell{"a heading that is long", {}, true}});

            return made;
        },
        1);

    sheet.children().add(one);

    same(said(sheet.render({{0, 0}, {30, 1}}), 0), std::string{"ab a heading that is long"});
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

// A cursor put before the table knew its height: the first frame moves the view only as far as
// the cursor needs. Before, the move assumed one row of room, so a fresh window with the cursor on
// the second row showed the list without its first row.
TRIAL("Table: a cursor put before the first frame scrolls only as far as the frame needs") {
    {
        Sheet sheet;

        const auto one = shown(sheet, 3);

        one->which(1);

        const View made = sheet.render({{0, 0}, {30, 6}});

        same(one->first(), std::size_t{0});
        same(said(made, 1), std::string{"plik0                        0"});
    }

    {
        Sheet sheet;

        const auto one = shown(sheet, 20);

        one->which(8);

        const View made = sheet.render({{0, 0}, {30, 4}});

        same(one->first(), std::size_t{6});
        same(said(made, 3), std::string{"plik8                       80"});
    }

    // Rows that open above the cursor before the first frame: the view waits too.
    {
        Sheet sheet;

        const auto one = shown(sheet, 3);

        one->which(2);
        one->changed(0, 0, 4);

        (void)sheet.render({{0, 0}, {30, 4}});

        same(*one->which(), std::size_t{6});
        same(one->first(), std::size_t{4});
    }

    // A view asked for by name, then rows replaced before the first frame: the cursor stands above
    // the view, and the frame brings the view back to it.
    {
        Sheet sheet;

        const auto one = shown(sheet, 20);

        one->first(5);
        one->changed(10, 0, 2);

        (void)sheet.render({{0, 0}, {30, 4}});

        same(*one->which(), std::size_t{0});
        same(one->first(), std::size_t{0});
    }

    // A view asked for by name wins over one still waiting for the cursor.
    {
        Sheet sheet;

        const auto one = shown(sheet, 20);

        one->which(8);
        one->first(2);

        (void)sheet.render({{0, 0}, {30, 4}});

        same(one->first(), std::size_t{2});
    }
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

    const ui::Hook heard_scroll = one->on<ui::ScrollEvent>([&moves](ui::ScrollEvent&) {
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

// The event says how the move came: the table's own keys, wheel and click by hand, a call from the
// program not, unless the caller says it carries out a person's key itself. A panel's quick search
// ends on the first and not on the second.
TRIAL("Table: a move says whether it came by hand") {
    Sheet sheet;

    const auto one = shown(sheet, 30);

    std::vector<bool> heard;

    const ui::Hook heard_scroll = one->on<ui::ScrollEvent>([&heard](ui::ScrollEvent& what) {
        heard.push_back(what.by_hand());

        return false;
    });

    one->focus();

    (void)sheet.render({{0, 0}, {30, 6}});

    heard.clear();

    ui::KeyEvent down{ui::Code::down};
    ui::KeyEvent end{ui::Code::end};
    ui::KeyEvent home{ui::Code::home};
    ui::KeyEvent page{ui::Code::pagedown};

    ought(sheet.dispatch(down));
    ought(sheet.dispatch(page));
    ought(sheet.dispatch(end));
    ought(sheet.dispatch(home));

    ui::WheelEvent wheel{{2, 2}, -1};

    ought(sheet.dispatch(wheel));

    ui::ClickEvent click{{2, 3}, ui::Mouse::left, {}};

    ought(sheet.dispatch(click));

    same(heard, (std::vector<bool>{true, true, true, true, true, true}));

    heard.clear();

    one->which(7);
    one->first(2);
    one->step(1);
    one->page(1);
    one->end();

    same(heard, (std::vector<bool>{false, false, false, false, false}));

    heard.clear();

    one->step(-1, ui::Moved::by_hand);

    same(heard, (std::vector<bool>{true}));
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

    const ui::Hook heard_key = one->on<ui::KeyEvent>([weak](ui::KeyEvent& what) {
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

// A cell's marks lay their roles over stretches of its text, as a label's do: a meter of five
// segments takes one color on the lit ones and another on the rest, and keeps the row's reverse
// video. The marks follow their bytes through eliding and justification: a mark over what eliding
// dropped covers nothing, nor does it cover the ellipsis, and a right-justified cell moves its
// marks with its text.
TRIAL("Table: a cell's marks lay roles over stretches of its text") {
    Sheet sheet;

    Style lit;
    lit.foreground = Color{false, 200, 0, 0};

    Style dim;
    dim.foreground = Color{false, 0, 0, 200};

    sheet.skin(ui::Skin{{{"probe.lit", lit}, {"probe.dim", dim}}});

    const auto one = std::make_shared<ui::Table>();

    one->heading(false);
    one->columns({ui::TableColumn{"", 5, ui::Justify::left}, ui::TableColumn{"", 6, ui::Justify::left},
                  ui::TableColumn{"", 6, ui::Justify::right}});
    one->source(
        [](std::size_t which, std::size_t most) {
            std::vector<ui::TableRow> made;

            const std::string meter = "\u25AC\u25AC\u25AC\u25AC\u25AC";

            if (which == 0 && most > 0)
                made.push_back(ui::TableRow{
                    ui::TableCell{meter, {}, false, ui::Elide::none, {ui::Mark{0, 0, 9, "probe.lit"}, ui::Mark{0, 9, 15, "probe.dim"}}},
                    ui::TableCell{"abcdefghij", {}, false, ui::Elide::left, {ui::Mark{0, 0, 2, "probe.lit"}, ui::Mark{0, 7, 10, "probe.dim"}}},
                    ui::TableCell{"ok", {}, false, ui::Elide::none, {ui::Mark{0, 0, 2, "probe.lit"}}}});

            return made;
        },
        1);

    sheet.children().add(one);

    const View made = sheet.render({{0, 0}, {19, 1}});

    same(said(made, 0), std::string{"\u25AC\u25AC\u25AC\u25AC\u25AC \u2026fghij     ok"});

    for (int x = 0; x < 3; ++x) ought(made.at({x, 0}).style.foreground == lit.foreground);
    for (int x = 3; x < 5; ++x) ought(made.at({x, 0}).style.foreground == dim.foreground);
    for (int x = 0; x < 5; ++x) ought(made.at({x, 0}).style.reverse);

    ought(made.at({6, 0}).style.foreground != lit.foreground);
    ought(made.at({7, 0}).style.foreground != lit.foreground);
    ought(made.at({8, 0}).style.foreground != dim.foreground);
    for (int x = 9; x < 12; ++x) ought(made.at({x, 0}).style.foreground == dim.foreground);

    ought(made.at({16, 0}).style.foreground != lit.foreground);
    ought(made.at({17, 0}).style.foreground == lit.foreground);
    ought(made.at({18, 0}).style.foreground == lit.foreground);
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

// --- Rows replaced in the middle -----------------------------------------------------------------

namespace {

// A table over a list of names that the trial changes, one column, no heading, hung on `sheet`.
std::shared_ptr<ui::Table> listed(Sheet& sheet, const std::shared_ptr<std::vector<std::string>>& names) {
    const auto one = std::make_shared<ui::Table>();

    one->heading(false);
    one->columns({ui::TableColumn{"", 0, ui::Justify::left}});
    one->source(
        [names](std::size_t which, std::size_t most) {
            std::vector<ui::TableRow> made;

            for (std::size_t at = which; at < names->size() && made.size() < most; ++at)
                made.push_back(ui::TableRow{ui::TableCell{(*names)[at], {}}});

            return made;
        },
        names->size());

    sheet.children().add(one);

    return one;
}

// Names "r0" to "r<much - 1>".
std::shared_ptr<std::vector<std::string>> named_rows(std::size_t much) {
    const auto made = std::make_shared<std::vector<std::string>>();

    for (std::size_t at = 0; at < much; ++at) made->push_back("r" + std::to_string(at));

    return made;
}

}

// Rows opening below the cursor and folding above it leave the cursor on its row and the view where
// it stood; the new rows are fetched, and no event says the cursor moved, because it did not.
TRIAL("Table: rows replaced around the cursor keep its row and the view") {
    Sheet sheet;

    const auto names = named_rows(20);
    const auto one = listed(sheet, names);

    int moves = 0;

    const ui::Hook heard_scroll = one->on<ui::ScrollEvent>([&moves](ui::ScrollEvent&) {
        ++moves;

        return false;
    });

    (void)sheet.render({{0, 0}, {10, 5}});

    one->first(4);
    one->which(6);
    moves = 0;

    // Three rows open under r6.
    names->insert(names->begin() + 7, {"a", "b", "c"});
    one->changed(7, 0, 3);

    same(*one->which(), std::size_t{6});
    same(one->first(), std::size_t{4});
    same(one->rows(), std::size_t{23});

    View made = sheet.render({{0, 0}, {10, 5}});

    same(said(made, 2), std::string{"r6"});
    same(said(made, 3), std::string{"a"});

    // r2 and r3 fold away above the view: everything below moves up by two, with the cursor.
    names->erase(names->begin() + 2, names->begin() + 4);
    one->changed(2, 2, 0);

    same(*one->which(), std::size_t{4});
    same(one->first(), std::size_t{2});

    made = sheet.render({{0, 0}, {10, 5}});

    same(said(made, 2), std::string{"r6"});
    same(moves, 0);
}

// A cursor on a row that went away stands where the rows were, and that is a move; rows pushed
// below the window pull the view after the cursor.
TRIAL("Table: a cursor on replaced rows moves to their place, and stays in view") {
    Sheet sheet;

    const auto names = named_rows(10);
    const auto one = listed(sheet, names);

    int moves = 0;

    const ui::Hook heard_scroll = one->on<ui::ScrollEvent>([&moves](ui::ScrollEvent&) {
        ++moves;

        return false;
    });

    (void)sheet.render({{0, 0}, {10, 4}});

    one->which(3);
    moves = 0;

    one->changed(2, 3, 1);

    same(*one->which(), std::size_t{2});
    same(one->rows(), std::size_t{8});
    same(moves, 1);

    // Six rows open above the cursor at the top of the view: the cursor goes down with its row, and
    // the view follows it so it is still seen.
    one->which(1);
    one->first(0);
    moves = 0;

    one->changed(1, 0, 6);

    same(*one->which(), std::size_t{7});
    same(one->first(), std::size_t{4});
    same(moves, 0);

    // Everything goes: no current row. Rows that come again bring the cursor to the first.
    one->changed(0, 14, 0);

    ought(!one->which().has_value());
    same(one->rows(), std::size_t{0});

    moves = 0;

    one->changed(0, 0, 3);

    same(one->which(), std::optional<std::size_t>{0});
    same(moves, 1);
}

// The selection stays with its rows: those after the change move, those replaced go.
TRIAL("Table: the selection follows rows replaced in the middle") {
    Sheet sheet;

    const auto one = shown(sheet, 10);

    one->chosen().add(1);
    one->chosen().add(4);
    one->chosen().add(8);

    one->changed(3, 2, 5);

    ought(one->chosen().all() == std::vector<std::size_t>({1, 11}));
    same(one->chosen().many(), std::size_t{2});
    same(one->rows(), std::size_t{13});
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

    ought(headed.render({{0, 0}, {30, 3}}).at({29, 1}).letter != U' ');
}

// The heading does not scroll, so the bar runs beside the rows only: it starts under the heading,
// whose cell in the bar's column stays clear, and a click there is no click on the bar.
TRIAL("Table: the scrollbar starts under the heading") {
    Sheet sheet;

    const auto one = shown(sheet, 40);

    one->scrollbar(true);

    const View made = sheet.render({{0, 0}, {30, 5}});

    ought(made.at({29, 0}).letter == U' ' || made.at({29, 0}).letter == 0);

    for (int y = 1; y < 5; ++y) ought(made.at({29, y}).letter != U' ' && made.at({29, y}).letter != 0);

    // The thumb at the top of the bar, under the heading, the track below it: the first rows are
    // shown.
    ought(made.at({29, 1}).letter != made.at({29, 2}).letter);
    same(made.at({29, 2}).letter, made.at({29, 4}).letter);

    // A press on the heading's cell does not move the view; one on the track below the thumb does.
    ui::MouseEvent heading{{29, 0}, ui::Press::down, ui::Mouse::left};

    (void)sheet.dispatch(heading);

    same(one->first(), std::size_t{0});

    ui::MouseEvent track{{29, 4}, ui::Press::down, ui::Mouse::left};

    (void)sheet.dispatch(track);

    ought(one->first() > 0);
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

// A refresh asks the source again, and of the visible rows only those that came out different are
// painted again: a spinner in a few rows does not repaint the whole table.
TRIAL("Table: a refresh repaints only the rows that came out different") {
    Sheet sheet;

    const auto texts = std::make_shared<std::vector<std::string>>(std::vector<std::string>{"a", "b", "c"});
    const auto one = std::make_shared<ui::Table>(ui::TableShape{.heading = false, .scrollbar = false});

    one->columns({ui::TableColumn{"Nazwa", 0, ui::Justify::left}});
    one->source(
        [texts](std::size_t which, std::size_t most) {
            std::vector<ui::TableRow> made;

            for (std::size_t at = which; at < texts->size() && made.size() < most; ++at) made.push_back(ui::TableRow{ui::TableCell{(*texts)[at], {}}});

            return made;
        },
        3);

    sheet.children().add(one);

    (void)sheet.render({{0, 0}, {10, 3}});

    (*texts)[1] = "B";

    one->refresh();

    const View made = sheet.render({{0, 0}, {10, 3}});

    same(said(made, 0), std::string{"a"});
    same(said(made, 1), std::string{"B"});
    same(sheet.painted(), (std::vector<std::pair<int, int>>{{1, 2}}));

    one->refresh();

    (void)sheet.render({{0, 0}, {10, 3}});

    ought(sheet.painted().empty());
}

// The room a cell's text leaves in its column is the cell's too: it takes the cell's role over the
// row's, on either side the justification leaves free, as the letters do.
TRIAL("Table: the room a cell's text leaves takes the cell's role too") {
    Sheet sheet;

    const auto one = std::make_shared<ui::Table>();

    one->heading(false);
    one->columns({ui::TableColumn{"", 4, ui::Justify::left}, ui::TableColumn{"", 4, ui::Justify::right}, ui::TableColumn{"", 4, ui::Justify::center}});
    one->source(
        [](std::size_t which, std::size_t most) {
            std::vector<ui::TableRow> made;

            if (which == 0 && most > 0)
                made.push_back(ui::TableRow{ui::TableCell{"aa", "talcite.table.mark"}, ui::TableCell{"bb", "talcite.table.mark"}, ui::TableCell{"c", "talcite.table.mark"}});

            return made;
        },
        1);

    sheet.children().add(one);

    const View made = sheet.render({{0, 0}, {14, 1}});

    ought(made.at({2, 0}).style.bold);
    ought(made.at({3, 0}).style.bold);
    ought(made.at({5, 0}).style.bold);
    ought(made.at({6, 0}).style.bold);
    same(made.at({7, 0}).letter, U'b');
    ought(made.at({10, 0}).style.bold);
    same(made.at({11, 0}).letter, U'c');
    ought(made.at({13, 0}).style.bold);
}

// A tab in a cell is a stray character of a name, shown as its picture in one cell, and the
// justification counts it as one cell too: a cell justified to the right ends at the column's
// edge, and a mark over the letter after the tab stands on that letter.
TRIAL("Table: a tab in a cell takes one cell, also for the justification and the marks") {
    Sheet sheet;

    const auto one = std::make_shared<ui::Table>();

    one->heading(false);
    one->columns({ui::TableColumn{"", 6, ui::Justify::right}});
    one->source(
        [](std::size_t which, std::size_t most) {
            std::vector<ui::TableRow> made;

            if (which == 0 && most > 0) {
                ui::TableCell cell{"a\tb", {}};

                cell.marks.push_back(ui::Mark{0, 2, 3, "talcite.table.mark"});

                made.push_back(ui::TableRow{cell});
            }

            return made;
        },
        1);

    sheet.children().add(one);

    const View made = sheet.render({{0, 0}, {6, 1}});

    same(made.at({3, 0}).letter, U'a');
    same(made.at({4, 0}).letter, U'␉');
    same(made.at({5, 0}).letter, U'b');
    ought(made.at({5, 0}).style.bold);
    ought(!made.at({3, 0}).style.bold);
}
