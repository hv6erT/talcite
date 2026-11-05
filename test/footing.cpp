#include "proving.hpp"

#include <algorithm>
#include <memory>
#include <string>

#include "talcite/encoding.hpp"
#include "talcite/footed.hpp"
#include "talcite/inset.hpp"

using namespace talcite;

// `Footed`: the content on top, the parts under it measured first, from the bottom up; `Inset`: one
// child with room around it.

namespace {

class Box : public ui::Node {
public:
    Box(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    // The size set in the constructor, within the room offered.
    [[nodiscard]] ui::Size wants(ui::Size room) const override {
        return {std::min(wants_.wide, room.wide), std::min(wants_.high, room.high)};
    }

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
std::shared_ptr<Box> box(ui::Size wants, char32_t what) { return std::make_shared<Box>(wants, what); }

}

// A panel: the table on top, a rule and a status line at the bottom. The table shows two rows but
// gets all the room the others leave, so the status line stands at the bottom edge; asked for its
// size, the column wants only what its children want.
TRIAL("Footed: the content gets all the room the parts under it leave") {
    ui::Footed one;

    const auto table = box({4, 2}, U't');
    const auto rule = box({4, 1}, U'-');
    const auto status = box({3, 1}, U's');

    one.children().add(table);
    one.children().add(rule);
    one.children().add(status);

    same(one.wants({10, 8}), (ui::Size{4, 4}));

    const Snapshot made = one.render({{0, 0}, {5, 6}});

    same(table->area(), (ui::Area{{0, 0}, {5, 4}}));
    same(read(made, 3), std::string{"ttttt"});
    same(read(made, 4), std::string{"-----"});
    same(read(made, 5), std::string{"sssss"});
}

// When the room is short the parts at the bottom keep their place, from the last one up, and the
// content gives way.
TRIAL("Footed: the parts at the bottom keep their place when the room is short") {
    ui::Footed one;

    const auto table = box({4, 9}, U't');
    const auto rule = box({4, 1}, U'-');
    const auto status = box({4, 1}, U's');

    one.children().add(table);
    one.children().add(rule);
    one.children().add(status);

    same(one.wants({10, 3}), (ui::Size{4, 3}));

    const Snapshot made = one.render({{0, 0}, {4, 3}});

    same(read(made, 0), std::string{"tttt"});
    same(read(made, 1), std::string{"----"});
    same(read(made, 2), std::string{"ssss"});

    // With a single row the status line keeps it.
    const Snapshot low = one.render({{0, 0}, {4, 1}});

    same(read(low, 0), std::string{"ssss"});
    same(table->area().size.high, 0);
}

// The gap stands only between two children that both take some height: none above a part that is
// empty, none under content that is empty.
TRIAL("Footed: a gap only between children that both take some height") {
    ui::Footed one{1};

    const auto body = box({4, 2}, U'b');
    const auto empty = box({4, 0}, U'e');
    const auto foot = box({4, 1}, U'f');

    one.children().add(body);
    one.children().add(empty);
    one.children().add(foot);

    same(one.wants({10, 10}), (ui::Size{4, 4}));

    const Snapshot made = one.render({{0, 0}, {4, 4}});

    same(read(made, 0), std::string{"bbbb"});
    same(read(made, 1), std::string{"bbbb"});
    same(read(made, 2), std::string{""});
    same(read(made, 3), std::string{"ffff"});

    // A single row: the footer gets no room after the gap, so neither stands, and the content keeps
    // the row.
    same(read(one.render({{0, 0}, {4, 1}}), 0), std::string{"bbbb"});

    ui::Footed bare{1};

    bare.children().add(box({4, 0}, U'b'));
    bare.children().add(box({4, 1}, U'f'));

    same(bare.wants({10, 10}), (ui::Size{4, 1}));
}

// Room around one child: cells on both sides and rows above, transparent; an empty child takes no
// height, so nothing opens beside it.
TRIAL("Inset: the child with room around it") {
    ui::Inset one{2, 1};

    const auto inside = box({3, 1}, U'i');

    one.children().add(inside);

    same(one.wants({20, 10}), (ui::Size{7, 2}));

    const Snapshot made = one.render({{0, 0}, {7, 2}});

    same(read(made, 0), std::string{""});
    same(read(made, 1), std::string{"  iii"});
    same(inside->area(), (ui::Area{{2, 1}, {3, 1}}));

    ui::Inset empty{1};

    empty.children().add(box({0, 0}, U'x'));

    same(empty.wants({20, 10}), (ui::Size{2, 0}));
    same(ui::Inset{3}.wants({4, 5}), (ui::Size{4, 0}));
}
