#include "proving.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "talcite/root.hpp"

using namespace talcite;

namespace {

// A focusable node that records in `told` the keys and sizes that reach it, and counts itself in
// `alive` while it lives.
class Leaf : public ui::Node {
public:
    // A leaf named `named`, counted in `alive` when that is given.
    explicit Leaf(std::string named, std::vector<std::string>* told = nullptr,
                  int* alive = nullptr)
        : named_(std::move(named)), told_(told), alive_(alive) {
        if (alive_ != nullptr) ++*alive_;
    }

    // Every leaf can take the focus.
    [[nodiscard]] bool focusable() const override { return true; }

    // Any number of children, so that leaves can be nested.
    [[nodiscard]] int capacity() const override { return ui::plenty; }

    // Takes the leaf off the `alive` count.
    ~Leaf() override {
        if (alive_ != nullptr) --*alive_;
    }

    // Sends a pick from the leaf itself, as a widget does when the program presses it, outside any
    // delivery of the root.
    bool picked() { return trigger(ui::PickEvent{}); }

protected:
    // Records the leaf's name and lets the key go on.
    bool handle(ui::KeyEvent& what) override {
        (void)what;

        if (told_ != nullptr) told_->push_back(named_);

        return false;
    }

    // Records the leaf's name with the size suffix and lets the event go on.
    bool handle(ui::SizeEvent& what) override {
        (void)what;

        if (told_ != nullptr) told_->push_back(named_ + ":rozmiar");

        return false;
    }

private:
    std::string named_;
    std::vector<std::string>* told_ = nullptr;
    int* alive_ = nullptr;
};

// A root that records in `told` the keys and sizes that reach it.
class Crown : public ui::Root {
public:
    explicit Crown(std::vector<std::string>* told = nullptr) : told_(told) {}

    // Any number of children, so that several leaves can stand under the root.
    [[nodiscard]] int capacity() const override { return ui::plenty; }

protected:
    // Records the root and lets the key go on.
    bool handle(ui::KeyEvent& what) override {
        (void)what;

        if (told_ != nullptr) told_->push_back("korzeń");

        return false;
    }

    // Records the root with the size suffix and lets the event go on.
    bool handle(ui::SizeEvent& what) override {
        (void)what;

        if (told_ != nullptr) told_->push_back("korzeń:rozmiar");

        return false;
    }

private:
    std::vector<std::string>* told_ = nullptr;
};

// A root that sees keys first: it takes `z` itself, and on `x` removes the focused node from the
// tree (like a prefix that closes a view) and lets the key go on. On `y` it removes its first child
// and notes in `seen` how many of the counted nodes still live right after the removal.
class Seer : public Crown {
public:
    explicit Seer(std::vector<std::string>* told) : Crown(told), told_(told) {}

    std::shared_ptr<ui::Node> doomed;
    int* alive = nullptr;
    int seen = -1;

protected:
    // Removes `doomed` on `x` and lets the key go on; takes `z` and records that it foresaw it; on
    // `y` removes the first child and counts who still lives.
    bool foresee(ui::KeyEvent& what) override {
        if (what.key().letter() == U'y' && !children().empty()) {
            (void)children().remove(children().front().get());

            if (alive != nullptr) seen = *alive;

            return true;
        }

        if (what.key().letter() == U'x' && doomed) {
            (void)children().remove(doomed.get());
            doomed.reset();

            return false;
        }

        if (what.key().letter() != U'z') return false;

        told_->push_back("przewidział");

        return true;
    }

private:
    std::vector<std::string>* told_;
};

// A shared `Leaf`, with the same arguments as its constructor.
std::shared_ptr<Leaf> leaf(const std::string& named, std::vector<std::string>* told = nullptr,
                           int* alive = nullptr) {
    return std::make_shared<Leaf>(named, told, alive);
}

}

TRIAL("Root: a fresh root has no focus") {
    const Crown top;

    ought(top.focused_node() == nullptr);
}

TRIAL("Root: the root is its own root") {
    Crown top;

    ought(top.root() == &top);
}

TRIAL("Root: a node in the tree knows the root, outside the tree it does not") {
    Crown top;

    const std::shared_ptr<Leaf> mid = leaf("środek");
    const std::shared_ptr<Leaf> low = leaf("spód");

    ought(mid->root() == nullptr);

    top.children().add(mid);
    mid->children().add(low);

    ought(mid->root() == &top);
    ought(low->root() == &top);

    (void)top.children().remove(mid.get());

    ought(mid->root() == nullptr);
    ought(low->root() == nullptr);
}

