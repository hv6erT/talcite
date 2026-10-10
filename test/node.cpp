#include "proving.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "talcite/node.hpp"

using namespace talcite;

namespace {

// A node with a countable lifetime and with access to what is protected in Node. That is enough to
// check the tree and how events travel through it.
class Twig : public ui::Node {
public:
    // A node named `named` that records its name in `told` whenever a key reaches it, and counts
    // itself in `alive` while it lives.
    explicit Twig(std::string named, std::vector<std::string>* told = nullptr,
                  int* alive = nullptr)
        : named_(std::move(named)), told_(told), alive_(alive) {
        if (alive_ != nullptr) ++*alive_;
    }

    // Takes the node off the `alive` count.
    ~Twig() override {
        if (alive_ != nullptr) --*alive_;
    }

    using ui::Node::broadcast;

    // Sends the event from this node up the tree; true when someone took it.
    template <typename One>
    bool send(One& what) {
        return trigger(what);
    }

    // How many children the node has.
    [[nodiscard]] std::size_t much() const { return children().many(); }

    // The first child, or null when there are none.
    [[nodiscard]] const ui::Node* first() const {
        return children().empty() ? nullptr : children().front().get();
    }

    // Sets whether the node takes the keys that reach it.
    void eats(bool given) { eats_ = given; }

    // Any number of children, unlike a plain node.
    [[nodiscard]] int capacity() const override { return ui::plenty; }

protected:
    // Records the node's name for the key and takes it when set to by `eats`.
    bool handle(ui::KeyEvent& what) override {
        (void)what;

        if (told_ != nullptr) told_->push_back(named_);

        return eats_;
    }

private:
    std::string named_;
    std::vector<std::string>* told_ = nullptr;
    int* alive_ = nullptr;
    bool eats_ = false;
};

// A shared `Twig`, with the same arguments as its constructor.
std::shared_ptr<Twig> twig(const std::string& named, std::vector<std::string>* told = nullptr,
                           int* alive = nullptr) {
    return std::make_shared<Twig>(named, told, alive);
}

}

TRIAL("Node: an attached child knows its parent") {
    Twig up{"góra"};

    const std::shared_ptr<Twig> down = twig("dół");

    up.children().add(down);

    ought(down->parent() == &up);
    same(up.much(), std::size_t{1});
    ought(up.first() == down.get());
}

// `find` gives the number of a child among the children, and nothing for a node that is not a
// child, also after it is detached.
TRIAL("Node: find knows a child's number and not a stranger's") {
    Twig top{"góra"};

    const std::shared_ptr<Twig> first = twig("pierwsze");
    const std::shared_ptr<Twig> second = twig("drugie");
    const std::shared_ptr<Twig> stranger = twig("obce");

    top.children().add(first);
    top.children().add(second);

    same(top.children().find(*first), std::optional<std::size_t>{0});
    same(top.children().find(*second), std::optional<std::size_t>{1});
    ought(!top.children().find(*stranger).has_value());
    ought(!top.children().find(top).has_value());

    (void)top.children().remove(first.get());

    ought(!top.children().find(*first).has_value());
    same(top.children().find(*second), std::optional<std::size_t>{0});
}

TRIAL("Node: a fresh node has no parent and no children") {
    const Twig one{"sam"};

    ought(one.parent() == nullptr);
    same(one.much(), std::size_t{0});
}

TRIAL("Node: attaching nothing is refused") {
    Twig up{"góra"};

    threw(up.children().add(nullptr));
}

TRIAL("Node: a node with a parent cannot be attached a second time") {
    Twig first{"pierwszy"};
    Twig second{"drugi"};

    const std::shared_ptr<Twig> down = twig("dół");

    first.children().add(down);

    threw(second.children().add(down));
    ought(down->parent() == &first);
    same(second.much(), std::size_t{0});
}

TRIAL("Node: a node cannot be its own child") {
    const std::shared_ptr<Twig> one = twig("sam");

    threw(one->children().add(one));
}

TRIAL("Node: a node cannot get its own ancestor") {
    const std::shared_ptr<Twig> top = twig("szczyt");
    const std::shared_ptr<Twig> mid = twig("środek");
    const std::shared_ptr<Twig> low = twig("spód");

    top->children().add(mid);
    mid->children().add(low);

    threw(low->children().add(top));
    threw(low->children().add(mid));
}

TRIAL("Node: remove gives back the child and breaks its pointer to the parent") {
    Twig up{"góra"};

    const std::shared_ptr<Twig> down = twig("dół");

    up.children().add(down);

    const std::shared_ptr<ui::Node> taken = up.children().remove(down.get());

    ought(taken == down);
    ought(down->parent() == nullptr);
    same(up.much(), std::size_t{0});
}

