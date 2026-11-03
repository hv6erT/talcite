#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/encoding.hpp"
#include "talcite/float.hpp"
#include "talcite/row.hpp"

using namespace talcite;

// Floats: `Left`, `Right` and `Center` stand one child at their side of the width they get, `Top`,
// `Bottom` and `Middle` at their side of the height; a row widens the first ones and a column
// lengthens the others by the room it does not need, split equally between their free sides.

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

// A float of type `Side` holding `inside`.
template <typename Side>
std::shared_ptr<Side> holding(std::shared_ptr<ui::Node> inside) {
    auto made = std::make_shared<Side>();

    made->children().add(std::move(inside));

    return made;
}

}

// The last row of a window: what it says on the left, its buttons on the right. Asked for its size,
// the row wants only what its children want; the spare room exists only when it is given.
TRIAL("Float: a right float stands at the right edge of a row") {
    ui::Row one{1};

    const auto note = box({3, 1}, U'n');
    const auto yes = box({2, 1}, U'y');
    const auto no = box({2, 1}, U'z');
    const auto buttons = std::make_shared<ui::Row>(1);

    buttons->children().add(yes);
    buttons->children().add(no);

    const auto right = holding<ui::Right>(buttons);

    one.children().add(note);
    one.children().add(right);

    same(one.wants({20, 1}).wide, 9);
    same(right->wants({20, 1}).wide, 5);

    const View made = one.render({{0, 0}, {14, 1}});

    same(read(made, 0), std::string{"nnn      yy zz"});
    same(note->area().at.x, 0);
    same(right->area().at.x, 4);
    same(right->area().size.wide, 10);
    same(buttons->area().at.x, 5);

    same(read(one.render({{0, 0}, {9, 1}}), 0), std::string{"nnn yy zz"});
}

// Four free sides (left float: one, centered: two, right float: one) share eight spare cells, two
// each. With nine, the odd cell goes to the last side.
TRIAL("Float: the spare width is split equally between the free sides") {
    ui::Row one;

    one.children().add(holding<ui::Left>(box({2, 1}, U'a')));
    one.children().add(holding<ui::Center>(box({2, 1}, U'b')));
    one.children().add(holding<ui::Right>(box({2, 1}, U'c')));

    same(read(one.render({{0, 0}, {14, 1}}), 0), std::string{"aa    bb    cc"});
    same(read(one.render({{0, 0}, {15, 1}}), 0), std::string{"aa    bb     cc"});
}

// A centered float between two plain children stands in the middle between them; with an odd cell
// to spare it stands where `Center` alone would put it, the odd cell on its right.
TRIAL("Float: a centered float stands between its neighbors") {
    ui::Row one;

    const auto middle = box({2, 1}, U'b');

    one.children().add(box({1, 1}, U'x'));
    one.children().add(holding<ui::Center>(middle));
    one.children().add(box({1, 1}, U'y'));

    same(read(one.render({{0, 0}, {8, 1}}), 0), std::string{"x  bb  y"});
    same(read(one.render({{0, 0}, {7, 1}}), 0), std::string{"x bb  y"});
}

// A float that holds nothing visible takes no margin and opens no gap: the spare room stays after
// the last child, as in a row without floats.
TRIAL("Float: a float that occupies nothing gets no margin") {
    ui::Row one{1};

    one.children().add(box({2, 1}, U'a'));
    one.children().add(holding<ui::Right>(box({0, 1}, U'0')));
    one.children().add(box({2, 1}, U'b'));

    same(read(one.render({{0, 0}, {10, 1}}), 0), std::string{"aa bb"});
}

// A column gives every child its whole width, so a float there places its child across it, at its
// wanted width and over the full height it gets.
TRIAL("Float: in a column the child stands at the float's side") {
    ui::Column one;

    const auto left = box({4, 1}, U'l');
    const auto middle = box({4, 1}, U'm');
    const auto right = box({4, 1}, U'r');

    one.children().add(holding<ui::Left>(left));
    one.children().add(holding<ui::Center>(middle));
    one.children().add(holding<ui::Right>(right));

    same(one.wants({10, 5}), (ui::Size{4, 3}));

    const View made = one.render({{0, 0}, {10, 3}});

    same(read(made, 0), std::string{"llll"});
    same(read(made, 1), std::string{"   mmmm"});
    same(read(made, 2), std::string{"      rrrr"});
    same(middle->area().at.x, 3);
    same(right->area().size.wide, 4);
}