TRIAL("Root: a tree without a root has no root") {
    ui::Node up;

    const std::shared_ptr<Leaf> down = leaf("dół");

    up.children().add(down);

    ought(down->root() == nullptr);
}

TRIAL("Root: the focus can be set and read from both sides") {
    Crown top;

    const std::shared_ptr<Leaf> one = leaf("jeden");

    top.children().add(one);

    ought(!one->focused());

    one->focus();

    ought(top.focused_node() == one.get());
    ought(one->focused());
}

TRIAL("Root: the focus can be cleared") {
    Crown top;

    const std::shared_ptr<Leaf> one = leaf("jeden");

    top.children().add(one);
    one->focus();

    top.move_focus(nullptr);

    ought(top.focused_node() == nullptr);
    ought(!one->focused());
}

TRIAL("Root: a node from another tree does not get the focus") {
    Crown first;
    Crown second;

    const std::shared_ptr<Leaf> one = leaf("jeden");

    first.children().add(one);

    threw(second.move_focus(one.get()));
    ought(second.focused_node() == nullptr);
}

TRIAL("Root: a node outside the tree does not take the focus") {
    const std::shared_ptr<Leaf> one = leaf("jeden");

    threw(one->focus());
    ought(!one->focused());
}

TRIAL("Root: a key goes to the focus and travels up") {
    std::vector<std::string> told;

    Crown top{&told};

    const std::shared_ptr<Leaf> mid = leaf("środek", &told);
    const std::shared_ptr<Leaf> low = leaf("spód", &told);

    top.children().add(mid);
    mid->children().add(low);

    low->focus();

    ui::KeyEvent what{U'a'};

    ought(!top.dispatch(what));
    same(told.size(), std::size_t{3});
    same(told[0], std::string{"spód"});
    same(told[1], std::string{"środek"});
    same(told[2], std::string{"korzeń"});
    ought(what.target() == low.get());
}

// `foresee` sees the key before the focus: taken, it goes no further; let go, it travels the usual
// way.
TRIAL("Root: foresee sees the key first") {
    std::vector<std::string> told;

    Seer top{&told};

    const std::shared_ptr<Leaf> low = leaf("spód", &told);

    top.children().add(low);
    low->focus();

    ui::KeyEvent taken{U'z'};

    ought(top.dispatch(taken));
    same(told.size(), std::size_t{1});
    same(told[0], std::string{"przewidział"});

    told.clear();

    ui::KeyEvent passed{U'a'};

    ought(!top.dispatch(passed));
    same(told.size(), std::size_t{2});
    same(told[0], std::string{"spód"});
}

// The foreseer may remove the focused node; the key then goes to the root, not to the removed one.
TRIAL("Root: foresee may remove the focus, the key then goes to the root") {
    std::vector<std::string> told;

    Seer top{&told};

    const std::shared_ptr<Leaf> low = leaf("spód", &told);

    top.children().add(low);
    low->focus();

    top.doomed = low;

    ui::KeyEvent what{U'x'};

    ought(!top.dispatch(what));
    same(told.size(), std::size_t{1});
    same(told[0], std::string{"korzeń"});
    ought(top.focused_node() == nullptr);
}

// `foresee` runs inside the key's delivery, as the focus would: a node it takes off the tree lives
// until the key is over, so the foreseer may still reach it.
TRIAL("Root: a node foresee removes lives until the key is over") {
    std::vector<std::string> told;
    int alive = 0;

    Seer top{&told};

    top.alive = &alive;

    top.children().add(leaf("spód", nullptr, &alive));

    ui::KeyEvent what{U'y'};

    ought(top.dispatch(what));
    same(top.seen, 1);
    same(alive, 0);
}

TRIAL("Root: a key without focus goes to the root") {
    std::vector<std::string> told;

    Crown top{&told};

    top.children().add(leaf("jeden", &told));

    ui::KeyEvent what{U'a'};

    ought(!top.dispatch(what));
    same(told.size(), std::size_t{1});
    same(told[0], std::string{"korzeń"});
    ought(what.target() == &top);
}

