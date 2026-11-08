#include "tal/testing.hpp"

#include <optional>
#include <string>
#include <utility>
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

    OUGHT(!one.send(what));
    SAME(told.size(), std::size_t{1});
    SAME(told[0], std::string{"jeden:handle"});
}

TRIAL("Handler: an unhandled event goes to the parent") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);

    ui::KeyEvent what{U'a'};

    OUGHT(!down.send(what));
    SAME(told.size(), std::size_t{2});
    SAME(told[0], std::string{"dół:handle"});
    SAME(told[1], std::string{"góra:handle"});
}

TRIAL("Handler: a handled event goes no higher") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);
    down.takes(true);

    ui::KeyEvent what{U'a'};

    OUGHT(down.send(what));
    SAME(told.size(), std::size_t{1});
    SAME(told[0], std::string{"dół:handle"});
}

TRIAL("Handler: the target is the first node, not the last") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);
    up.takes(true);

    ui::KeyEvent what{U'a'};

    OUGHT(down.send(what));
    OUGHT(what.target() == &down);
}

TRIAL("Handler: a hook from on receives the event when handle did not take it") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    const ui::Hook heard_key = one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("uchwyt");

        return false;
    });

    ui::KeyEvent what{U'a'};

    OUGHT(!one.send(what));
    SAME(told.size(), std::size_t{2});
    SAME(told[0], std::string{"jeden:handle"});
    SAME(told[1], std::string{"uchwyt"});
}

// Two channels, two independent stops. The widget and the caller travel the same way, but each
// closes only its own path: true from `handle` says "no more reacting upward", true from a hook
// says "no more hooks upward", and neither says anything about the other channel. So a widget never
// hides from the caller an event the caller has every right to see.
TRIAL("Handler: a handle that took the event does not silence the hooks") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    one.takes(true);

    const ui::Hook heard_key = one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("uchwyt");

        return false;
    });

    ui::KeyEvent what{U'a'};

    OUGHT(one.send(what));
    SAME(told.size(), std::size_t{2});
    SAME(told[0], std::string{"jeden:handle"});
    SAME(told[1], std::string{"uchwyt"});
}

TRIAL("Handler: all hooks run, even when the first one took the event") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    const ui::Hook heard_key_1 = one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("pierwszy");

        return true;
    });

    const ui::Hook heard_key_2 = one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("drugi");

        return false;
    });

    ui::KeyEvent what{U'a'};

    OUGHT(one.send(what));
    SAME(told.size(), std::size_t{3});
    SAME(told[1], std::string{"pierwszy"});
    SAME(told[2], std::string{"drugi"});
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

    const ui::Hook heard_key_1 = down.on<ui::KeyEvent>([](ui::KeyEvent&) { return true; });

    const ui::Hook heard_key_2 = up.on<ui::KeyEvent>([&seen](ui::KeyEvent&) {
        ++seen;

        return true;
    });

    ui::KeyEvent what{U'a'};

    OUGHT(down.send(what));
    SAME(told.size(), std::size_t{2});
    SAME(told[0], std::string{"dół:handle"});
    SAME(told[1], std::string{"góra:handle"});

    // The hook at the top no longer runs: its channel was closed at the bottom.
    SAME(seen, 0);
}

TRIAL("Handler: hooks of another kind stay silent") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    const ui::Hook heard_mouse = one.on<ui::MouseEvent>([&told](ui::MouseEvent&) {
        told.push_back("mysz");

        return true;
    });

    ui::KeyEvent what{U'a'};

    OUGHT(!one.send(what));
    SAME(told.size(), std::size_t{1});
    SAME(told[0], std::string{"jeden:handle"});
}

// A hook unhooks its listener when it goes: the listener lasts exactly as long as whoever keeps the
// hook.
TRIAL("Handler: a hook that goes unhooks its listener") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    {
        const ui::Hook which = one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
            told.push_back("uchwyt");

            return true;
        });

        ui::KeyEvent what{U'a'};

        OUGHT(one.send(what));
        SAME(told.size(), std::size_t{2});
    }

    ui::KeyEvent what{U'a'};

    OUGHT(!one.send(what));
    SAME(told.size(), std::size_t{3});
    SAME(told[2], std::string{"jeden:handle"});
}

// A hook is empty when fresh, after it gave its listener away and after it was emptied by
// assignment; only `on` gives a full one. Moving hands the listener over without unhooking it, and
// assigning an empty hook unhooks it.
TRIAL("Handler: a hook moves its listener and empties when given away") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    const ui::Hook fresh;

    OUGHT(fresh.empty());

    ui::Hook which = one.on<ui::KeyEvent>([](ui::KeyEvent&) { return true; });

    OUGHT(!which.empty());

    ui::Hook moved = std::move(which);

    OUGHT(which.empty());
    OUGHT(!moved.empty());

    ui::KeyEvent first{U'a'};

    OUGHT(one.send(first));

    moved = ui::Hook{};

    OUGHT(moved.empty());

    ui::KeyEvent second{U'a'};

    OUGHT(!one.send(second));
}

// Assigning a hook over a full one unhooks what the old one held and keeps the new listener.
TRIAL("Handler: a hook assigned over another unhooks the old listener") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    ui::Hook which = one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("stary");

        return false;
    });

    which = one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("nowy");

        return false;
    });

    ui::KeyEvent what{U'a'};

    OUGHT(!one.send(what));
    SAME(told.size(), std::size_t{2});
    SAME(told[1], std::string{"nowy"});
}

