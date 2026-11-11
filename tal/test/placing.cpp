#include <stdexcept>
#include <vector>

#include "tal/document.hpp"
#include "tal/geometry.hpp"
#include "tal/layout.hpp"
#include "tal/node.hpp"
#include "tal/style.hpp"

#include "tal/testing.hpp"

// Layouts: rows and columns that share their room by weights, a grid of tracks, and a holder that
// stands its child at an edge.

namespace {

using tal::Align;
using tal::Area;
using tal::Axis;
using tal::Colors;
using tal::Column;
using tal::Document;
using tal::Edge;
using tal::Grid;
using tal::Limits;
using tal::Node;
using tal::Row;
using tal::Share;
using tal::Size;
using tal::Spot;

// A node that wants a fixed size.
class Block : public Node {
public:
    explicit Block(Size want) : want_(want) {}

protected:
    // The size it was made with.
    Size measure(Limits room) const override {
        (void)room;

        return want_;
    }

private:
    Size want_;
};

// Whether `what` throws an exception of type E.
template <typename E, typename F>
bool throws(F what) {
    try {
        what();
    } catch (const E&) {
        return true;
    }

    return false;
}

// A document of `screen` cells holding a layout of type T made from `given`, laid out once.
template <typename T>
struct Laid {
    Document document;
    T* layout = nullptr;

    // Makes the layout and its document.
    template <typename... Given>
    explicit Laid(Size screen, Given&&... given) {
        document.display(screen, Colors::rgb, true);
        layout = &document.children().template add<T>(std::forward<Given>(given)...);
    }

    // Adds a block wanting `want`, sharing as `how`.
    Block& block(Size want, Share how = {}) {
        Block& made = layout->children().template add<Block>(want);

        made.share(how);

        return made;
    }

