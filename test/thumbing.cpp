#include "proving.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/column.hpp"
#include "talcite/editor.hpp"
#include "talcite/encoding.hpp"
#include "talcite/following.hpp"
#include "talcite/list.hpp"
#include "talcite/listitem.hpp"
#include "talcite/loop.hpp"
#include "talcite/mark.hpp"
#include "talcite/root.hpp"
#include "talcite/scroll.hpp"
#include "talcite/table.hpp"
#include "talcite/viewer.hpp"

using namespace talcite;

// The mouse on a scrollbar: the track before and after the thumb moves by a page, and the thumb can
// be dragged, in a table, a page, a list and a scroller, with one mechanism; a row at a time is the
// wheel's.
// A click on the scrollbar does not set a table row and is not a double click on a row.
namespace {

// A root that counts the double clicks arriving along the widget path, the same path along which
// the twa panel turns them into Enter.
class Sheet : public ui::Root {
public:
    int doubled = 0;

protected:
    // Counts the double click and consumes it.
    bool handle(ui::DoubleClickEvent&) override {
        ++doubled;

        return true;
    }
};

// Reads row `y` of a drawn view as UTF-8 text, skipping the empty halves of wide cells.
std::string said_at(const Snapshot& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter == 0) continue;

        made += spelled(letter);
    }

    return made;
}

// Sends one mouse event of the given kind at `at` to the root.
void pressed(ui::Root& top, ui::Spot at, ui::Press how, ui::Mouse button = ui::Mouse::left) {
    ui::MouseEvent what{at, how, button};

    (void)top.dispatch(what);
}

// A left click at `at`: a press followed by a release.
void clicked(ui::Root& top, ui::Spot at) {
    pressed(top, at, ui::Press::down);
    pressed(top, at, ui::Press::up);
}

// A one-column row source of `much` file names; it returns at most `most` rows and none past the
// end.
ui::TableSource numbered(std::size_t much) {
    return [much](std::size_t which, std::size_t most) {
        std::vector<ui::TableRow> made;

        for (std::size_t one = which; one < much && made.size() < most; ++one)
            made.push_back(ui::TableRow{ui::TableCell{"plik" + std::to_string(one), {}}});

        return made;
    };
}

// A table of a hundred rows in a 20x11 window: a heading and ten rows, the scrollbar in column 19
// beside the rows, from row 1 to 10, under the heading; its thumb one cell long.
std::shared_ptr<ui::Table> table(Sheet& top) {
    const auto one = std::make_shared<ui::Table>(ui::TableShape{.heading = true, .scrollbar = true});

    one->columns({ui::TableColumn{"Nazwa", 0, ui::Justify::left}, ui::TableColumn{"Wiek", 4, ui::Justify::right}});
    one->source(numbered(100), 100);

    top.children().add(one);

    (void)top.render({{0, 0}, {20, 11}});

    return one;
}

// The scrollbar row where the thumb stands, found by the glyph the default skin draws it with; -1
// if there is none between `from` and `to`.
int thumb_row(Sheet& top, int x, int from, int to) {
    const Snapshot shown = top.render({{0, 0}, {20, 11}});
    const char32_t track = shown.at({x, to - 1}).letter;

    for (int y = from; y < to; ++y)
        if (shown.at({x, y}).letter != track) return y;

    return -1;
}

// A text of `much` numbered lines.
std::string lines(int much) {
    std::string made;

    for (int one = 0; one < much; ++one) made += "wiersz " + std::to_string(one) + "\n";

    return made;
}

}

TRIAL("Scrollbar: a table's track scrolls by a page and stops at the end, and leaves the current row alone") {
    Sheet top;

    const auto one = table(top);

    one->which(0);

    // The thumb stands at the top of the track, so the track below it is a page down.
    clicked(top, {19, 9});

    same(one->first(), std::size_t{10});
    same(*one->which(), std::size_t{0});

    // The last cell of the track stays below the thumb until the end.
    for (int round = 0; round < 20; ++round) {
        (void)top.render({{0, 0}, {20, 11}});
        clicked(top, {19, 10});
    }

    same(one->first(), std::size_t{90});

    (void)top.render({{0, 0}, {20, 11}});
    clicked(top, {19, 1});

    same(one->first(), std::size_t{80});

    // A page past the end stops at the end, not beyond it.
    one->first(85);

    (void)top.render({{0, 0}, {20, 11}});
    clicked(top, {19, 1});

    same(one->first(), std::size_t{75});

    // In a taller window (twenty rows, a track of twenty cells under the heading, a thumb of four)
    // the thumb at 75 stands in rows 16–19, so there is track below it: a page down from 75 is 80,
    // not 95.
    one->first(75);

    (void)top.render({{0, 0}, {20, 21}});
    clicked(top, {19, 20});

    same(one->first(), std::size_t{80});
}

