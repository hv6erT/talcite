#include "proving.hpp"

#include <memory>
#include <string>
#include <vector>

#include "talcite/encoding.hpp"
#include "talcite/grid.hpp"
#include "talcite/root.hpp"
#include "talcite/span.hpp"

using namespace talcite;

// A grid: cells in rows and columns, every column as wide as its widest cell; a cell `Across` takes
// the rest of its row; any other child takes a row of its own over the whole width.

namespace {

class Box : public ui::Node {
public:
    Box(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    // The size set in the constructor, whatever room is offered.
    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

protected:
    // Fills the whole given size with the box's letter, shifted by the visible part.
    void paint(Canvas& onto) const override {
        const ui::Size given = onto.size();

        onto.fill({{0, 0}, given}, Style{});

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                onto.put({x, y}, what_, Style{});
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

// A shared box that asks for `wants` and is filled with `what`.
std::shared_ptr<Box> box(ui::Size wants, char32_t what) { return std::make_shared<Box>(wants, what); }

// A row of the given cells.
std::shared_ptr<ui::GridRow> row(const std::vector<std::shared_ptr<ui::Node>>& cells) {
    auto made = std::make_shared<ui::GridRow>();

    for (const std::shared_ptr<ui::Node>& one : cells) made->children().add(one);

    return made;
}

// A cell over the rest of its row, holding `inside`.
std::shared_ptr<ui::Across> across(std::shared_ptr<ui::Node> inside) {
    auto made = std::make_shared<ui::Across>();

    made->children().add(std::move(inside));

    return made;
}

}

// Labels of different lengths in one column, values from one column on: the column is as wide as
// the longest label, and every value starts a gap after it.
TRIAL("Grid: a column is as wide as its widest cell") {
    ui::Grid one{2};

    one.children().add(row({std::make_shared<ui::Span>("Pattern:"), std::make_shared<ui::Span>("*.mkv")}));
    one.children().add(row({std::make_shared<ui::Span>("Age rating:"), std::make_shared<ui::Span>("any")}));

    same(one.wants({40, 10}), (ui::Size{18, 2}));

    const View made = one.render({{0, 0}, {20, 2}});

    same(read(made, 0), std::string{"Pattern:     *.mkv"});
    same(read(made, 1), std::string{"Age rating:  any"});
}

// A column of empty cells takes no room and opens no gap, as a child that occupies nothing in a
// row.
TRIAL("Grid: an empty column opens no gap") {
    ui::Grid one{1};

    one.children().add(row({box({1, 1}, U'a'), std::make_shared<ui::Blank>(), box({1, 1}, U'b')}));

    same(read(one.render({{0, 0}, {10, 1}}), 0), std::string{"a b"});
}

// A cell gets the width it wants, at most its column's, and the height of its row, which is as
// tall as its tallest cell.
TRIAL("Grid: a row is as tall as its tallest cell, a cell as wide as it wants") {
    ui::Grid one{1};

    const auto tall = box({2, 2}, U'a');
    const auto low = box({1, 1}, U'b');
    const auto wide = box({5, 1}, U'c');

    one.children().add(row({tall, low}));
    one.children().add(row({wide, std::make_shared<ui::Blank>()}));

    same(one.wants({40, 10}), (ui::Size{7, 3}));

    const View made = one.render({{0, 0}, {10, 3}});

    same(read(made, 0), std::string{"aa    b"});
    same(read(made, 1), std::string{"aa    b"});
    same(read(made, 2), std::string{"ccccc"});
    same(tall->area().size, (ui::Size{2, 2}));
    same(low->area().at.x, 6);
    same(wide->area().at.y, 0);
}

// A cell across takes everything from its column to the end of the last one. One that wants more
// than that widens the last column, not the one it starts in, so the other rows keep their lines.
TRIAL("Grid: a cell across takes the rest of its row, and a wider one widens the last column") {
    ui::Grid one{1};

    const auto field = box({4, 1}, U'f');

    one.children().add(row({box({2, 1}, U'l'), box({3, 1}, U'a'), box({3, 1}, U'b')}));
    one.children().add(row({std::make_shared<ui::Blank>(), across(field)}));

    same(read(one.render({{0, 0}, {20, 2}}), 1), std::string{"   fffffff"});
    same(field->area().size.wide, 7);

    const auto longer = box({10, 1}, U'h');

    one.children().add(row({std::make_shared<ui::Blank>(), across(longer)}));

    same(one.wants({40, 10}).wide, 13);

    const View made = one.render({{0, 0}, {20, 3}});

    same(read(made, 0), std::string{"ll aaa bbb"});
    same(read(made, 2), std::string{"   hhhhhhhhhh"});
}

// A child that is not a row of cells is a row of its own over the whole width: a line across, an
// empty row between parts.
TRIAL("Grid: any other child takes a row of its own over the whole width") {
    ui::Grid one{1};

    one.children().add(row({box({2, 1}, U'l'), box({3, 1}, U'v')}));
    one.children().add(std::make_shared<ui::Blank>(ui::Size{0, 1}));
    one.children().add(box({9, 2}, U'-'));

    same(one.wants({40, 10}), (ui::Size{9, 4}));

    const View made = one.render({{0, 0}, {10, 4}});

    same(read(made, 0), std::string{"ll vvv"});
    same(read(made, 1), std::string{""});
    same(read(made, 2), std::string{"----------"});
    same(read(made, 3), std::string{"----------"});
}

// The grid gives its rows their rectangles and the cells theirs inside them, so a click goes down
// through the row to the cell, the way it goes through every node.
TRIAL("Grid: a click reaches the cell through its row") {
    ui::Root top;

    const auto grid = std::make_shared<ui::Grid>(1);
    const auto label = box({3, 1}, U'l');
    const auto value = box({2, 1}, U'v');

    grid->children().add(row({label, value}));
    top.children().add(grid);

    (void)top.render({{0, 0}, {10, 2}});

    int hits = 0;

    const ui::Hook heard = value->on<ui::ClickEvent>([&hits](ui::ClickEvent&) {
        ++hits;

        return true;
    });

    ui::ClickEvent on{{4, 0}, ui::Mouse::left, {}};

    (void)top.dispatch(on);
    same(hits, 1);

    ui::ClickEvent beside{{1, 0}, ui::Mouse::left, {}};

    (void)top.dispatch(beside);
    same(hits, 1);
}

// An empty place: its size within the room, and nothing inside.
TRIAL("Blank: wants its size within the room and holds nothing") {
    const ui::Blank one{{4, 2}};

    same(one.wants({10, 10}), (ui::Size{4, 2}));
    same(one.wants({3, 1}), (ui::Size{3, 1}));
    same(one.capacity(), 0);
    const ui::Blank none{{-2, -1}};

    same(none.wants({10, 10}), (ui::Size{0, 0}));
}