    // Makes a frame.
    void framed() { (void)document.frame(); }
};

// Children with weight 0 get what they want in order, those with a weight their least and then the
// rest in proportion; the cells the division leaves over go one each to the first of them. Across,
// every child gets the row's full height.
TRIAL("Row: fixed children first, then the weighted share the rest by their weights") {
    Laid<Row> laid{Size{30, 3}, 1};
    Block& fixed = laid.block({4, 1});
    Block& one = laid.block({9, 2}, Share{1, 2});
    Block& two = laid.block({1, 1}, Share{2, 0});
    Block& last = laid.block({5, 1});

    laid.framed();

    OUGHT(fixed.area() == (Area{{0, 0}, {4, 3}}));
    OUGHT(one.area() == (Area{{5, 0}, {8, 3}}));
    OUGHT(two.area() == (Area{{14, 0}, {10, 3}}));
    OUGHT(last.area() == (Area{{25, 0}, {5, 3}}));
}

// A row wants its children's widths (a weighted child at least its least) and the gaps; across,
// its tallest child. A negative gap counts as none, and an empty row wants the least of its room.
TRIAL("Row: what a row wants, with gaps, and an empty one") {
    Laid<Row> laid{Size{30, 5}, 2};

    (void)laid.block({4, 1});
    (void)laid.block({9, 3}, Share{1, 2});
    (void)laid.block({5, 2});

    OUGHT(laid.layout->measured(Limits{{0, 0}, {30, 5}}) == (Size{4 + 9 + 5 + 2 * 2, 3}));

    Laid<Row> reserved{Size{30, 5}};

    (void)reserved.block({2, 1}, Share{1, 6});

    OUGHT(reserved.layout->measured(Limits{{0, 0}, {30, 5}}) == (Size{6, 1}));

    Laid<Row> tight{Size{30, 5}, -3};

    (void)tight.block({4, 1});
    (void)tight.block({5, 2});

    OUGHT(tight.layout->measured(Limits{{0, 0}, {30, 5}}) == (Size{9, 2}));

    Laid<Row> empty{Size{30, 5}};

    OUGHT(empty.layout->measured(Limits{{3, 1}, {30, 5}}) == (Size{3, 1}));
}

// Without enough room the fixed children take what there is in order, then the least of the
// weighted ones; who comes last gets nothing. Gaps stay.
TRIAL("Row: without room the first children take what there is") {
    Laid<Row> laid{Size{10, 1}, 1};
    Block& first = laid.block({6, 1});
    Block& weighted = laid.block({1, 1}, Share{1, 3});
    Block& second = laid.block({6, 1});

    laid.framed();

    OUGHT(first.area() == (Area{{0, 0}, {6, 1}}));
    OUGHT(second.area() == (Area{{8, 0}, {2, 1}}));
    OUGHT(weighted.area() == (Area{{7, 0}, {0, 1}}));

    Laid<Row> two{Size{5, 1}};
    Block& fixed = two.block({3, 1});
    Block& greedy = two.block({1, 1}, Share{1, 3});
    Block& after = two.block({1, 1}, Share{1, 0});

    two.framed();

    OUGHT(fixed.area() == (Area{{0, 0}, {3, 1}}));
    OUGHT(greedy.area() == (Area{{3, 0}, {2, 1}}));
    OUGHT(after.area() == (Area{{5, 0}, {0, 1}}));
}

// A column lays out down as a row does across. A content of weight 1 over bars of weight 0 is the
// old footed column: the bars get their rows first, the content what is left.
TRIAL("Column: children down by the same rule, the content over its bars") {
    Laid<Column> laid{Size{20, 10}};
    Block& content = laid.block({20, 50}, Share{1, 0});
    Block& rule = laid.block({20, 1});
    Block& status = laid.block({5, 1});

    laid.framed();

    OUGHT(content.area() == (Area{{0, 0}, {20, 8}}));
    OUGHT(rule.area() == (Area{{0, 8}, {20, 1}}));
    OUGHT(status.area() == (Area{{0, 9}, {20, 1}}));
    OUGHT(laid.layout->measured(Limits{{0, 0}, {20, 60}}) == (Size{20, 52}));
}

// The share is the child's; a new one lays the parent out again, and a negative weight or least
// counts as zero.
TRIAL("Share: a new share lays the parent out again, negative parts count as zero") {
    Laid<Row> laid{Size{12, 1}};
    Block& one = laid.block({2, 1});
    Block& two = laid.block({2, 1});

    laid.framed();

    OUGHT(two.area() == (Area{{2, 0}, {2, 1}}));

    one.share(Share{1, 0});

    OUGHT(laid.document.due());

    laid.framed();

    OUGHT(one.area() == (Area{{0, 0}, {10, 1}}));
    OUGHT(two.area() == (Area{{10, 0}, {2, 1}}));

    two.share(Share{-2, -5});

    OUGHT(two.share() == (Share{0, 0}));
}

// Tracks of weight 0 are as large as their largest child standing in them alone (the second row has
// none, so it takes no room); weighted tracks share the rest. Children put in cells stand there,
// spanning several when asked, and the others take the free cells in reading order.
TRIAL("Grid: tracks by their children and weights, cells put and cells taken in order") {
    Laid<Grid> laid{Size{20, 6}, std::vector<Share>{{}, Share{1, 0}, {}}, std::vector<Share>{{}, {}, Share{1, 0}}, 1};
    Block& wide = laid.block({4, 1});
    Block& spanning = laid.block({30, 1});
    Block& corner = laid.block({2, 1});
    Block& next = laid.block({1, 2});

    laid.layout->at(spanning, Spot{1, 2}, Size{2, 1});
    laid.layout->at(corner, Spot{2, 0});

    laid.framed();

    OUGHT(wide.area() == (Area{{0, 0}, {4, 2}}));
    OUGHT(corner.area() == (Area{{18, 0}, {2, 2}}));
    OUGHT(next.area() == (Area{{5, 0}, {12, 2}}));
    OUGHT(spanning.area() == (Area{{5, 4}, {15, 2}}));
}

// More children than free cells: the last get no room. A child put over cells another one already
// holds gets none either.
TRIAL("Grid: children without a free cell get no room") {
    Laid<Grid> laid{Size{10, 2}, std::vector<Share>{{}, {}}, std::vector<Share>{{}}, 0};
    Block& one = laid.block({1, 1});
    Block& two = laid.block({1, 1});
    Block& three = laid.block({1, 1});

    laid.layout->at(two, Spot{0, 0});
    laid.layout->at(three, Spot{0, 0});

    laid.framed();

    OUGHT(two.area() == (Area{{0, 0}, {1, 1}}));
    OUGHT(one.area() == (Area{{1, 0}, {1, 1}}));
    OUGHT(three.area().size == (Size{0, 0}));

    laid.layout->at(three, Spot{1, 0});

    laid.framed();

    OUGHT(three.area() == (Area{{1, 0}, {1, 1}}));
    OUGHT(one.area().size == (Size{0, 0}));
}

// A child spanning several tracks does not size them: a track of weight 0 is as large as its
// largest child standing in it alone, also when a wider one spans it with its neighbour.
TRIAL("Grid: a spanning child does not size its tracks") {
    Laid<Grid> laid{Size{20, 2}, std::vector<Share>{{}, {}}, std::vector<Share>{{}, {}}, 0};
    Block& spanning = laid.block({12, 1});
    Block& left = laid.block({3, 1});
    Block& right = laid.block({4, 1});

    laid.layout->at(spanning, Spot{0, 0}, Size{2, 1});
    laid.layout->at(left, Spot{0, 1});
    laid.layout->at(right, Spot{1, 1});

    laid.framed();

    OUGHT(spanning.area() == (Area{{0, 0}, {7, 1}}));
    OUGHT(right.area() == (Area{{3, 1}, {4, 1}}));
    OUGHT(laid.layout->measured(Limits{{0, 0}, {20, 2}}) == (Size{7, 2}));
}

TRIAL("Grid: what it wants, and cells outside it or a stranger throw") {
    Laid<Grid> laid{Size{20, 6}, std::vector<Share>{{}, Share{2, 3}}, std::vector<Share>{{}, {}}, 1};
    Block& one = laid.block({4, 2});
    Block& two = laid.block({7, 1});
    Block& three = laid.block({2, 3});
    Block stranger{Size{1, 1}};

    (void)two;

    laid.layout->at(three, Spot{1, 1});

    OUGHT(laid.layout->measured(Limits{{0, 0}, {20, 6}}) == (Size{4 + 1 + 7, 2 + 1 + 3}));
    OUGHT(throws<std::out_of_range>([&] { laid.layout->at(one, Spot{1, 1}, Size{2, 1}); }));
    OUGHT(throws<std::out_of_range>([&] { laid.layout->at(one, Spot{0, 2}); }));
    OUGHT(throws<std::out_of_range>([&] { laid.layout->at(one, Spot{-1, 0}); }));
    OUGHT(throws<std::out_of_range>([&] { laid.layout->at(one, Spot{0, 0}, Size{0, 1}); }));
    OUGHT(throws<std::logic_error>([&] { laid.layout->at(stranger, Spot{0, 0}); }));
}

// Along its axis the child gets what it wants and stands at the start, in the middle or at the end;
// across, the full extent.
TRIAL("Align: the child at an edge or in the middle along the axis") {
    const auto placed = [](Axis axis, Edge where) {
        Laid<Align> laid{Size{11, 5}, axis, where};
        Block& one = laid.block({4, 2});

        laid.framed();

        return one.area();
    };

    OUGHT(placed(Axis::horizontal, Edge::start) == (Area{{0, 0}, {4, 5}}));
    OUGHT(placed(Axis::horizontal, Edge::middle) == (Area{{3, 0}, {4, 5}}));
    OUGHT(placed(Axis::horizontal, Edge::end) == (Area{{7, 0}, {4, 5}}));
    OUGHT(placed(Axis::vertical, Edge::middle) == (Area{{0, 1}, {11, 2}}));
    OUGHT(placed(Axis::vertical, Edge::end) == (Area{{0, 3}, {11, 2}}));

    Laid<Align> big{Size{3, 3}, Axis::horizontal, Edge::end};
    Block& wide = big.block({8, 1});

    big.framed();

    OUGHT(wide.area() == (Area{{0, 0}, {3, 3}}));
    OUGHT(big.layout->measured(Limits{{0, 0}, {3, 3}}) == (Size{3, 1}));
}

}