TRIAL("Scrollbar: a table's thumb can be dragged, also outside the scrollbar") {
    Sheet top;

    const auto one = table(top);

    const int y = thumb_row(top, 19, 1, 11);

    ought(y == 1);

    pressed(top, {19, y}, ui::Press::down);
    pressed(top, {19, 10}, ui::Press::move);

    same(one->first(), std::size_t{90});

    // One cell of the track is ten rows: the fourth cell, in row 4, gives 30.
    pressed(top, {19, 4}, ui::Press::move);

    same(one->first(), std::size_t{30});

    // A move sideways, off the scrollbar: holding the button still leads it to the table.
    pressed(top, {3, 5}, ui::Press::move);

    ought(one->first() > 0 && one->first() < 90);

    pressed(top, {40, 0}, ui::Press::move);

    same(one->first(), std::size_t{0});

    pressed(top, {40, 0}, ui::Press::up);

    // After release a move drags nothing.
    pressed(top, {19, 9}, ui::Press::move);

    same(one->first(), std::size_t{0});
}

// A thumb several cells long keeps to the cursor by the cell it was grabbed at: a move by one cell
// shifts the view by one cell of the track, rather than jumping the start of the thumb under the
// cursor.
TRIAL("Scrollbar: a thumb grabbed in the middle follows the cursor by the same cell") {
    Sheet top;

    const auto one = std::make_shared<ui::Table>(ui::TableShape{.heading = true, .scrollbar = true});

    one->columns({ui::TableColumn{"Nazwa", 0, ui::Justify::left}});
    one->source(numbered(20), 20);

    top.children().add(one);

    (void)top.render({{0, 0}, {20, 11}});

    // A track of 11 cells, a window of 10 out of 20: the thumb is five cells, from row 0 to 4. The
    // grab is in row 3.
    pressed(top, {19, 3}, ui::Press::down);
    pressed(top, {19, 3}, ui::Press::move);

    same(one->first(), std::size_t{0});

    pressed(top, {19, 4}, ui::Press::move);

    same(one->first(), std::size_t{2});

    pressed(top, {19, 4}, ui::Press::up);
}

// A view between two cells of the track (after the wheel or a key) stays where it is when the
// thumb is grabbed and moved within the cell it stands in; a move by a cell moves the view by a
// cell of the track from there, and the ends of the track are the ends of the whole.
TRIAL("Scrollbar: grabbing the thumb does not move the view, moving it moves the view from where it was") {
    Sheet top;

    const auto one = table(top);

    // A track of ten cells and a thumb of one: a cell of the track is ten rows. At 5 the thumb
    // stands in the second cell, row 2.
    one->first(5);

    const int y = thumb_row(top, 19, 1, 11);

    ought(y == 2);

    pressed(top, {19, y}, ui::Press::down);
    pressed(top, {19, y}, ui::Press::move);

    same(one->first(), std::size_t{5});

    pressed(top, {19, y + 1}, ui::Press::move);

    same(one->first(), std::size_t{15});

    pressed(top, {19, y}, ui::Press::move);

    same(one->first(), std::size_t{5});

    pressed(top, {19, 1}, ui::Press::move);

    same(one->first(), std::size_t{0});

    pressed(top, {19, 10}, ui::Press::move);

    same(one->first(), std::size_t{90});

    pressed(top, {19, 10}, ui::Press::up);

    // At 3 the thumb stands in the first cell of the track, which is also the start of the whole:
    // a grab there keeps 3 all the same.
    one->first(3);

    ought(thumb_row(top, 19, 1, 11) == 1);

    pressed(top, {19, 1}, ui::Press::down);
    pressed(top, {19, 1}, ui::Press::move);

    same(one->first(), std::size_t{3});

    pressed(top, {19, 1}, ui::Press::up);
}

TRIAL("Scrollbar: a click on a table's scrollbar does not set a row and is not a double click on a row") {
    Sheet top;

    const auto one = table(top);

    one->which(3);

    clicked(top, {19, 5});
    clicked(top, {19, 5});

    same(*one->which(), std::size_t{3});
    same(top.doubled, 0);

    // A click beside the scrollbar sets the row as always.
    (void)top.render({{0, 0}, {20, 11}});
    clicked(top, {2, 2});

    same(*one->which(), one->first() + 1);
}

