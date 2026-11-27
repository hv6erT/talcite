#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "tal/document.hpp"
#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/key.hpp"
#include "tal/layout.hpp"
#include "tal/node.hpp"
#include "tal/scene.hpp"
#include "tal/style.hpp"

#include "tal/testing.hpp"

// Carrying a load with the mouse: a node begins it, the nodes under the pointer are told the load
// is over them and one may take it (the state `drag` while it does), the release drops it on the
// one that took it last; Esc, a new press, or a release over nothing that takes it drops nothing.

namespace {

using namespace std::chrono_literals;
using tal::Document;
using tal::DragOver;
using tal::Dropped;
using tal::Hook;
using tal::Key;
using tal::MouseButton;
using tal::Node;
using tal::Press;
using tal::Spot;
using tal::State;
using tal::testing::Scene;

// A load of a kind, named.
struct Apple : tal::Carried {
    explicit Apple(std::string named) : name(std::move(named)) {}

    std::string name;
};

// A load of another kind.
struct Stone : tal::Carried {};

// An apple that keeps, in `heard`, how each carrying of it landed.
struct Heeded : Apple {
    explicit Heeded(std::shared_ptr<std::vector<bool>> kept) : Apple("heeded"), heard(std::move(kept)) {}

    // Keeps `taken`.
    void landed(bool taken) override { heard->push_back(taken); }

    std::shared_ptr<std::vector<bool>> heard;
};

// A node that begins carrying an apple named after it when the left button, pressed on it, moves
// more than a cell away.
class Tree : public Node {
public:
    explicit Tree(std::string named) : name(std::move(named)) {
        hook_ = on<tal::MouseEvent>([this](tal::MouseEvent& what) {
            if (what.press == Press::down && what.button == MouseButton::left) {
                from = what.screen;
                capture();
                what.take();
            } else if (what.press == Press::move && from && (std::abs(what.screen.x - from->x) > 1 || std::abs(what.screen.y - from->y) > 1)) {
                from.reset();
                what.take();
                dragged(std::make_shared<Apple>(name));
            }
        });
    }

    // Begins carrying `one` at once.
    void lift(std::shared_ptr<tal::Carried> one) { dragged(std::move(one)); }

    std::string name;
    std::optional<Spot> from;

private:
    Hook hook_;
};

// A node that takes apples (not stones) over its whole area, or only over its right half when
// `half`, and keeps what was dropped on it and where.
class Basket : public Node {
public:
    explicit Basket(bool halved = false) : half(halved) {
        over_ = on<DragOver>([this](DragOver& what) {
            ++overs;

            if (dynamic_cast<Apple*>(what.carried.get()) == nullptr) return;

            const std::optional<Spot> here = local(what.screen);

            if (half && here && here->x < area().size.wide / 2) return;

            what.accept(*this);
        });
        dropped_ = on<Dropped>([this](Dropped& what) {
            got.push_back(dynamic_cast<Apple&>(*what.carried).name);
            at.push_back(what.at);
        });
        moused_ = on<tal::MouseEvent>([this](tal::MouseEvent&) { ++mice; });
    }

    bool half = false;
    int overs = 0;
    int mice = 0;
    std::vector<std::string> got;
    std::vector<Spot> at;

private:
    Hook over_;
    Hook dropped_;
    Hook moused_;
};

// A row of a tree and two baskets, each ten columns wide, on a screen of 30 by 4.
struct Orchard {
    Orchard() : document(scene.time()) {
        tal::Row& row = document.children().add<tal::Row>();

        tree = &row.children().add<Tree>("first");
        left = &row.children().add<Basket>();
        right = &row.children().add<Basket>(true);

        for (Node* one : {static_cast<Node*>(tree), static_cast<Node*>(left), static_cast<Node*>(right)}) one->share(tal::Share{1, 10});

        scene.show(document);
        scene.wait(0ms);
    }

    Scene scene{{.screen = {30, 4}}};
    Document document;
    Tree* tree = nullptr;
    Basket* left = nullptr;
    Basket* right = nullptr;
};

}

