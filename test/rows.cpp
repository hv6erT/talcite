#include "tal/testing.hpp"

#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/encoding.hpp"
#include "talcite/flow.hpp"
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
std::string read(const Snapshot& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter != 0) made += talcite::spelled(letter);
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

    SAME(one.capacity(), 1);

    one.children().add(box({2, 1}, U'a'));

    THREW(one.children().add(box({2, 1}, U'b')));
}

TRIAL("Children: a span takes none") {
    ui::Span one{"kot"};

    SAME(one.capacity(), 0);

    THREW(one.children().add(box({2, 1}, U'a')));
}

TRIAL("Children: the root is a wrapper, not a layout") {
    ui::Root top;

    SAME(top.capacity(), 1);

    top.children().add(box({2, 1}, U'a'));

    THREW(top.children().add(box({2, 1}, U'b')));
}

TRIAL("Children: a row and a column take as many as needed") {
    ui::Row row;
    ui::Column column;

    SAME(row.capacity(), ui::plenty);
    SAME(column.capacity(), ui::plenty);

    for (int which = 0; which < 5; ++which) {
        row.children().add(box({1, 1}, U'a'));
        column.children().add(box({1, 1}, U'a'));
    }

    SAME(row.children().many(), std::size_t{5});
    SAME(column.children().many(), std::size_t{5});
}

TRIAL("Node: the only child gets the whole rectangle") {
    ui::Node one;

    const auto inner = box({2, 1}, U'a');

    one.children().add(inner);

    (void)one.render({{0, 0}, {6, 3}});

    SAME(inner->area().size.wide, 6);
    SAME(inner->area().size.high, 3);
}

TRIAL("Node: wants as much as its only child") {
    ui::Node one;

    one.children().add(box({4, 2}, U'a'));

    const ui::Size wants = one.wants({80, 25});

    SAME(wants.wide, 4);
    SAME(wants.high, 2);
}

TRIAL("Node without children wants nothing") {
    const ui::Node one;

    SAME(one.wants({80, 25}).wide, 0);
    SAME(one.wants({80, 25}).high, 0);
}

TRIAL("Column: children stand one below the other") {
    ui::Column one;

    const auto first = box({6, 1}, U'a');
    const auto second = box({6, 2}, U'b');

    one.children().add(first);
    one.children().add(second);

    const Snapshot made = one.render({{0, 0}, {6, 4}});

    SAME(first->area().at.y, 0);
    SAME(second->area().at.y, 1);
    SAME(read(made, 0), std::string{"aaaaaa"});
    SAME(read(made, 1), std::string{"bbbbbb"});
    SAME(read(made, 2), std::string{"bbbbbb"});
    SAME(read(made, 3), std::string{""});
}

TRIAL("Row: children stand side by side") {
    ui::Row one;

    const auto first = box({2, 1}, U'a');
    const auto second = box({3, 1}, U'b');

    one.children().add(first);
    one.children().add(second);

    const Snapshot made = one.render({{0, 0}, {6, 1}});

    SAME(first->area().at.x, 0);
    SAME(second->area().at.x, 2);
    SAME(read(made, 0), std::string{"aabbb"});
}

// A flow: a child that does not fit in the rest of the line starts the next one, and each
// line is centered. A child of zero width opens neither a gap nor a line; one wider than the whole
// flow stands alone, clipped.
TRIAL("Flow: carries over what does not fit and centers the lines") {
    ui::Flow one{1};

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

    SAME(one.wants({12, 10}), (ui::Size{12, 3}));
    SAME(one.wants({12, 2}), (ui::Size{12, 2}));

    const Snapshot made = one.render({{0, 0}, {12, 3}});

    SAME(read(made, 0), std::string{"aaaaa bbbbb"});
    SAME(read(made, 1), std::string{"   ccccc"});
    SAME(read(made, 2), std::string{"dddddddddddd"});
    SAME(third->area().at.y, 1);
    SAME(empty->area().size.wide, 0);
    SAME(wide->area().size.wide, 12);

    // The gap counts when wrapping: two fives in ten columns make two lines, not one.
    SAME(one.wants({10, 10}), (ui::Size{10, 4}));

    // A child that stopped taking any room gets a zero rectangle, not the old one.
    first->wanting({0, 1});

    (void)one.render({{0, 0}, {12, 3}});

    SAME(first->area().size.wide, 0);

    first->wanting({5, 1});

    // Enough room: one line, as without wrapping.
    SAME(one.wants({60, 10}), (ui::Size{38, 1}));
}

TRIAL("Row: a child gets the full height") {
    ui::Row one;

    const auto first = box({2, 1}, U'a');

    one.children().add(first);

    (void)one.render({{0, 0}, {6, 3}});

    SAME(first->area().size.high, 3);
}

TRIAL("Column: a child gets the full width") {
    ui::Column one;

    const auto first = box({2, 1}, U'a');

    one.children().add(first);

    (void)one.render({{0, 0}, {6, 3}});

    SAME(first->area().size.wide, 6);
}

TRIAL("Column: wants the sum of heights, but no more than it got") {
    ui::Column one;

    one.children().add(box({4, 3}, U'a'));
    one.children().add(box({4, 3}, U'b'));

    SAME(one.wants({10, 10}).high, 6);
    SAME(one.wants({10, 4}).high, 4);
    SAME(one.wants({2, 10}).wide, 2);
}

// A child of zero height, an empty line or a hidden part, opens no gap and adds no width: the rule
// of `Row` with the axes swapped.
TRIAL("Column: a child of zero height opens no gap and adds no width") {
    ui::Column one{1};

    const auto last = box({2, 1}, U'b');

    one.children().add(box({2, 1}, U'a'));
    one.children().add(box({5, 0}, U'e'));
    one.children().add(last);

    SAME(one.wants({10, 10}), (ui::Size{2, 3}));

    (void)one.render({{0, 0}, {10, 3}});

    SAME(last->area().at.y, 2);
}

TRIAL("Row: wants the sum of widths, but no more than it got") {
    ui::Row one;

    one.children().add(box({4, 1}, U'a'));
    one.children().add(box({4, 1}, U'b'));

    SAME(one.wants({10, 10}).wide, 8);
    SAME(one.wants({5, 10}).wide, 5);
    SAME(one.wants({10, 1}).high, 1);
}

TRIAL("Row: a child past the right edge gets zero width") {
    ui::Row one;

    const auto first = box({4, 1}, U'a');
    const auto second = box({4, 1}, U'b');

    one.children().add(first);
    one.children().add(second);

    (void)one.render({{0, 0}, {4, 1}});

    SAME(first->area().size.wide, 4);
    SAME(second->area().size.wide, 0);
    SAME(second->area().at.x, 4);
}

TRIAL("Row in a column is laid out as expected") {
    ui::Column outer;

    const auto row = std::make_shared<ui::Row>();

    row->children().add(std::make_shared<ui::Span>("ala"));
    row->children().add(std::make_shared<ui::Span>("kot"));

    outer.children().add(row);
    outer.children().add(std::make_shared<ui::Span>("pies"));

    const Snapshot made = outer.render({{0, 0}, {8, 2}});

    SAME(read(made, 0), std::string{"alakot"});
    SAME(read(made, 1), std::string{"pies"});
}