TRIAL("Scrollbar: the right button and a table without a scrollbar scroll nothing") {
    Sheet top;

    const auto one = table(top);

    pressed(top, {19, 10}, ui::Press::down, ui::Mouse::right);

    same(one->first(), std::size_t{0});

    one->scrollbar(false);

    (void)top.render({{0, 0}, {20, 11}});

    clicked(top, {19, 10});

    same(one->first(), std::size_t{0});
}

TRIAL("Table: column_under tells which heading column a cell stands under") {
    Sheet top;

    const auto one = table(top);

    // A width of 19 without the scrollbar: Wiek 4, a gap of 1, Nazwa 14.
    same(one->column_under({0, 0}).value_or(9), std::size_t{0});
    same(one->column_under({13, 0}).value_or(9), std::size_t{0});
    same(one->column_under({14, 0}).value_or(9), std::size_t{0});
    same(one->column_under({15, 0}).value_or(9), std::size_t{1});
    same(one->column_under({18, 0}).value_or(9), std::size_t{1});
    ought(!one->column_under({19, 0}).has_value());
    ought(!one->column_under({2, 1}).has_value());
}

TRIAL("Scrollbar: a text page, with track and thumb") {
    Sheet top;

    const auto one = std::make_shared<ui::Viewer>(ui::ScrollShape{.scrollbar = true});

    one->hold(bytes(lines(100)));

    top.children().add(one);

    (void)top.render({{0, 0}, {20, 10}});

    clicked(top, {19, 8});

    same(one->first().value_or(99), std::size_t{10});

    (void)top.render({{0, 0}, {20, 10}});
    clicked(top, {19, 0});

    same(one->first().value_or(99), std::size_t{0});

    // The thumb dragged to the very bottom of the track: the end of the text.
    const Snapshot shown = top.render({{0, 0}, {20, 10}});
    const char32_t track = shown.at({19, 9}).letter;

    int y = -1;

    for (int row = 0; row < 9 && y < 0; ++row)
        if (shown.at({19, row}).letter != track) y = row;

    ought(y >= 0);

    pressed(top, {19, y}, ui::Press::down);
    pressed(top, {19, 30}, ui::Press::move);
    pressed(top, {19, 30}, ui::Press::up);

    same(one->first().value_or(0), std::size_t{90});
}

// The editor's scrollbar is the page's: the track scrolls a page and the thumb drags, and neither
// the press nor the click and double click that follow it move the cursor or select. A click beside
// the bar still puts the cursor.
TRIAL("Scrollbar: in an editor the track and thumb move the view, not the cursor") {
    Sheet top;

    const auto one = std::make_shared<ui::Editor>(ui::ScrollShape{.scrollbar = true});

    one->hold(bytes(lines(100)));

    top.children().add(one);

    (void)top.render({{0, 0}, {20, 10}});

    clicked(top, {19, 8});

    same(one->first().value_or(99), std::size_t{10});
    same(one->cursor(), std::size_t{0});

    (void)top.render({{0, 0}, {20, 10}});
    clicked(top, {19, 0});
    clicked(top, {19, 0});

    same(one->first().value_or(99), std::size_t{0});
    same(one->cursor(), std::size_t{0});
    ought(one->selection().empty());

    const Snapshot shown = top.render({{0, 0}, {20, 10}});
    const char32_t track = shown.at({19, 9}).letter;

    int y = -1;

    for (int row = 0; row < 9 && y < 0; ++row)
        if (shown.at({19, row}).letter != track) y = row;

    ought(y >= 0);

    pressed(top, {19, y}, ui::Press::down);
    pressed(top, {19, 30}, ui::Press::move);
    pressed(top, {19, 30}, ui::Press::up);

    same(one->first().value_or(0), std::size_t{90});
    same(one->cursor(), std::size_t{0});

    (void)top.render({{0, 0}, {20, 10}});
    clicked(top, {2, 1});

    ought(one->cursor() > 0);
}

// A click on a page's scrollbar is the bar's even where a mark with an address runs under it: the
// page follows no link there. A click on the text of the mark still follows it.
TRIAL("Scrollbar: a click on a page's scrollbar follows no link beside it") {
    Loop loop;
    Sheet top;

    const auto one = std::make_shared<ui::Viewer>(ui::ScrollShape{.scrollbar = true});

    one->hold(bytes(lines(100)));
    one->marks().add(ui::Mark{0, 0, 9, "talcite.link", "https://example.org/"});

    top.children().add(one);

    int followed = 0;

    const ui::Hook heard = top.on<ui::FollowEvent>([&followed](ui::FollowEvent&) {
        ++followed;

        return true;
    });

    (void)top.render({{0, 0}, {20, 10}});

    clicked(top, {19, 0});

    for (int at = 0; at < 5; ++at) (void)loop.turn();

    same(followed, 0);

    (void)top.render({{0, 0}, {20, 10}});
    clicked(top, {2, 0});

    for (int at = 0; at < 5; ++at) (void)loop.turn();

    same(followed, 1);
}