// Pressed on the tree and moved away, an apple is carried; over a basket it is taken there (the
// state `drag`), over the next one taken there instead, over the left half of the right basket by
// nobody; the release drops it where it was last taken, the spot counted from the basket. Nothing
// of the mouse reaches the tree while it is carried, and no click comes of it.
TRIAL("Dragging: carried over the nodes, taken, and dropped where taken") {
    Orchard here;
    Document& document = here.document;

    document.mouse(Press::down, MouseButton::left, Spot{2, 1});
    document.mouse(Press::move, MouseButton::left, Spot{3, 1});

    OUGHT(document.carried() == nullptr);

    document.mouse(Press::move, MouseButton::left, Spot{12, 2});

    OUGHT(document.carried() != nullptr);
    OUGHT(here.left->states().has(State::drag));
    SAME(here.left->overs, 1);

    document.mouse(Press::move, MouseButton::left, Spot{27, 2});

    OUGHT(!here.left->states().has(State::drag));
    OUGHT(here.right->states().has(State::drag));

    document.mouse(Press::wheel, MouseButton::none, Spot{27, 2}, {}, 1);
    document.key(Key{tal::Code::escape, tal::Mods{.shift = true}});

    OUGHT(document.carried() != nullptr);
    SAME(here.right->mice, 0);

    document.mouse(Press::move, MouseButton::left, Spot{21, 2});

    OUGHT(!here.right->states().has(State::drag));

    document.mouse(Press::move, MouseButton::left, Spot{15, 3});
    document.mouse(Press::up, MouseButton::left, Spot{15, 3});

    OUGHT(document.carried() == nullptr);
    SAME(here.left->got, (std::vector<std::string>{"first"}));
    OUGHT(here.left->at == (std::vector<Spot>{Spot{5, 3}}));
    OUGHT(!here.left->states().has(State::drag));
    OUGHT(here.right->got.empty());

    document.mouse(Press::move, MouseButton::left, Spot{2, 1});
    document.mouse(Press::down, MouseButton::left, Spot{2, 1});
    document.mouse(Press::up, MouseButton::left, Spot{2, 1});

    OUGHT(document.carried() == nullptr);
    SAME(here.left->got.size(), std::size_t{1});
}

// A release over nothing that takes the load, over a half that does not, Esc, or a new press whose
// release was lost: nothing is dropped and no basket keeps the state `drag`. A stone is taken by
// no basket. A basket taken away while it holds the load gets nothing.
TRIAL("Dragging: given up, refused, or the taker gone") {
    Orchard here;
    Document& document = here.document;

    here.tree->lift(std::make_shared<Apple>("a"));
    document.mouse(Press::move, MouseButton::left, Spot{22, 0});
    document.mouse(Press::up, MouseButton::left, Spot{22, 0});

    OUGHT(here.right->got.empty());

    here.tree->lift(std::make_shared<Apple>("b"));
    document.mouse(Press::move, MouseButton::left, Spot{12, 0});
    document.key(Key{tal::Code::escape});

    OUGHT(document.carried() == nullptr);
    OUGHT(!here.left->states().has(State::drag));

    document.mouse(Press::move, MouseButton::left, Spot{13, 0});
    document.mouse(Press::up, MouseButton::left, Spot{13, 0});

    OUGHT(here.left->got.empty());

    here.tree->lift(std::make_shared<Apple>("c"));
    document.mouse(Press::move, MouseButton::left, Spot{12, 0});
    document.mouse(Press::down, MouseButton::left, Spot{12, 0});

    OUGHT(document.carried() == nullptr);
    OUGHT(!here.left->states().has(State::drag));

    document.mouse(Press::up, MouseButton::left, Spot{12, 0});

    OUGHT(here.left->got.empty());

    here.tree->lift(std::make_shared<Stone>());
    document.mouse(Press::move, MouseButton::left, Spot{12, 0});

    OUGHT(!here.left->states().has(State::drag));

    document.mouse(Press::up, MouseButton::left, Spot{12, 0});

    OUGHT(here.left->got.empty());

    here.tree->lift(std::make_shared<Apple>("d"));
    document.mouse(Press::move, MouseButton::left, Spot{12, 0});

    const std::shared_ptr<Node> gone = here.left->parent()->children().remove(*here.left);

    document.mouse(Press::up, MouseButton::left, Spot{12, 0});

    OUGHT(document.carried() == nullptr);
    OUGHT(here.left->got.empty());
    OUGHT(!here.left->states().has(State::drag));
}

