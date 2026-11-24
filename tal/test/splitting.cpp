#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "tal/document.hpp"
#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/layout.hpp"
#include "tal/node.hpp"
#include "tal/scene.hpp"
#include "tal/style.hpp"

#include "tal/testing.hpp"

// A split shares its room among its children in proportion to their parts, none below its least,
// with lines in its gaps; a border moves by a call or by dragging it with the mouse where nothing
// under the pointer takes the press, and every move tells `Resized`.

namespace {

using namespace std::chrono_literals;
using tal::Area;
using tal::Axis;
using tal::Document;
using tal::Hook;
using tal::MouseButton;
using tal::Node;
using tal::Press;
using tal::Resized;
using tal::Size;
using tal::Split;
using tal::Spot;
using tal::State;
using tal::testing::Scene;

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

// The lengths of the children of `split` along its axis, in order.
std::vector<int> lengths(Split& split) {
    std::vector<int> made;

    for (Node& one : split.children()) made.push_back(*split.axis == Axis::horizontal ? one.area().size.wide : one.area().size.high);

    return made;
}

// A node that takes every press of a button on it, as a control would.
class Grabbing : public Node {
public:
    Grabbing() {
        taking_ = on<tal::MouseEvent>([](tal::MouseEvent& what) {
            if (what.press == Press::down) what.take();
        });
    }

private:
    Hook taking_;
};

// Where the children of `split` start along its axis, in order.
std::vector<int> starts(Split& split) {
    std::vector<int> made;

    for (Node& one : split.children()) made.push_back(*split.axis == Axis::horizontal ? one.area().at.x : one.area().at.y);

    return made;
}

}

// Parts alike share the room evenly, the cells the rounding leaves going to the first children;
// parts set share it in their proportion; a child without a part takes the mean of those set. The
// gaps come out of the room first, the children stand one after another with them between, each
// over the whole extent across. A split down the screen does the same along its height.
TRIAL("Split: children share the room in proportion to their parts") {
    Scene scene{{.screen = {30, 5}}};
    Document document{scene.time()};
    Split& split = document.children().add<Split>(Axis::horizontal);
    Node& one = split.children().add<Node>();
    Node& two = split.children().add<Node>();
    Node& three = split.children().add<Node>();

    scene.show(document);
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{10, 10, 10}));
    SAME(starts(split), (std::vector<int>{0, 10, 20}));
    SAME(one.area().size.high, 5);
    SAME(split.part(two), 1.0);
    SAME(split.borders(), std::size_t{2});

    split.part(one, 2);
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{10, 10, 10}));
    SAME(split.part(three), 2.0);

    split.part(two, 1);
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{13, 7, 10}));
    SAME(split.part(three), 1.5);

    split.part(two, 2);
    split.part(three, 2);
    scene.resize(Size{31, 5});

    SAME(lengths(split), (std::vector<int>{11, 10, 10}));

    Scene gapped{{.screen = {5, 30}}};
    Document other{gapped.time()};
    Split& apart = other.children().add<Split>(Axis::vertical, 1);

    for (int count = 0; count < 3; ++count) (void)apart.children().add<Node>();

    gapped.show(other);
    gapped.wait(0ms);

    SAME(lengths(apart), (std::vector<int>{10, 9, 9}));
    SAME(starts(apart), (std::vector<int>{0, 11, 21}));
    SAME((*apart.children().begin()).area().size.wide, 5);
    SAME(gapped.at({2, 10}).cluster, std::string{"\xe2\x94\x80"});
    SAME(gapped.at({2, 20}).cluster, std::string{"\xe2\x94\x80"});
    SAME(gapped.at({2, 9}).cluster, std::string{" "});
}

// A child whose share falls below `least` gets `least`, and the others share what is left in their
// proportion; where the room is less than `least` for each child, the children share it evenly
// whatever their parts.
TRIAL("Split: no child below least while the room allows") {
    Scene scene{{.screen = {22, 3}}};
    Document document{scene.time()};
    Split& split = document.children().add<Split>(Axis::horizontal);
    Node& small = split.children().add<Node>();
    Node& large = split.children().add<Node>();
    Node& middle = split.children().add<Node>();

    split.part(small, 1);
    split.part(large, 20);
    split.part(middle, 1);
    scene.show(document);
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{4, 14, 4}));

    split.least = 2;
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{2, 18, 2}));

    split.least = 8;
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{8, 7, 7}));

    split.least = 4;
    split.part(small, 3);
    split.part(large, 3);
    split.part(middle, 1);
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{9, 9, 4}));
}