TRIAL("Node: remove of another's node gives nothing") {
    Twig up{"góra"};
    Twig apart{"obok"};

    const std::shared_ptr<Twig> down = twig("dół");

    apart.children().add(down);

    ought(up.children().remove(down.get()) == nullptr);
    ought(up.children().remove(nullptr) == nullptr);
    ought(down->parent() == &apart);
}

// --- Children in bulk -----------------------------------------------------------------------------

// Putting a window together usually means several children in a row, so they go in with one call,
// and the order stays as given.
TRIAL("Node: a bulk attach lets all in and keeps the order") {
    Twig up{"góra"};

    const std::shared_ptr<Twig> one = twig("raz");
    const std::shared_ptr<Twig> two = twig("dwa");
    const std::shared_ptr<Twig> three = twig("trzy");

    up.children().add(std::vector<std::shared_ptr<ui::Node>>{one, two, three});

    same(up.much(), std::size_t{3});
    ought(up.children()[0].get() == one.get());
    ought(up.children()[1].get() == two.get());
    ought(up.children()[2].get() == three.get());
}

// All or none. A plain node accepts one child, so the second of a pair is refused, and then the
// first must leave too, instead of leaving the caller a half-built tree.
TRIAL("Node: a bulk attach that fails leaves no half behind") {
    ui::Node up;

    const std::shared_ptr<Twig> one = twig("raz");
    const std::shared_ptr<Twig> two = twig("dwa");

    threw(up.children().add(std::vector<std::shared_ptr<ui::Node>>{one, two}));

    same(up.children().many(), std::size_t{0});
    ought(one->parent() == nullptr);
    ought(two->parent() == nullptr);
}

TRIAL("Node: clear removes all children at once") {
    Twig up{"góra"};

    const std::shared_ptr<Twig> one = twig("raz");
    const std::shared_ptr<Twig> two = twig("dwa");

    up.children().add(one);
    up.children().add(two);

    up.children().clear();

    ought(up.children().empty());
    ought(one->parent() == nullptr);
    ought(two->parent() == nullptr);
}

// A removed child must die just as after a single `remove`, that is when nobody holds it anymore.
TRIAL("Node: clear lets the children go instead of holding on to them") {
    int alive = 0;

    Twig up{"góra"};

    up.children().add(twig("raz", nullptr, &alive));
    up.children().add(twig("dwa", nullptr, &alive));

    same(alive, 2);

    up.children().clear();

    same(alive, 0);
}

TRIAL("Node: contains recognizes its own child and not another's") {
    Twig up{"góra"};
    Twig apart{"obok"};

    const std::shared_ptr<Twig> mine = twig("moje");
    const std::shared_ptr<Twig> theirs = twig("cudze");

    up.children().add(mine);
    apart.children().add(theirs);

    ought(up.children().contains(mine.get()));
    ought(!up.children().contains(theirs.get()));
    ought(!up.children().contains(nullptr));
}

// --- Broadcasting down ---------------------------------------------------------------------------

TRIAL("Node: a broadcast goes down in order and ends at the first that takes it") {
    Twig up{"góra"};

    const std::shared_ptr<Twig> one = twig("raz");
    const std::shared_ptr<Twig> two = twig("dwa");
    const std::shared_ptr<Twig> three = twig("trzy");

    up.children().add(one);
    up.children().add(two);
    up.children().add(three);

    std::vector<std::string> said;

    for (const auto& [named, which] :
         {std::pair{std::string{"raz"}, one}, std::pair{std::string{"dwa"}, two},
          std::pair{std::string{"trzy"}, three}})
        (void)which->on<ui::ShortcutEvent>([&said, named](ui::ShortcutEvent&) {
            said.push_back(named);

            return named == "dwa";
        });

    ui::KeyEvent what{U'x'};

    ought(up.broadcast(what));

    same(static_cast<int>(said.size()), 2);
    same(said[0], std::string{"raz"});
    same(said[1], std::string{"dwa"});
}

// A handler may rebuild the tree during a broadcast. Walking the live vector would be a memory
// violation here: attaching moves the buffer while the walk goes on at the old address. The walk
// goes over a snapshot, so the move has nothing to affect.
TRIAL("Node: a broadcast survives the tree being rebuilt in a handler") {
    Twig up{"góra"};

    const std::shared_ptr<Twig> one = twig("raz");
    const std::shared_ptr<Twig> two = twig("dwa");

    up.children().add(one);
    up.children().add(two);

    (void)one->on<ui::ShortcutEvent>([&up](ui::ShortcutEvent&) {
        for (int at = 0; at < 64; ++at) up.children().add(twig("dokładka"));

        return false;
    });

    int done = 0;

    (void)two->on<ui::ShortcutEvent>([&done](ui::ShortcutEvent&) {
        ++done;

        return true;
    });

    ui::KeyEvent what{U'x'};

    ought(up.broadcast(what));

    same(done, 1);
    same(up.much(), std::size_t{66});
}