// The load hears once how each carrying of it ended: taken when dropped on a basket that took it,
// not taken when released over nothing that takes it, given up by Esc or by a new press, or when
// its taker went away; after the basket heard `Dropped`.
TRIAL("Dragging: the load hears how it landed") {
    Orchard here;
    Document& document = here.document;
    auto heard = std::make_shared<std::vector<bool>>();
    std::size_t got_before = 0;
    const Hook dropped = here.left->on<Dropped>([&](Dropped&) { got_before = heard->size(); });

    here.tree->lift(std::make_shared<Heeded>(heard));
    document.mouse(Press::move, MouseButton::left, Spot{12, 0});
    document.mouse(Press::up, MouseButton::left, Spot{12, 0});

    OUGHT(*heard == (std::vector<bool>{true}));
    SAME(got_before, std::size_t{0});

    here.tree->lift(std::make_shared<Heeded>(heard));
    document.mouse(Press::move, MouseButton::left, Spot{22, 0});
    document.mouse(Press::up, MouseButton::left, Spot{22, 0});

    here.tree->lift(std::make_shared<Heeded>(heard));
    document.mouse(Press::move, MouseButton::left, Spot{12, 0});
    document.key(Key{tal::Code::escape});

    here.tree->lift(std::make_shared<Heeded>(heard));
    document.mouse(Press::move, MouseButton::left, Spot{12, 0});
    document.mouse(Press::down, MouseButton::left, Spot{12, 0});
    document.mouse(Press::up, MouseButton::left, Spot{12, 0});

    here.tree->lift(std::make_shared<Heeded>(heard));
    document.mouse(Press::move, MouseButton::left, Spot{12, 0});

    const std::shared_ptr<Node> gone = here.left->parent()->children().remove(*here.left);

    document.mouse(Press::up, MouseButton::left, Spot{12, 0});

    OUGHT(*heard == (std::vector<bool>{true, false, false, false, false}));
}

// Without a document a node carries nothing; no load throws std::invalid_argument. A spot of the
// screen is counted from a node's corner (across and down), and not at all for a node not placed.
TRIAL("Dragging: nothing outside a document, no empty load; spots of the screen in a node") {
    Tree alone{"x"};
    Orchard here;

    alone.lift(std::make_shared<Apple>("x"));

    OUGHT(here.document.carried() == nullptr);

    bool threw = false;

    try {
        here.tree->lift(nullptr);
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    OUGHT(threw);
    OUGHT(here.right->local(Spot{25, 3}) == (std::optional<Spot>{Spot{5, 3}}));
    OUGHT(!alone.local(Spot{1, 1}).has_value());

    Scene scene{{.screen = {10, 4}}};
    Document document{scene.time()};
    tal::Column& column = document.children().add<tal::Column>();

    (void)column.children().add<Node>().share(tal::Share{1, 1});

    Node& low = column.children().add<Node>();

    low.share(tal::Share{1, 1});
    scene.show(document);
    scene.wait(0ms);

    OUGHT(low.local(Spot{4, 3}) == (std::optional<Spot>{Spot{4, 1}}));
}

// While the focus is confined (a modal window open), the load is over the confinement wherever the
// pointer is outside it: a basket outside does not take it.
TRIAL("Dragging: confined, over the confinement") {
    Orchard here;
    Document& document = here.document;

    document.confine(here.tree);
    here.tree->lift(std::make_shared<Apple>("e"));
    document.mouse(Press::move, MouseButton::left, Spot{12, 0});

    OUGHT(!here.left->states().has(State::drag));
    SAME(here.left->overs, 0);

    document.mouse(Press::up, MouseButton::left, Spot{12, 0});

    OUGHT(here.left->got.empty());
}