// A move of a border grows the child before it by what the one after it loses, neither below
// `least`, and tells `Resized` with the border; from then on the parts are the lengths, so a larger
// screen keeps the proportions. A border past the last child, a move of nothing, a move against a
// child already at `least` and any move before the split is first arranged do nothing and tell
// nothing.
TRIAL("Split: a border moved by a call") {
    Scene scene{{.screen = {30, 3}}};
    Document document{scene.time()};
    Split& split = document.children().add<Split>(Axis::horizontal);
    Node& one = split.children().add<Node>();
    Node& two = split.children().add<Node>();
    std::vector<std::size_t> told;
    const Hook heard = split.on<Resized>([&told](Resized& what) { told.push_back(what.border); });

    split.moved(0, 3);

    OUGHT(told.empty());

    scene.show(document);
    scene.wait(0ms);
    split.moved(0, 5);
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{20, 10}));
    SAME(told, (std::vector<std::size_t>{0}));
    SAME(split.part(one), 20.0);
    SAME(split.part(two), 10.0);

    split.moved(0, 100);
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{26, 4}));

    split.moved(0, 1);
    split.moved(0, 0);
    split.moved(1, 3);
    split.moved(7, -3);
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{26, 4}));
    SAME(told, (std::vector<std::size_t>{0, 0}));

    split.moved(0, -100);
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{4, 26}));

    split.moved(0, 11);
    scene.resize(Size{60, 3});

    SAME(lengths(split), (std::vector<int>{30, 30}));
    SAME(told, (std::vector<std::size_t>{0, 0, 0, 0}));

    Node& three = split.children().add<Node>();

    scene.wait(0ms);

    SAME(split.part(three), 15.0);
    SAME(lengths(split), (std::vector<int>{20, 20, 20}));
}

// The border is dragged from the last cell of the child before it (and from the gap after that
// cell): the border follows the pointer by as much as it moved, with the state `drag` on the split
// until the button comes up, even over another child. A press that a node under the pointer takes
// (a control standing at the border) is that node's; a press of another button, on the first cell
// of the child after the border, or away from a border, drags nothing.
TRIAL("Split: a border dragged with the mouse") {
    Scene scene{{.screen = {30, 3}}};
    Document document{scene.time()};
    Split& split = document.children().add<Split>(Axis::horizontal);

    (void)split.children().add<Node>();
    (void)split.children().add<Node>();

    std::vector<std::size_t> told;
    const Hook heard = split.on<Resized>([&told](Resized& what) { told.push_back(what.border); });

    scene.show(document);
    scene.wait(0ms);

    document.mouse(Press::down, MouseButton::left, Spot{14, 1});

    OUGHT(split.states().has(State::drag));

    document.mouse(Press::move, MouseButton::left, Spot{20, 2});
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{21, 9}));

    document.mouse(Press::move, MouseButton::left, Spot{29, 0});
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{26, 4}));

    document.mouse(Press::up, MouseButton::left, Spot{29, 0});

    OUGHT(!split.states().has(State::drag));

    document.mouse(Press::move, MouseButton::left, Spot{5, 0});
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{26, 4}));

    document.mouse(Press::down, MouseButton::left, Spot{26, 1});
    document.mouse(Press::move, MouseButton::left, Spot{20, 1});
    document.mouse(Press::up, MouseButton::left, Spot{20, 1});
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{26, 4}));

    document.mouse(Press::down, MouseButton::left, Spot{25, 1});
    document.mouse(Press::move, MouseButton::left, Spot{23, 1});
    document.mouse(Press::up, MouseButton::left, Spot{23, 1});
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{24, 6}));

    document.mouse(Press::down, MouseButton::right, Spot{23, 1});
    document.mouse(Press::move, MouseButton::right, Spot{10, 1});
    document.mouse(Press::up, MouseButton::right, Spot{10, 1});
    document.mouse(Press::down, MouseButton::left, Spot{10, 1});
    document.mouse(Press::move, MouseButton::left, Spot{5, 1});
    document.mouse(Press::up, MouseButton::left, Spot{5, 1});
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{24, 6}));
    OUGHT(told.size() == 3);

    (void)(*split.children().begin()).children().add<Grabbing>();
    scene.wait(0ms);
    document.mouse(Press::down, MouseButton::left, Spot{23, 1});
    document.mouse(Press::move, MouseButton::left, Spot{20, 1});
    document.mouse(Press::up, MouseButton::left, Spot{20, 1});
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{24, 6}));

    Scene down{{.screen = {10, 21}}};
    Document other{down.time()};
    Split& apart = other.children().add<Split>(Axis::vertical, 1);

    (void)apart.children().add<Node>();
    (void)apart.children().add<Node>();
    down.show(other);
    down.wait(0ms);

    SAME(lengths(apart), (std::vector<int>{10, 10}));

    other.mouse(Press::down, MouseButton::left, Spot{4, 11});
    other.mouse(Press::move, MouseButton::left, Spot{4, 6});
    other.mouse(Press::up, MouseButton::left, Spot{4, 6});
    down.wait(0ms);

    SAME(lengths(apart), (std::vector<int>{10, 10}));

    other.mouse(Press::down, MouseButton::left, Spot{4, 10});
    other.mouse(Press::move, MouseButton::left, Spot{4, 6});
    other.mouse(Press::up, MouseButton::left, Spot{4, 6});
    down.wait(0ms);

    SAME(lengths(apart), (std::vector<int>{6, 14}));
    SAME(down.at({0, 6}).cluster, std::string{"\xe2\x94\x80"});

    other.mouse(Press::down, MouseButton::left, Spot{4, 3});
    other.mouse(Press::move, MouseButton::left, Spot{4, 0});
    other.mouse(Press::up, MouseButton::left, Spot{4, 0});
    down.wait(0ms);

    SAME(lengths(apart), (std::vector<int>{6, 14}));
}