// The same from the other side: a handler that removes its siblings must not break the walk either.
TRIAL("Node: a broadcast survives the tree being emptied in a handler") {
    Twig up{"góra"};

    const std::shared_ptr<Twig> one = twig("raz");
    const std::shared_ptr<Twig> two = twig("dwa");

    up.children().add(one);
    up.children().add(two);

    (void)one->on<ui::ShortcutEvent>([&up](ui::ShortcutEvent&) {
        up.children().clear();

        return false;
    });

    int done = 0;

    (void)two->on<ui::ShortcutEvent>([&done](ui::ShortcutEvent&) {
        ++done;

        return true;
    });

    ui::KeyEvent what{U'x'};

    // The removed siblings get the event anyway, because the snapshot keeps them alive; the
    // handlers in `Handler` behave the same way.
    ought(up.broadcast(what));

    same(done, 1);
    ought(up.children().empty());
}

TRIAL("Node: moving under another parent in one expression") {
    Twig first{"pierwszy"};
    Twig second{"drugi"};

    const std::shared_ptr<Twig> down = twig("dół");

    first.children().add(down);
    second.children().add(first.children().remove(down.get()));

    ought(down->parent() == &second);
    same(first.much(), std::size_t{0});
    same(second.much(), std::size_t{1});
}

TRIAL("Node: a removed child dies when nobody holds it") {
    int alive = 0;

    {
        Twig up{"góra"};

        up.children().add(twig("dół", nullptr, &alive));

        same(alive, 1);

        (void)up.children().remove(const_cast<ui::Node*>(up.first()));

        same(alive, 0);
    }
}

TRIAL("Node: the parent takes its children with it") {
    int alive = 0;

    {
        Twig up{"góra"};

        up.children().add(twig("jeden", nullptr, &alive));
        up.children().add(twig("dwa", nullptr, &alive));

        same(alive, 2);
    }

    same(alive, 0);
}

TRIAL("Node: a child outliving its parent is not left with a dangling pointer") {
    std::shared_ptr<Twig> down = twig("dół");

    {
        Twig up{"góra"};

        up.children().add(down);

        ought(down->parent() == &up);
    }

    ought(down->parent() == nullptr);
}

TRIAL("Node: the whole subtree dies together with the root of the branch") {
    int alive = 0;

    {
        Twig up{"góra"};

        const std::shared_ptr<Twig> mid = twig("środek", nullptr, &alive);

        mid->children().add(twig("spód", nullptr, &alive));

        up.children().add(mid);

        same(alive, 2);
    }

    same(alive, 0);
}

TRIAL("Node: an event travels up the tree") {
    std::vector<std::string> told;

    const std::shared_ptr<Twig> top = twig("szczyt", &told);
    const std::shared_ptr<Twig> mid = twig("środek", &told);
    const std::shared_ptr<Twig> low = twig("spód", &told);

    top->children().add(mid);
    mid->children().add(low);

    ui::KeyEvent what{U'a'};

    ought(!low->send(what));
    same(told.size(), std::size_t{3});
    same(told[0], std::string{"spód"});
    same(told[2], std::string{"szczyt"});
    ought(what.target() == low.get());
}

TRIAL("Node: a detached branch does not pass events to its former parent") {
    std::vector<std::string> told;

    const std::shared_ptr<Twig> top = twig("szczyt", &told);
    const std::shared_ptr<Twig> mid = twig("środek", &told);

    top->children().add(mid);

    (void)top->children().remove(mid.get());

    ui::KeyEvent what{U'a'};

    ought(!mid->send(what));
    same(told.size(), std::size_t{1});
    same(told[0], std::string{"środek"});
}

TRIAL("Node: a handler attached to the parent catches an event from the child") {
    std::vector<std::string> told;

    const std::shared_ptr<Twig> up = twig("góra", &told);
    const std::shared_ptr<Twig> down = twig("dół", &told);

    up->children().add(down);

    const ui::Handler* seen = nullptr;

    (void)up->on<ui::KeyEvent>([&seen](ui::KeyEvent& what) {
        seen = what.target();

        return true;
    });

    ui::KeyEvent what{U'a'};

    ought(down->send(what));
    ought(seen == down.get());
}

TRIAL("Node: a fresh node has no rectangle") {
    const Twig one{"sam"};

    same(one.area().at.x, 0);
    same(one.area().at.y, 0);
    same(one.area().size.wide, 0);
    same(one.area().size.high, 0);
}

// Reaching past the end is the caller's error, not a state to keep quiet about; the same rule as in
// the key set and in the option set.
TRIAL("Children: reaching past the end throws") {
    const std::shared_ptr<Twig> up = twig("góra");

    threw((void)up->children().front());
    threw((void)up->children().back());
    threw((void)up->children()[0]);

    up->children().add(twig("dół"));

    same(up->children()[0].get(), up->children().front().get());
    same(up->children().front().get(), up->children().back().get());

    threw((void)up->children()[1]);
}