// A hook with the same number on a different type does not remove someone else's: the type is in
// the hook, so exactly what was attached is detached.
TRIAL("Handler: a hook detaches only its own type") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    ui::Hook mouse = one.on<ui::MouseEvent>([](ui::MouseEvent&) { return true; });

    const ui::Hook key = one.on<ui::KeyEvent>([](ui::KeyEvent&) { return true; });

    mouse = ui::Hook{};

    ui::KeyEvent what{U'a'};

    OUGHT(one.send(what));
}

TRIAL("Handler: a hook that goes removes one listener, not all") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    ui::Hook first = one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("pierwszy");

        return false;
    });

    const ui::Hook second = one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("drugi");

        return false;
    });

    first = ui::Hook{};

    ui::KeyEvent what{U'a'};

    OUGHT(!one.send(what));
    SAME(told.size(), std::size_t{2});
    SAME(told[1], std::string{"drugi"});
}

// A hook that outlives its handler has nothing left to unhook: it is empty, and its end breaks
// nothing.
TRIAL("Handler: a hook outliving its handler is empty and goes quietly") {
    std::vector<std::string> told;

    ui::Hook which;

    {
        Knot one{told, "jeden"};

        which = one.on<ui::KeyEvent>([](ui::KeyEvent&) { return true; });

        OUGHT(!which.empty());
    }

    OUGHT(which.empty());

    which = ui::Hook{};

    OUGHT(which.empty());
}

TRIAL("Handler: a hook sees the target") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);

    const ui::Handler* seen = nullptr;

    const ui::Hook heard_key = up.on<ui::KeyEvent>([&seen](ui::KeyEvent& what) {
        seen = what.target();

        return true;
    });

    ui::KeyEvent what{U'a'};

    OUGHT(down.send(what));
    OUGHT(seen == &down);
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

    OUGHT(low.send(what));
    SAME(told.size(), std::size_t{3});
    SAME(told[0], std::string{"spód:handle"});
    SAME(told[1], std::string{"środek:handle"});
    SAME(told[2], std::string{"szczyt:handle"});
}

TRIAL("Handler: every kind of event travels the same way") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);

    ui::MouseEvent what{ui::Spot{}, ui::Press::down, ui::Mouse::left};

    OUGHT(!down.send(what));
    SAME(told.size(), std::size_t{2});
    SAME(told[0], std::string{"dół:mysz"});
    SAME(told[1], std::string{"góra:mysz"});
}

TRIAL("Handler: a hook detached while it runs does not break the loop") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    ui::Hook which;

    which = one.on<ui::KeyEvent>([&which, &told](ui::KeyEvent&) {
        told.push_back("pierwszy");
        which = ui::Hook{};

        return false;
    });

    const ui::Hook heard_key = one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("drugi");

        return false;
    });

    ui::KeyEvent what{U'a'};

    OUGHT(!one.send(what));
    SAME(told.size(), std::size_t{3});
    SAME(told[2], std::string{"drugi"});

    ui::KeyEvent again{U'a'};

    OUGHT(!one.send(again));
    SAME(told.size(), std::size_t{5});
    SAME(told[4], std::string{"drugi"});
}

// A listener unhooked by an earlier one during the same delivery is not called any more: its keeper
// may be gone, and the delivery already holds its own copy of the listeners.
TRIAL("Handler: a listener unhooked during a delivery is skipped in it") {
    std::vector<std::string> told;

    Knot one{told, "jeden"};

    std::optional<ui::Hook> later;

    const ui::Hook first = one.on<ui::KeyEvent>([&later, &told](ui::KeyEvent&) {
        told.push_back("pierwszy");
        later.reset();

        return false;
    });

    later.emplace(one.on<ui::KeyEvent>([&told](ui::KeyEvent&) {
        told.push_back("drugi");

        return false;
    }));

    ui::KeyEvent what{U'a'};

    OUGHT(!one.send(what));
    SAME(told.size(), std::size_t{2});
    SAME(told[1], std::string{"pierwszy"});
}

TRIAL("Handler: a node detached from its parent during handle still passes the event up") {
    std::vector<std::string> told;

    Knot up{told, "góra"};
    Knot down{told, "dół"};

    down.under(&up);

    const ui::Hook heard_key = down.on<ui::KeyEvent>([&down](ui::KeyEvent&) {
        down.under(nullptr);

        return false;
    });

    ui::KeyEvent what{U'a'};

    OUGHT(!down.send(what));
    SAME(told.size(), std::size_t{2});
    SAME(told[1], std::string{"góra:handle"});
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

    const ui::Hook heard_own_1 = middle.on<Own>([&heard](Own&) {
        heard.emplace_back("środek");

        return false;
    });
    const ui::Hook heard_own_2 = above.on<Own>([&heard](Own&) {
        heard.emplace_back("góra");

        return true;
    });

    Own what;

    OUGHT(under.send(what));
    SAME(heard.size(), std::size_t{2});
    SAME(heard[1], std::string{"góra"});
}

TRIAL("Handler: a program's event that nobody took returns false") {
    Mute under;
    Mute above;

    under.under(&above);

    int heard = 0;

    const ui::Hook heard_own = above.on<Own>([&heard](Own&) {
        ++heard;

        return false;
    });

    Own what;

    OUGHT(!under.send(what));
    SAME(heard, 1);
}