TRIAL("Root: a size always goes to the root, even with a focus") {
    std::vector<std::string> told;

    Crown top{&told};

    const std::shared_ptr<Leaf> one = leaf("jeden", &told);

    top.children().add(one);
    one->focus();

    ui::SizeEvent what{80, 25};

    ought(!top.dispatch(what));
    same(told.size(), std::size_t{1});
    same(told[0], std::string{"korzeń:rozmiar"});
}

TRIAL("Root: dispatch returns what the walk said") {
    Crown top;

    const std::shared_ptr<Leaf> one = leaf("jeden");

    top.children().add(one);
    one->focus();

    const ui::Hook heard_key = one->on<ui::KeyEvent>([](ui::KeyEvent&) { return true; });

    ui::KeyEvent what{U'a'};

    ought(top.dispatch(what));
}

TRIAL("Root: removing the focused node clears the focus") {
    Crown top;

    const std::shared_ptr<Leaf> one = leaf("jeden");

    top.children().add(one);
    one->focus();

    (void)top.children().remove(one.get());

    ought(top.focused_node() == nullptr);
}

TRIAL("Root: removing an ancestor of the focused node clears the focus too") {
    Crown top;

    const std::shared_ptr<Leaf> mid = leaf("środek");
    const std::shared_ptr<Leaf> low = leaf("spód");

    top.children().add(mid);
    mid->children().add(low);
    low->focus();

    (void)top.children().remove(mid.get());

    ought(top.focused_node() == nullptr);
}

TRIAL("Root: removing a node next to the focus clears nothing") {
    Crown top;

    const std::shared_ptr<Leaf> one = leaf("jeden");
    const std::shared_ptr<Leaf> two = leaf("dwa");

    top.children().add(one);
    top.children().add(two);
    one->focus();

    (void)top.children().remove(two.get());

    ought(top.focused_node() == one.get());
}

TRIAL("Root: removing outside delivery frees at once") {
    int alive = 0;

    Crown top;

    top.children().add(leaf("jeden", nullptr, &alive));

    same(alive, 1);

    (void)top.children().remove(top.children().front().get());

    same(alive, 0);
}

TRIAL("Root: a node closing during its own reaction does not break the walk") {
    std::vector<std::string> told;
    int alive = 0;

    Crown top{&told};

    const std::shared_ptr<Leaf> mid = leaf("środek", &told, &alive);

    top.children().add(mid);

    const std::shared_ptr<Leaf> low = leaf("spód", &told, &alive);

    mid->children().add(low);
    low->focus();

    Leaf* stale = mid.get();

    const ui::Hook heard_key = low->on<ui::KeyEvent>([&top, stale](ui::KeyEvent&) {
        (void)top.children().remove(stale);

        return false;
    });

    same(alive, 2);

    ui::KeyEvent what{U'a'};

    ought(!top.dispatch(what));

    // The handler removed the branch the walk was going along. The walk still reached the end along
    // the old route, and the nodes lived until delivery ended.
    same(told.size(), std::size_t{2});
    same(told[0], std::string{"spód"});
    same(told[1], std::string{"środek"});
}

TRIAL("Root: a deferred release happens after delivery") {
    int alive = 0;

    Crown top;

    const std::shared_ptr<Leaf> mid = leaf("środek", nullptr, &alive);

    top.children().add(mid);

    Leaf* stale = mid.get();

    const ui::Hook heard_key = top.on<ui::KeyEvent>([&top, stale](ui::KeyEvent&) {
        (void)top.children().remove(stale);

        return false;
    });

    same(alive, 1);

    ui::KeyEvent what{U'a'};

    ought(!top.dispatch(what));

    same(alive, 1);
}

TRIAL("Root: a deferred release really releases") {
    int alive = 0;

    Crown top;

    {
        const std::shared_ptr<Leaf> mid = leaf("środek", nullptr, &alive);

        top.children().add(mid);
    }

    Leaf* stale = static_cast<Leaf*>(top.children().front().get());

    const ui::Hook heard_key = top.on<ui::KeyEvent>([&top, stale](ui::KeyEvent&) {
        (void)top.children().remove(stale);

        return false;
    });

    same(alive, 1);

    ui::KeyEvent what{U'a'};

    ought(!top.dispatch(what));

    same(alive, 0);
}