// A drag follows only moves: the wheel turned while dragging moves no border, and a press without a
// release before it (one the terminal lost) ends the drag, whatever the moves after it.
TRIAL("Split: a drag moves with moves only, and a new press ends it") {
    Scene scene{{.screen = {30, 3}}};
    Document document{scene.time()};
    Split& split = document.children().add<Split>(Axis::horizontal);

    (void)split.children().add<Node>();
    (void)split.children().add<Node>();
    scene.show(document);
    scene.wait(0ms);

    document.mouse(Press::down, MouseButton::left, Spot{14, 1});
    document.mouse(Press::move, MouseButton::left, Spot{18, 1});
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{19, 11}));

    document.mouse(Press::wheel, MouseButton::none, Spot{5, 1}, {}, 1);
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{19, 11}));

    document.mouse(Press::down, MouseButton::left, Spot{5, 1});
    document.mouse(Press::move, MouseButton::left, Spot{25, 1});
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{19, 11}));
    OUGHT(!split.states().has(State::drag));

    document.mouse(Press::up, MouseButton::left, Spot{25, 1});
}

// A move of a border leaves the other children their lengths; where the two children beside it
// have less than `least` each, the border does not move.
TRIAL("Split: a border moved between two of three, and in too little room") {
    Scene scene{{.screen = {40, 3}}};
    Document document{scene.time()};
    Split& split = document.children().add<Split>(Axis::horizontal);
    Node& one = split.children().add<Node>();
    Node& two = split.children().add<Node>();
    Node& three = split.children().add<Node>();

    split.part(one, 1);
    split.part(two, 2);
    split.part(three, 1);
    scene.show(document);
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{10, 20, 10}));

    split.moved(1, 5);
    scene.wait(0ms);

    SAME(lengths(split), (std::vector<int>{10, 25, 5}));

    Scene small{{.screen = {6, 3}}};
    Document other{small.time()};
    Split& tight = other.children().add<Split>(Axis::horizontal);
    std::vector<std::size_t> told;
    const Hook heard = tight.on<Resized>([&told](Resized& what) { told.push_back(what.border); });

    (void)tight.children().add<Node>();
    (void)tight.children().add<Node>();
    small.show(other);
    small.wait(0ms);

    SAME(lengths(tight), (std::vector<int>{3, 3}));

    tight.moved(0, 1);
    tight.moved(0, -1);
    small.wait(0ms);

    SAME(lengths(tight), (std::vector<int>{3, 3}));
    OUGHT(told.empty());
}

// A part asked or set for a node that is not a child throws std::logic_error; a part that is not a
// positive number throws std::invalid_argument and changes nothing.
TRIAL("Split: parts of strangers and parts that are not positive") {
    Document document;
    Split& split = document.children().add<Split>(Axis::horizontal);
    Node& one = split.children().add<Node>();
    Node stranger;

    OUGHT(throws<std::logic_error>([&] { split.part(stranger, 1); }));
    OUGHT(throws<std::logic_error>([&] { (void)split.part(stranger); }));

    for (const double bad : {0.0, -1.0, std::nan(""), std::numeric_limits<double>::infinity()})
        OUGHT(throws<std::invalid_argument>([&] { split.part(one, bad); }));

    SAME(split.part(one), 1.0);
    SAME(Split{Axis::horizontal}.borders(), std::size_t{0});
}