// A float wants what it holds; an empty one wants nothing.
TRIAL("Float: wants what it holds") {
    const auto right = holding<ui::Right>(box({4, 2}, U'a'));

    same(right->wants({20, 5}), (ui::Size{4, 2}));
    same(ui::Center{}.wants({20, 5}), (ui::Size{0, 0}));
}

// The six floats are two axes and three sides of each; the class says which.
TRIAL("Float: each class has its axis and its side") {
    ought(ui::Left{}.axis() == ui::Axis::horizontal && ui::Left{}.align() == ui::Align::start);
    ought(ui::Right{}.axis() == ui::Axis::horizontal && ui::Right{}.align() == ui::Align::end);
    ought(ui::Center{}.axis() == ui::Axis::horizontal && ui::Center{}.align() == ui::Align::middle);
    ought(ui::Top{}.axis() == ui::Axis::vertical && ui::Top{}.align() == ui::Align::start);
    ought(ui::Bottom{}.axis() == ui::Axis::vertical && ui::Bottom{}.align() == ui::Align::end);
    ought(ui::Middle{}.axis() == ui::Axis::vertical && ui::Middle{}.align() == ui::Align::middle);
}

// A panel's content and its status line at the bottom edge: a column gives the rows it does not
// need to the bottom float's free side above it; asked for its size, it wants only what its
// children want.
TRIAL("Float: a bottom float stands at the bottom edge of a column") {
    ui::Column one;

    const auto content = box({4, 1}, U'c');
    const auto status = box({4, 1}, U's');

    one.children().add(content);
    one.children().add(holding<ui::Bottom>(status));

    same(one.wants({10, 5}), (ui::Size{4, 2}));

    const View made = one.render({{0, 0}, {6, 5}});

    same(read(made, 0), std::string{"cccccc"});
    same(read(made, 3), std::string{""});
    same(read(made, 4), std::string{"ssssss"});
    same(status->area(), (ui::Area{{0, 3}, {6, 1}}));
}

// The spare rows are split equally between the free sides of the vertical floats, the odd ones to
// the last margins, so a middle float with an odd row to spare has it under it.
TRIAL("Float: the spare height is split equally between the free sides") {
    ui::Column one;

    const auto low = box({2, 1}, U'b');
    const auto middle = box({2, 1}, U'm');

    one.children().add(holding<ui::Bottom>(low));
    one.children().add(holding<ui::Middle>(middle));

    const View made = one.render({{0, 0}, {2, 8}});

    same(read(made, 2), std::string{"bb"});
    same(read(made, 5), std::string{"mm"});
    same(read(made, 7), std::string{""});

    ui::Column odd;

    const auto alone = box({2, 1}, U'a');

    odd.children().add(holding<ui::Middle>(alone));

    (void)odd.render({{0, 0}, {2, 4}});

    same(alone->area().at.y, 1);
}

// The odd rows of the split go to the last margins, so with a middle float and a bottom one the
// bottom float's margin takes them and the middle one keeps an even share on both sides.
TRIAL("Float: the odd rows of the split go to the last margins") {
    ui::Column one;

    const auto middle = box({2, 1}, U'm');
    const auto low = box({2, 1}, U'b');

    one.children().add(holding<ui::Middle>(middle));
    one.children().add(holding<ui::Bottom>(low));

    const View made = one.render({{0, 0}, {2, 7}});

    same(read(made, 1), std::string{"mm"});
    same(read(made, 6), std::string{"bb"});
}

// A horizontal float is an ordinary child of a column: it gets the height it wants and no more,
// and the spare rows stay under the last child.
TRIAL("Float: a column gives a horizontal float no rows beyond what it holds") {
    ui::Column one;

    const auto right = box({2, 1}, U'r');
    const auto next = box({3, 1}, U'n');

    one.children().add(holding<ui::Right>(right));
    one.children().add(next);

    const View made = one.render({{0, 0}, {4, 4}});

    same(read(made, 0), std::string{"  rr"});
    same(read(made, 1), std::string{"nnnn"});
    same(read(made, 2), std::string{""});
}

// In a row every child gets the full height, so there a vertical float places its child in it, and
// the row gives it no width beyond what it holds; a horizontal float in a column does the same
// across the width (the trial above).
TRIAL("Float: in a row a vertical float places its child in the full height") {
    ui::Row one;

    const auto low = box({2, 1}, U'b');
    const auto next = box({1, 1}, U'n');

    one.children().add(holding<ui::Bottom>(low));
    one.children().add(next);

    const View made = one.render({{0, 0}, {6, 3}});

    same(read(made, 0), std::string{"  n"});
    same(read(made, 2), std::string{"bbn"});
    same(next->area().at.x, 2);
}