// Moving the focus sends two events in one delivery: a listener of the first that takes the new
// focus off the tree leaves it alive for the second, which reaches a living node; it goes only when
// the move is over. Before, it was freed between the two, and the second reached freed memory.
TRIAL("Root: moving the focus is one delivery, so the new focus lives through both events") {
    int alive = 0;

    Crown top;

    const std::shared_ptr<Leaf> first = leaf("pierwszy");

    top.children().add(first);

    {
        const std::shared_ptr<Leaf> second = leaf("drugi", nullptr, &alive);

        top.children().add(second);
    }

    Leaf* next = static_cast<Leaf*>(top.children()[1].get());

    first->focus();

    const ui::Hook heard_leaving = first->on<ui::UnfocusEvent>([&top, next](ui::UnfocusEvent&) {
        (void)top.children().remove(next);

        return false;
    });

    int seen = -1;

    const ui::Hook heard_coming = next->on<ui::FocusEvent>([&seen, &alive](ui::FocusEvent&) {
        seen = alive;

        return false;
    });

    next->focus();

    same(seen, 1);
    same(alive, 0);
    ought(top.focused_node() == nullptr);
}

// An event a node starts itself (a button pressed by the program, not by a key) is a delivery of
// its root too: a listener that removes the branch it climbs through, as an OK closing its window
// does, leaves the nodes alive until the event is over, the climb goes on over them, and they go
// only then. Before, only the root's own deliveries counted, and the climb went on over freed nodes
// (ThreadSanitizer in twa: the genres window's OK).
TRIAL("Root: an event a node starts itself is a delivery, so its branch lives until it ends") {
    std::vector<std::string> heard;
    int alive = 0;

    Crown top;

    {
        const std::shared_ptr<Leaf> mid = leaf("środek", nullptr, &alive);
        const std::shared_ptr<Leaf> low = leaf("spód", nullptr, &alive);

        top.children().add(mid);
        mid->children().add(low);
    }

    Leaf* const window = static_cast<Leaf*>(top.children().front().get());
    Leaf* const button = static_cast<Leaf*>(window->children().front().get());

    const ui::Hook closing = button->on<ui::PickEvent>([&top, &alive, &heard, window](ui::PickEvent&) {
        (void)top.children().remove(window);

        heard.push_back("spód:" + std::to_string(alive));

        return false;
    });

    const ui::Hook above = window->on<ui::PickEvent>([&alive, &heard](ui::PickEvent&) {
        heard.push_back("środek:" + std::to_string(alive));

        return false;
    });

    same(alive, 2);

    ought(!button->picked());

    same(heard, (std::vector<std::string>{"spód:2", "środek:2"}));
    same(alive, 0);
}

// The same inside the root's own delivery: the inner event changes nothing, the branch goes when
// the outer one ends.
TRIAL("Root: an event a node starts inside a delivery of the root is part of it") {
    int alive = 0;

    Crown top;

    {
        const std::shared_ptr<Leaf> mid = leaf("środek", nullptr, &alive);
        const std::shared_ptr<Leaf> low = leaf("spód", nullptr, &alive);

        top.children().add(mid);
        mid->children().add(low);
        low->focus();
    }

    Leaf* const window = static_cast<Leaf*>(top.children().front().get());
    Leaf* const button = static_cast<Leaf*>(window->children().front().get());

    int during = -1;

    const ui::Hook closing = button->on<ui::PickEvent>([&top, window](ui::PickEvent&) {
        (void)top.children().remove(window);

        return true;
    });

    const ui::Hook pressing = button->on<ui::KeyEvent>([&alive, &during, button](ui::KeyEvent&) {
        (void)button->picked();

        during = alive;

        return true;
    });

    ui::KeyEvent what{U'a'};

    ought(top.dispatch(what));

    same(during, 2);
    same(alive, 0);
}

TRIAL("Root: an exception from a handler does not leave the root in the delivery state") {
    int alive = 0;

    Crown top;

    {
        const std::shared_ptr<Leaf> mid = leaf("środek", nullptr, &alive);

        top.children().add(mid);
    }

    Leaf* stale = static_cast<Leaf*>(top.children().front().get());

    {
        const ui::Hook which = top.on<ui::KeyEvent>([](ui::KeyEvent&) -> bool {
            throw std::runtime_error("the handler fell over");
        });

        ui::KeyEvent what{U'a'};

        threw((void)top.dispatch(what));
    }

    // If the exception left the delivery counter raised, the removal would be deferred instead of
    // freeing at once, and this is the only way to see that from outside.
    same(alive, 1);

    (void)top.children().remove(stale);

    same(alive, 0);
}

