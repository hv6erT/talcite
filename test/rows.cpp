#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/row.hpp"
#include "talcite/span.hpp"

using namespace talcite;

namespace {

class Box : public ui::Node {
public:
    Box(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    // The size set in the constructor or by `wanting`, whatever room is offered.
    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

    // Changes the size the box asks for.
    void wanting(ui::Size given) { wants_ = given; }

protected:
    // Fills the whole given size with the box's letter, shifted by the visible part.
    View paint(ui::Size given, ui::Area seen) override {
        View made{seen.size};

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                made.put({x - seen.at.x, y - seen.at.y}, what_, Style{});

        return made;
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

        if (letter != 0) talcite::utf8::encode(letter, made);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

// A shared box that asks for `wants` and is filled with `what`.
std::shared_ptr<Box> box(ui::Size wants, char32_t what) {
    return std::make_shared<Box>(wants, what);
}

}

TRIAL("Children: a plain node takes one and throws on the second") {
    ui::Node one;

    same(one.capacity(), 1);

    one.children().add(box({2, 1}, U'a'));

    threw(one.children().add(box({2, 1}, U'b')));
}

TRIAL("Children: a span takes none") {
    ui::Span one{"kot"};

    same(one.capacity(), 0);

    threw(one.children().add(box({2, 1}, U'a')));
}

TRIAL("Children: the root is a wrapper, not a layout") {
    ui::Root top;

    same(top.capacity(), 1);

    top.children().add(box({2, 1}, U'a'));

    threw(top.children().add(box({2, 1}, U'b')));
}

TRIAL("Children: a row and a column take as many as needed") {
    ui::Row row;
    ui::Column column;

    same(row.capacity(), ui::plenty);
    same(column.capacity(), ui::plenty);

    for (int which = 0; which < 5; ++which) {
        row.children().add(box({1, 1}, U'a'));
        column.children().add(box({1, 1}, U'a'));
    }

    same(row.children().many(), std::size_t{5});
    same(column.children().many(), std::size_t{5});
}

TRIAL("Node: the only child gets the whole rectangle") {
    ui::Node one;

    const auto inner = box({2, 1}, U'a');

    one.children().add(inner);

    (void)one.render({{0, 0}, {6, 3}});

    same(inner->area().size.wide, 6);
    same(inner->area().size.high, 3);
}

TRIAL("Node: wants as much as its only child") {
    ui::Node one;

    one.children().add(box({4, 2}, U'a'));

    const ui::Size wants = one.wants({80, 25});

    same(wants.wide, 4);
    same(wants.high, 2);
}

TRIAL("Node without children wants nothing") {
    const ui::Node one;

    same(one.wants({80, 25}).wide, 0);
    same(one.wants({80, 25}).high, 0);
}

TRIAL("Column: children stand one below the other") {
    ui::Column one;

    const auto first = box({6, 1}, U'a');
    const auto second = box({6, 2}, U'b');

    one.children().add(first);
    one.children().add(second);

    const View made = one.render({{0, 0}, {6, 4}});

    same(first->area().at.y, 0);
    same(second->area().at.y, 1);
    same(read(made, 0), std::string{"aaaaaa"});
    same(read(made, 1), std::string{"bbbbbb"});
    same(read(made, 2), std::string{"bbbbbb"});
    same(read(made, 3), std::string{""});
}

TRIAL("Row: children stand side by side") {
    ui::Row one;

    const auto first = box({2, 1}, U'a');
    const auto second = box({3, 1}, U'b');

    one.children().add(first);
    one.children().add(second);

    const View made = one.render({{0, 0}, {6, 1}});

    same(first->area().at.x, 0);
    same(second->area().at.x, 2);
    same(read(made, 0), std::string{"aabbb"});
}

// A wrapping row: a child that does not fit in the rest of the line starts the next one, and each
// line is centered. A child of zero width opens neither a gap nor a line; one wider than the whole
// row stands alone, clipped.
TRIAL("Row: wrapping carries over what does not fit and centers the lines") {
    ui::Row one{ui::RowShape{1, true}};

    const auto first = box({5, 1}, U'a');
    const auto empty = box({0, 1}, U'x');
    const auto second = box({5, 1}, U'b');
    const auto third = box({5, 1}, U'c');
    const auto wide = box({20, 1}, U'd');

    one.children().add(first);
    one.children().add(empty);
    one.children().add(second);
    one.children().add(third);
    one.children().add(wide);

    ought(one.wrapping());
    same(one.wants({12, 10}), (ui::Size{12, 3}));
    same(one.wants({12, 2}), (ui::Size{12, 2}));

    const View made = one.render({{0, 0}, {12, 3}});

    same(read(made, 0), std::string{"aaaaa bbbbb"});
    same(read(made, 1), std::string{"   ccccc"});
    same(read(made, 2), std::string{"dddddddddddd"});
    same(third->area().at.y, 1);
    same(empty->area().size.wide, 0);
    same(wide->area().size.wide, 12);

    // The gap counts when wrapping: two fives in ten columns make two lines, not one.
    same(one.wants({10, 10}), (ui::Size{10, 4}));

    // A child that stopped taking any room gets a zero rectangle, not the old one.
    first->wanting({0, 1});

    (void)one.render({{0, 0}, {12, 3}});

    same(first->area().size.wide, 0);

    first->wanting({5, 1});

    // Enough room: one line, as without wrapping.
    same(one.wants({60, 10}), (ui::Size{38, 1}));

    one.wrapping(false);

    same(one.wants({12, 10}).high, 1);
}

TRIAL("Row: a child gets the full height") {
    ui::Row one;

    const auto first = box({2, 1}, U'a');

    one.children().add(first);

    (void)one.render({{0, 0}, {6, 3}});

    same(first->area().size.high, 3);
}

TRIAL("Column: a child gets the full width") {
    ui::Column one;

    const auto first = box({2, 1}, U'a');

    one.children().add(first);

    (void)one.render({{0, 0}, {6, 3}});

    same(first->area().size.wide, 6);
}

TRIAL("Column: wants the sum of heights, but no more than it got") {
    ui::Column one;

    one.children().add(box({4, 3}, U'a'));
    one.children().add(box({4, 3}, U'b'));

    same(one.wants({10, 10}).high, 6);
    same(one.wants({10, 4}).high, 4);
    same(one.wants({2, 10}).wide, 2);
}

TRIAL("Row: wants the sum of widths, but no more than it got") {
    ui::Row one;

    one.children().add(box({4, 1}, U'a'));
    one.children().add(box({4, 1}, U'b'));

    same(one.wants({10, 10}).wide, 8);
    same(one.wants({5, 10}).wide, 5);
    same(one.wants({10, 1}).high, 1);
}

TRIAL("Row: a child past the right edge gets zero width") {
    ui::Row one;

    const auto first = box({4, 1}, U'a');
    const auto second = box({4, 1}, U'b');

    one.children().add(first);
    one.children().add(second);

    (void)one.render({{0, 0}, {4, 1}});

    same(first->area().size.wide, 4);
    same(second->area().size.wide, 0);
    same(second->area().at.x, 4);
}

TRIAL("Row in a column is laid out as expected") {
    ui::Column outer;

    const auto row = std::make_shared<ui::Row>();

    row->children().add(std::make_shared<ui::Span>("ala"));
    row->children().add(std::make_shared<ui::Span>("kot"));

    outer.children().add(row);
    outer.children().add(std::make_shared<ui::Span>("pies"));

    const View made = outer.render({{0, 0}, {8, 2}});

    same(read(made, 0), std::string{"alakot"});
    same(read(made, 1), std::string{"pies"});
}
