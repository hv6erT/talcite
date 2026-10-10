#include "proving.hpp"

#include <string>
#include <vector>

#include "talcite/handler.hpp"

using namespace talcite;

namespace {

// The simplest possible node: it can have a parent and can let an event in. Everything `Handler`
// needs for the journey, and nothing more. Each `handle` it receives is written down as
// "name:kind".
class Knot : public ui::Handler {
public:
    explicit Knot(std::vector<std::string>& told, std::string named)
        : told_(told), named_(std::move(named)) {}

    // Hangs this node under the given parent.
    void under(Knot* one) { up_ = one; }

    // The parent set by `under`, or none.
    [[nodiscard]] ui::Handler* parent() const override { return up_; }

    // Sets whether `handle` takes the events it receives.
    void takes(bool given) { takes_ = given; }

    // Starts the event's journey at this node.
    template <typename One>
    bool send(One& what) {
        return trigger(what);
    }

    using ui::Handler::trigger;

protected:
    // Records the key event and takes it if told to.
    bool handle(ui::KeyEvent& what) override {
        (void)what;

        told_.push_back(named_ + ":handle");

        return takes_;
    }

    // Records the mouse event and takes it if told to.
    bool handle(ui::MouseEvent& what) override {
        (void)what;

        told_.push_back(named_ + ":mysz");

        return takes_;
    }

private:
    std::vector<std::string>& told_;
    std::string named_;
    Knot* up_ = nullptr;
    bool takes_ = false;
};

}

TRIAL("Handler: the target's handle receives the event") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};
    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
    same(told.size(), std::size_t{1});
    same(told[0], std::string{"jeden:handle"});
}

TRIAL("Handler: an unhandled event goes to the parent") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);

    ui::KeyEvent what{U'a'};

    ought(!down.send(what));
    same(told.size(), std::size_t{2});
    same(told[0], std::string{"dół:handle"});
    same(told[1], std::string{"góra:handle"});
}

TRIAL("Handler: a handled event goes no higher") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);
    down.takes(true);

    ui::KeyEvent what{U'a'};

    ought(down.send(what));
    same(told.size(), std::size_t{1});
    same(told[0], std::string{"dół:handle"});
}

TRIAL("Handler: the target is the first node, not the last") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);
    up.takes(true);

    ui::KeyEvent what{U'a'};

    ought(down.send(what));
    ought(what.target() == &down);
}

TRIAL("Handler: a hook from on receives the event when handle did not take it") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    (void)one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("uchwyt");

        return false;
    });

    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
    same(told.size(), std::size_t{2});
    same(told[0], std::string{"jeden:handle"});
    same(told[1], std::string{"uchwyt"});
}

// Two channels, two independent stops. The widget and the caller travel the same way, but each
// closes only its own path: true from `handle` says "no more reacting upward", true from a hook
// says "no more hooks upward", and neither says anything about the other channel. So a widget never
// hides from the caller an event the caller has every right to see.
TRIAL("Handler: a handle that took the event does not silence the hooks") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    one.takes(true);

    (void)one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("uchwyt");

        return false;
    });

    ui::KeyEvent what{U'a'};

    ought(one.send(what));
    same(told.size(), std::size_t{2});
    same(told[0], std::string{"jeden:handle"});
    same(told[1], std::string{"uchwyt"});
}

TRIAL("Handler: all hooks run, even when the first one took the event") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    (void)one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("pierwszy");

        return true;
    });

    (void)one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("drugi");

        return false;
    });

    ui::KeyEvent what{U'a'};

    ought(one.send(what));
    same(told.size(), std::size_t{3});
    same(told[1], std::string{"pierwszy"});
    same(told[2], std::string{"drugi"});
}

// True from a hook closes the path of hooks, not the whole journey: the ancestor's `handle` still
// speaks. This way a caller's hook never blinds a widget standing above it, so a message's button
// cannot swallow its own press.
TRIAL("Handler: a hook that took the event stops only its own channel") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);

    int seen = 0;

    (void)down.on<ui::KeyEvent>([](ui::KeyEvent&) { return true; });

    (void)up.on<ui::KeyEvent>([&seen](ui::KeyEvent&) {
        ++seen;

        return true;
    });

    ui::KeyEvent what{U'a'};

    ought(down.send(what));
    same(told.size(), std::size_t{2});
    same(told[0], std::string{"dół:handle"});
    same(told[1], std::string{"góra:handle"});

    // The hook at the top no longer runs: its channel was closed at the bottom.
    same(seen, 0);
}

TRIAL("Handler: hooks of another kind stay silent") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    (void)one.on<ui::MouseEvent>([&told](ui::MouseEvent&) {
        told.push_back("mysz");

        return true;
    });

    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
    same(told.size(), std::size_t{1});
    same(told[0], std::string{"jeden:handle"});
}

TRIAL("Handler: off removes a hook") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    ui::Hook which = one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("uchwyt");

        return true;
    });

    one.off(which);

    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
    same(told.size(), std::size_t{1});
}