namespace {

// A node that records what happened to it with the focus, and who was on the other side.
class Watched : public ui::Node {
public:
    // A watched node named `named` that records its focus changes in `told`.
    explicit Watched(std::string named, std::vector<std::string>* told)
        : named_(std::move(named)), told_(told) {}

    // Every watched node can take the focus.
    [[nodiscard]] bool focusable() const override { return true; }

    // Any number of children, so that watched nodes can be nested.
    [[nodiscard]] int capacity() const override { return ui::plenty; }

    // The other side of the last focus change, or null.
    [[nodiscard]] const ui::Node* other() const { return other_; }

    // Whether the node had the focus at the time of the last focus event.
    [[nodiscard]] bool held() const { return held_; }

protected:
    // Records gaining the focus, who lost it, and whether `focused()` already says so.
    bool handle(ui::FocusEvent& what) override {
        told_->push_back(named_ + ":objął");
        other_ = what.other();
        held_ = focused();

        return false;
    }

    // Records losing the focus, who gained it, and whether `focused()` already says so.
    bool handle(ui::UnfocusEvent& what) override {
        told_->push_back(named_ + ":stracił");
        other_ = what.other();
        held_ = focused();

        return false;
    }

private:
    std::string named_;
    std::vector<std::string>* told_ = nullptr;
    const ui::Node* other_ = nullptr;
    bool held_ = false;
};

}

TRIAL("Root: taking the focus sends an event") {
    std::vector<std::string> told;

    Crown top;

    const auto one = std::make_shared<Watched>("jeden", &told);

    top.children().add(one);
    one->focus();

    same(told.size(), std::size_t{1});
    same(told[0], std::string{"jeden:objął"});
    ought(one->other() == nullptr);
}

TRIAL("Root: moving the focus sends the loss first, then the gain") {
    std::vector<std::string> told;

    Crown top;

    const auto one = std::make_shared<Watched>("jeden", &told);
    const auto two = std::make_shared<Watched>("dwa", &told);

    top.children().add(one);
    top.children().add(two);

    one->focus();
    told.clear();

    two->focus();

    same(told.size(), std::size_t{2});
    same(told[0], std::string{"jeden:stracił"});
    same(told[1], std::string{"dwa:objął"});
}

TRIAL("Root: focus events carry the other side of the change") {
    std::vector<std::string> told;

    Crown top;

    const auto one = std::make_shared<Watched>("jeden", &told);
    const auto two = std::make_shared<Watched>("dwa", &told);

    top.children().add(one);
    top.children().add(two);

    one->focus();
    two->focus();

    ought(one->other() == two.get());
    ought(two->other() == one.get());
}

TRIAL("Root: in both events the focus is already moved") {
    std::vector<std::string> told;

    Crown top;

    const auto one = std::make_shared<Watched>("jeden", &told);
    const auto two = std::make_shared<Watched>("dwa", &told);

    top.children().add(one);
    top.children().add(two);

    one->focus();
    two->focus();

    // The one losing it asks focused() and must already hear "no", the one gaining it "yes".
    ought(!one->held());
    ought(two->held());
}

TRIAL("Root: clearing the focus sends only the loss") {
    std::vector<std::string> told;

    Crown top;

    const auto one = std::make_shared<Watched>("jeden", &told);

    top.children().add(one);
    one->focus();
    told.clear();

    top.move_focus(nullptr);

    same(told.size(), std::size_t{1});
    same(told[0], std::string{"jeden:stracił"});
    ought(one->other() == nullptr);
}

TRIAL("Root: setting the same focus sends nothing") {
    std::vector<std::string> told;

    Crown top;

    const auto one = std::make_shared<Watched>("jeden", &told);

    top.children().add(one);
    one->focus();
    told.clear();

    one->focus();
    top.move_focus(one.get());

    same(told.size(), std::size_t{0});
}

TRIAL("Root: a focus event travels up like any other") {
    std::vector<std::string> told;

    Crown top;

    const auto mid = std::make_shared<Watched>("środek", &told);
    const auto low = std::make_shared<Watched>("spód", &told);

    top.children().add(mid);
    mid->children().add(low);

    bool seen = false;

    const ui::Hook heard_focus = mid->on<ui::FocusEvent>([&seen](ui::FocusEvent&) {
        seen = true;

        return false;
    });

    low->focus();

    ought(seen);
}
