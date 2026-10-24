#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/encoding.hpp"
#include "talcite/float.hpp"
#include "talcite/row.hpp"

using namespace talcite;

// Floats: `Left`, `Right` and `Center` stand one child at their side of the width they get; a row
// widens them by the room it does not need, split equally between their free sides.

namespace {

class Box : public ui::Node {
public:
    Box(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    // The size set in the constructor, whatever room is offered.
    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

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
    ought(right->side() == ui::Justify::right);
}