TRIAL("Scrollbar: in a list the track and thumb move the view, not the choice") {
    Sheet top;

    const auto one = std::make_shared<ui::List>(ui::plenty, ui::plenty, ui::ScrollShape{.scrollbar = true});

    for (int at = 0; at < 50; ++at) one->children().add(std::make_shared<ui::ListItem>("pozycja " + std::to_string(at)));

    top.children().add(one);

    const Snapshot first = top.render({{0, 0}, {20, 10}});

    ought(said_at(first, 0).starts_with("pozycja 0"));

    // A track of ten cells, a window of 10 out of 50: the thumb is two cells, at the top.
    clicked(top, {19, 9});

    ought(said_at(top.render({{0, 0}, {20, 10}}), 0).starts_with("pozycja 10"));

    // At 10 the thumb stands in cells 2–3.
    pressed(top, {19, 2}, ui::Press::down);
    pressed(top, {19, 40}, ui::Press::move);
    pressed(top, {19, 40}, ui::Press::up);

    ought(said_at(top.render({{0, 0}, {20, 10}}), 0).starts_with("pozycja 40"));

    // The track above the thumb is a page up.
    clicked(top, {19, 1});

    ought(said_at(top.render({{0, 0}, {20, 10}}), 0).starts_with("pozycja 30"));

    // The thumb at 30 stands in cells 6–7 of the track; the track below it is a page down, to the
    // end.
    clicked(top, {19, 9});

    ought(said_at(top.render({{0, 0}, {20, 10}}), 0).starts_with("pozycja 40"));

    int moved = 0;

    const ui::Hook heard_scroll = one->on<ui::ScrollEvent>([&moved](ui::ScrollEvent&) {
        ++moved;

        return false;
    });

    // At the end the thumb fills the last cells; a click on it moves nothing and does not notify.
    clicked(top, {19, 9});

    same(moved, 0);
    ought(said_at(top.render({{0, 0}, {20, 10}}), 0).starts_with("pozycja 40"));
}

TRIAL("Scrollbar: a scroller in both axes, each scrollbar with its own axis") {
    Sheet top;

    const auto one = std::make_shared<ui::Scroll>(ui::Axis::both, ui::ScrollShape{.scrollbar = true});
    const auto column = std::make_shared<ui::Column>();

    for (int at = 0; at < 40; ++at)
        column->children().add(std::make_shared<ui::ListItem>("bardzo długi wiersz numer " + std::to_string(at)));

    one->children().add(column);
    top.children().add(one);

    (void)top.render({{0, 0}, {20, 10}});

    // Vertical: column 19, rows 0–8, the thumb two cells at the top; horizontal: row 9, columns
    // 0–18, the thumb twelve cells at the left. The track past a thumb is a page: nine rows down,
    // nineteen columns right, which is as far as the widest row goes, nine.
    clicked(top, {19, 8});

    same(one->shift().y, 9);
    same(one->shift().x, 0);

    (void)top.render({{0, 0}, {20, 10}});
    clicked(top, {18, 9});

    same(one->shift().x, 9);
    same(one->shift().y, 9);

    (void)top.render({{0, 0}, {20, 10}});
    clicked(top, {0, 9});

    // A row scrolled under the horizontal scrollbar does not catch the click: the scrollbar gets it,
    // and the vertical view stays.
    same(one->shift().x, 0);
    same(one->shift().y, 9);

    (void)top.render({{0, 0}, {20, 10}});
    clicked(top, {19, 7});

    same(one->shift().y, 18);

    // A vertical thumb dragged across the horizontal scrollbar is still vertical: the move goes to
    // the grab that holds it, not to the scrollbar under the cursor.
    one->shift({0, 0});

    (void)top.render({{0, 0}, {20, 10}});

    pressed(top, {19, 1}, ui::Press::down);
    pressed(top, {5, 9}, ui::Press::move);

    same(one->shift().y, 31);
    same(one->shift().x, 0);

    pressed(top, {5, 9}, ui::Press::up);
}