// A hook knows its type and whether something hangs on it. Mixing up the type when detaching is
// impossible; a hook is empty when fresh and when detached, and only `on` gives a non-empty one.
TRIAL("Handler: a hook is empty when fresh and after detaching, and full after attaching") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    const ui::Hook fresh;

    ought(fresh.empty());

    ui::Hook which = one.on<ui::KeyEvent>([](ui::KeyEvent&) { return true; });

    ought(!which.empty());

    ui::Hook copy = which;

    one.off(which);

    ought(which.empty());

    // The copy points to what is already detached: detaching it does nothing and breaks nothing.
    ought(!copy.empty());

    one.off(copy);

    ought(copy.empty());

    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
}

// A hook with the same number on a different type does not remove someone else's: the type is in
// the hook, not in the call, so exactly what was attached is detached.
TRIAL("Handler: a hook detaches only its own type") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    ui::Hook mouse = one.on<ui::MouseEvent>([](ui::MouseEvent&) { return true; });

    (void)one.on<ui::KeyEvent>([](ui::KeyEvent&) { return true; });

    one.off(mouse);

    ui::KeyEvent what{U'a'};

    ought(one.send(what));
}

TRIAL("Handler: off removes one hook, not all") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    ui::Hook first = one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("pierwszy");

        return false;
    });

    (void)one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("drugi");

        return false;
    });

    one.off(first);

    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
    same(told.size(), std::size_t{2});
    same(told[1], std::string{"drugi"});
}

TRIAL("Handler: off on an empty hook breaks nothing") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    ui::Hook none;

    one.off(none);
    one.off(none);

    ought(none.empty());

    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
}

TRIAL("Handler: a hook sees the target") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);

    const ui::Handler* seen = nullptr;

    (void)up.on<ui::KeyEvent>([&seen](ui::KeyEvent& what) {
        seen = what.target();

        return true;
    });

    ui::KeyEvent what{U'a'};

    ought(down.send(what));
    ought(seen == &down);
}

TRIAL("Handler: the journey through three levels") {
    std::vector<std::string> told;

    Knot top{told, "szczyt"};
    Knot mid{told, "środek"};
    Knot low{told, "spód"};

    low.under(&mid);
    mid.under(&top);

    top.takes(true);

    ui::KeyEvent what{U'a'};

    ought(low.send(what));
    same(told.size(), std::size_t{3});
    same(told[0], std::string{"spód:handle"});
    same(told[1], std::string{"środek:handle"});
    same(told[2], std::string{"szczyt:handle"});
}

TRIAL("Handler: every kind of event travels the same way") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);

    ui::MouseEvent what{ui::Spot{}, ui::Press::down, ui::Mouse::left};

    ought(!down.send(what));
    same(told.size(), std::size_t{2});
    same(told[0], std::string{"dół:mysz"});
    same(told[1], std::string{"góra:mysz"});
}

TRIAL("Handler: a hook detached while it runs does not break the loop") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    ui::Hook which;

    which = one.on<ui::KeyEvent>([&one, &which, &told](ui::KeyEvent&) {
        told.push_back("pierwszy");
        one.off(which);

        return false;
    });

    (void)one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("drugi");

        return false;
    });

    ui::KeyEvent what{U'a'};

    ought(!one.send(what));
    same(told.size(), std::size_t{3});
    same(told[2], std::string{"drugi"});
}

TRIAL("Handler: a node detached from its parent during handle still passes the event up") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);

    (void)down.on<ui::KeyEvent>([&down](ui::KeyEvent&) {
        down.under(nullptr);

        return false;
    });

    ui::KeyEvent what{U'a'};

    ought(!down.send(what));
    same(told.size(), std::size_t{2});
    same(told[1], std::string{"góra:handle"});
}

// An event of the program travels only on the callers' channel. The widget's channel knows only the
// library's events (for a type outside the set, `carried` does not even open it), and hooks receive
// such an event like any other: upward, to the first one that takes it.
namespace {

class Own : public ui::Event {};

class Mute : public ui::Handler {
public:
    // Hangs this node under the given parent.
    void under(ui::Handler* one) { up_ = one; }

    // The parent set by `under`, or none.
    [[nodiscard]] ui::Handler* parent() const override { return up_; }

    // Starts the event's journey at this node.
    template <typename One>
    bool send(One& what) {
        return trigger(what);
    }

private:
    ui::Handler* up_ = nullptr;
};

}

TRIAL("Handler: a program's event travels up to the hooks") {
    Mute under;
    Mute middle;
    Mute above;

    under.under(&middle);
    middle.under(&above);

    std::vector<std::string> heard;

    (void)middle.on<Own>([&heard](Own&) {
        heard.emplace_back("środek");

        return false;
    });
    (void)above.on<Own>([&heard](Own&) {
        heard.emplace_back("góra");

        return true;
    });

    Own what;

    ought(under.send(what));
    same(heard.size(), std::size_t{2});
    same(heard[1], std::string{"góra"});
}

TRIAL("Handler: a program's event that nobody took returns false") {
    Mute under;
    Mute above;

    under.under(&above);

    int heard = 0;

    (void)above.on<Own>([&heard](Own&) {
        ++heard;

        return false;
    });

    Own what;

    ought(!under.send(what));
    same(heard, 1);
}
