#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "tal/clock.hpp"
#include "tal/document.hpp"
#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/key.hpp"
#include "tal/node.hpp"
#include "tal/port.hpp"
#include "tal/style.hpp"

#include "tal/testing.hpp"

// Events of tal::ui: one track from the root to the target and back, taking an event, default
// actions, hooks that go even while an event is delivered, the focus with Tab, the mouse with its
// capture, clicks and hovering, notifications after a change or a layout, and timers on the
// document's clock.

namespace {

using namespace std::chrono_literals;

using tal::Area;
using tal::MouseButton;
using tal::Changed;
using tal::Code;
using tal::Colors;
using tal::Document;
using tal::Effect;
using tal::Fn;
using tal::Hook;
using tal::Hooks;
using tal::Key;
using tal::KeyEvent;
using tal::Limits;
using tal::Mods;
using tal::MouseEvent;
using tal::Node;
using tal::PasteEvent;
using tal::Phase;
using tal::Press;
using tal::Property;
using tal::Size;
using tal::Spot;
using tal::State;
using tal::Timer;

// A node of a fixed width and one row, focusable unless told otherwise.
class Knob : public Node {
public:
    explicit Knob(int wide, bool takes = true) : wide_(wide) { focusable(takes); }

    Property<std::string> label{*this, Effect::layout, ""};

    Property<int> shade{*this, Effect::paint, 0};

    // A default action for keys, as a widget registers it.
    [[nodiscard]] Hook fallback(Fn<KeyEvent> what) { return otherwise<KeyEvent>(std::move(what)); }

    // Holds the mouse until the button is released.
    void grab() { capture(); }

    // Gives up wanting the focus, or wants it again.
    void takes(bool on) { focusable(on); }

protected:
    // As wide as given, one row.
    Size measure(Limits room) const override { return Size{std::max(room.least.wide, wide_), std::max(room.least.high, 1)}; }

private:
    int wide_;
};

// Children side by side, each as wide as it measures.
class Strip : public Node {
protected:
    // The widths added up, the highest child's height.
    Size measure(Limits room) const override {
        Size made{0, 0};

        for (Node& one : children()) {
            const Size wanted = one.measured(Limits{{0, 0}, room.most});

            made = Size{made.wide + wanted.wide, std::max(made.high, wanted.high)};
        }

        return Size{std::max(made.wide, room.least.wide), std::max(made.high, room.least.high)};
    }

    // Each child after the one before it.
    void arrange(Area inside) override {
        int x = inside.at.x;

        for (Node& one : children()) {
            const Size wanted = one.measured(Limits{{0, 0}, inside.size});

            one.placed(Area{{x, inside.at.y}, {wanted.wide, inside.size.high}});
            x += wanted.wide;
        }
    }
};

// A document of 20 by 2 cells on a manual clock, with a strip of three knobs four cells wide (the
// middle one does not take the focus), laid out once.
struct Stage {
    std::shared_ptr<tal::ManualClock> clock = std::make_shared<tal::ManualClock>();
    Document document{tal::Port<tal::Clock>{clock}};
    Strip* strip = nullptr;
    Knob* first = nullptr;
    Knob* middle = nullptr;
    Knob* last = nullptr;
    std::vector<std::string> heard;
    Hooks hooks;

    Stage() {
        document.display(Size{20, 2}, Colors::rgb, true);

        strip = &document.children().add<Strip>();
        first = &strip->children().add<Knob>(4);
        middle = &strip->children().add<Knob>(4, false);
        last = &strip->children().add<Knob>(4);

        (void)document.frame();
    }

    // Notes every key event `one` hears in `phase`, under `name`.
    void listen(Node& one, std::string name, Phase phase) {
        hooks.add(one.on<KeyEvent>(
            [this, name = std::move(name), phase](KeyEvent& got) {
                heard.push_back(name + (phase == Phase::capture ? ":capture" : ":bubble") +
                                (&got.target() == last ? "@last" : ""));
            },
            phase));
    }
};

TRIAL("Events: down from the root through capture, back up through bubble, the target in both") {
    Stage stage;

    for (Node* one : {static_cast<Node*>(&stage.document), static_cast<Node*>(stage.strip), static_cast<Node*>(stage.last)}) {
        const std::string name = one == &stage.document ? "document" : one == stage.strip ? "strip" : "last";

        stage.listen(*one, name, Phase::bubble);
        stage.listen(*one, name, Phase::capture);
    }

    stage.last->focus();
    stage.document.key(Key{U'x'});

    SAME(stage.heard, (std::vector<std::string>{"document:capture@last", "strip:capture@last", "last:capture@last", "last:bubble@last",
                                                "strip:bubble@last", "document:bubble@last"}));
}

TRIAL("Events: taking an event stops it and cancels the default actions") {
    Stage stage;
    bool cancelled = true;

    stage.first->focus();
    stage.hooks.add(stage.first->fallback([&cancelled](KeyEvent&) { cancelled = false; }));
    stage.hooks.add(stage.strip->on<KeyEvent>([](KeyEvent& one) { one.take(); }, Phase::capture));
    stage.listen(*stage.first, "first", Phase::capture);

    stage.document.key(Key{Code::tab});

    OUGHT(stage.heard.empty());
    OUGHT(cancelled);
    OUGHT(stage.first->focused());
}

TRIAL("Events: default actions go from the target up, and the first that takes ends them") {
    Stage stage;
    std::vector<std::string> done;

    stage.first->focus();
    stage.hooks.add(stage.first->fallback([&done](KeyEvent&) { done.push_back("first"); }));
    stage.hooks.add(stage.first->fallback([&done](KeyEvent& one) {
        done.push_back("taking");
        one.take();
    }));

    stage.document.key(Key{Code::tab});

    SAME(done, (std::vector<std::string>{"first", "taking"}));
    OUGHT(stage.first->focused());

    stage.hooks = Hooks{};
    stage.document.key(Key{Code::tab});

    OUGHT(stage.last->focused());
}

TRIAL("Events: a hook that goes takes its handler with it, also while the event is delivered") {
    Stage stage;
    int later = 0;
    int earlier = 0;
    Hook second;
    Hook first = stage.document.on<KeyEvent>([&](KeyEvent&) {
        ++earlier;
        second = Hook{};
    });

    second = stage.document.on<KeyEvent>([&later](KeyEvent&) { ++later; });

    stage.document.key(Key{U'a'});
    SAME(earlier, 1);
    SAME(later, 0);

    Hook moved = std::move(first);

    stage.document.key(Key{U'a'});
    SAME(earlier, 2);

    moved = Hook{};
    stage.document.key(Key{U'a'});
    SAME(earlier, 2);
}

TRIAL("Focus: Tab and Shift+Tab go through the nodes that take it in tree order, round the end") {
    Stage stage;
    std::vector<std::string> told;

    stage.hooks.add(stage.first->on<tal::Focused>([&told](tal::Focused&) { told.push_back("first in"); }));
    stage.hooks.add(stage.first->on<tal::Blurred>([&told](tal::Blurred&) { told.push_back("first out"); }));

    Knob& fourth = stage.strip->children().add<Knob>(4);

    (void)stage.document.frame();

    OUGHT(stage.document.focused() == nullptr);

    stage.document.key(Key{Code::tab});
    OUGHT(stage.first->focused());
    OUGHT(stage.first->states().has(State::focus));

    stage.document.key(Key{Code::tab});
    OUGHT(stage.last->focused());
    OUGHT(!stage.first->states().has(State::focus));

    stage.document.key(Key{Code::tab});
    OUGHT(fourth.focused());

    stage.document.key(Key{Code::tab});
    OUGHT(stage.first->focused());

    stage.document.key(Key{Code::tab, Mods{.shift = true}});
    OUGHT(fourth.focused());

    stage.document.key(Key{Code::tab, Mods{.shift = true}});
    OUGHT(stage.last->focused());

    stage.middle->focus();
    OUGHT(stage.last->focused());

    SAME(told, (std::vector<std::string>{"first in", "first out", "first in", "first out"}));
}

TRIAL("Focus: an inactive node takes none, and making it inactive passes the focus on") {
    Stage stage;

    stage.first->focus();
    stage.first->enable(false);

    OUGHT(!stage.first->enabled());
    OUGHT(stage.first->states().has(State::disabled));
    OUGHT(stage.last->focused());

    stage.first->focus();
    OUGHT(stage.last->focused());

    stage.document.key(Key{Code::tab});
    OUGHT(stage.last->focused());

    stage.strip->enable(false);
    OUGHT(stage.document.focused() == nullptr);
    OUGHT(!stage.last->focusable());

    stage.strip->enable(true);
    stage.first->enable(true);
    stage.document.key(Key{Code::tab});
    OUGHT(stage.first->focused());

    stage.first->takes(false);
    OUGHT(stage.last->focused());
}

TRIAL("Focus: removing the node that has it passes it on, and keys go to the document without one") {
    Stage stage;
    std::vector<std::string> got;

    stage.hooks.add(stage.document.on<KeyEvent>([&got, &stage](KeyEvent& one) {
        got.push_back(&one.target() == &stage.document ? "document" : "other");
    }));
    stage.hooks.add(stage.document.on<PasteEvent>([&got](PasteEvent& one) { got.push_back(one.text); }));

    stage.last->focus();
    (void)stage.strip->children().remove(*stage.last);
    OUGHT(stage.first->focused());

    (void)stage.strip->children().remove(*stage.first);
    OUGHT(stage.document.focused() == nullptr);

    stage.document.key(Key{U'q'});
    stage.document.paste("pasted");

    SAME(got, (std::vector<std::string>{"document", "pasted"}));
}

TRIAL("Mouse: the deepest active node under the pointer, a later sibling on top, inactive parts giving way") {
    Stage stage;
    std::vector<std::pair<Node*, Spot>> got;

    for (Node* one : {static_cast<Node*>(&stage.document), static_cast<Node*>(stage.strip), static_cast<Node*>(stage.first),
                      static_cast<Node*>(stage.middle)})
        stage.hooks.add(one->on<MouseEvent>([&got, one](MouseEvent& event) {
            if (&event.target() == one && event.press == Press::move) got.emplace_back(one, event.at);
        }));

    stage.document.mouse(Press::move, MouseButton::none, Spot{5, 0});
    stage.document.mouse(Press::move, MouseButton::none, Spot{2, 1});
    stage.document.mouse(Press::move, MouseButton::none, Spot{15, 0});

    stage.middle->enable(false);
    stage.document.mouse(Press::move, MouseButton::none, Spot{6, 0});

    SAME(got.size(), std::size_t{4});
    OUGHT(got[0].first == stage.middle);
    SAME(got[0].second, (Spot{1, 0}));
    OUGHT(got[1].first == stage.first);
    SAME(got[1].second, (Spot{2, 1}));
    OUGHT(got[2].first == stage.strip);
    SAME(got[2].second, (Spot{15, 0}));
    OUGHT(got[3].first == stage.strip);

    Node& above = stage.document.children().add<Node>();

    (void)stage.document.frame();
    got.clear();
    stage.hooks.add(above.on<MouseEvent>([&got, &above](MouseEvent& event) { got.emplace_back(&above, event.at); }));
    stage.document.mouse(Press::move, MouseButton::none, Spot{1, 0});

    SAME(got.size(), std::size_t{1});
    OUGHT(got[0].first == &above);
}

TRIAL("Mouse: a press gives the focus to the deepest node on the way that takes it") {
    Stage stage;
    Node& inside = stage.last->children().add<Node>();

    (void)stage.document.frame();

    stage.document.mouse(Press::down, MouseButton::left, Spot{9, 0});
    OUGHT(stage.last->focused());

    stage.document.mouse(Press::down, MouseButton::left, Spot{5, 0});
    OUGHT(stage.last->focused());

    stage.document.mouse(Press::down, MouseButton::back, Spot{1, 0});
    OUGHT(stage.last->focused());

    stage.document.mouse(Press::down, MouseButton::right, Spot{1, 0});
    OUGHT(stage.first->focused());
    OUGHT(inside.parent() == stage.last);
}

TRIAL("Mouse: a capture keeps the mouse until the button is released") {
    Stage stage;
    std::vector<Press> first;

    stage.hooks.add(stage.first->on<MouseEvent>([&](MouseEvent& one) {
        if (one.press == Press::down) stage.first->grab();

        if (&one.target() == stage.first) first.push_back(one.press);
    }));

    stage.document.mouse(Press::down, MouseButton::left, Spot{1, 0});
    stage.document.mouse(Press::move, MouseButton::left, Spot{15, 1});
    stage.document.mouse(Press::up, MouseButton::left, Spot{15, 1});
    stage.document.mouse(Press::move, MouseButton::none, Spot{15, 1});

    SAME(first, (std::vector<Press>{Press::down, Press::move, Press::up}));
}

TRIAL("Mouse: a release where the press was is a click, a second one soon a double click") {
    Stage stage;
    std::vector<std::pair<Press, Node*>> got;

    stage.hooks.add(stage.document.on<MouseEvent>([&got](MouseEvent& one) {
        if (one.press == Press::click || one.press == Press::double_click) got.emplace_back(one.press, &one.target());
    }));

    const auto clicked = [&stage](Spot where, MouseButton which = MouseButton::left) {
        stage.document.mouse(Press::down, which, where);
        stage.document.mouse(Press::up, which, where);
    };

    clicked(Spot{1, 0});
    stage.clock->advance(300ms);
    clicked(Spot{1, 0});
    clicked(Spot{1, 0});
    stage.clock->advance(401ms);
    clicked(Spot{1, 0});
    clicked(Spot{2, 0});
    clicked(Spot{2, 0}, MouseButton::right);
    clicked(Spot{2, 0}, MouseButton::forward);

    stage.document.mouse(Press::down, MouseButton::left, Spot{9, 0});
    stage.document.mouse(Press::up, MouseButton::left, Spot{10, 0});

    using P = Press;
    SAME(got, (std::vector<std::pair<Press, Node*>>{{P::click, stage.first},
                                                    {P::double_click, stage.first},
                                                    {P::click, stage.first},
                                                    {P::click, stage.first},
                                                    {P::click, stage.first},
                                                    {P::click, stage.first}}));
}

TRIAL("Hover: the node under the pointer and the nodes above it, short of the document") {
    Stage stage;

    stage.document.mouse(Press::move, MouseButton::none, Spot{1, 0});
    OUGHT(stage.first->states().has(State::hover));
    OUGHT(stage.strip->states().has(State::hover));
    OUGHT(!stage.document.states().has(State::hover));
    OUGHT(!stage.first->focused());

    stage.document.mouse(Press::move, MouseButton::none, Spot{9, 0});
    OUGHT(!stage.first->states().has(State::hover));
    OUGHT(stage.last->states().has(State::hover));

    stage.document.mouse(Press::move, MouseButton::none, Spot{19, 1});
    OUGHT(!stage.last->states().has(State::hover));
    OUGHT(stage.strip->states().has(State::hover));

    stage.document.mouse(Press::move, MouseButton::none, Spot{9, 0});

    const std::shared_ptr<Node> gone = stage.strip->children().remove(*stage.last);

    OUGHT(!gone->states().has(State::hover));
    OUGHT(stage.strip->states().has(State::hover));
}

TRIAL("Changed: a property tells its owner which one changed, once per new value") {
    Stage stage;
    std::vector<std::string> told;

    stage.hooks.add(stage.first->on<Changed>([&](Changed& one) {
        told.push_back(one.of(stage.first->label) ? "label" : one.of(stage.first->shade) ? "shade" : "?");
    }));

    stage.first->label = "a";
    stage.first->label = "a";
    stage.first->shade = 3;

    SAME(told, (std::vector<std::string>{"label", "shade"}));
}

TRIAL("Arranged: every arranged node hears it; a repaint goes into the same frame, a layout into the next") {
    Stage stage;
    int arranged = 0;

    stage.hooks.add(stage.first->on<tal::Arranged>([&](tal::Arranged&) {
        ++arranged;
        stage.first->shade = arranged;
        stage.last->label = std::to_string(arranged);
    }));

    stage.first->label = "x";

    (void)stage.document.frame();
    SAME(arranged, 1);
    OUGHT(stage.document.due());
    SAME(stage.document.tally().painted, std::uint64_t{6});

    (void)stage.document.frame();
    SAME(arranged, 1);
    OUGHT(!stage.document.due());
}

TRIAL("Timers: ring on the document's clock, earliest first; a dropped one never rings") {
    Stage stage;
    std::vector<std::string> rang;
    Timer later = stage.document.after(100ms, [&rang](Timer&) { rang.push_back("later"); });
    Timer sooner = stage.document.after(50ms, [&rang](Timer&) { rang.push_back("sooner"); });
    Timer same = stage.document.after(100ms, [&rang](Timer& one) {
        rang.push_back("same");
        one.again(30ms);
    });
    std::optional<Timer> dropped{stage.document.after(10ms, [&rang](Timer&) { rang.push_back("dropped"); })};

    dropped.reset();

    const auto start = stage.clock->now();

    SAME(stage.document.alarm(), std::optional{start + 50ms});

    stage.clock->advance(49ms);
    stage.document.ring();
    OUGHT(rang.empty());

    stage.clock->advance(51ms);
    stage.document.ring();
    SAME(rang, (std::vector<std::string>{"sooner", "later", "same"}));
    OUGHT(!later.waiting());
    OUGHT(same.waiting());
    SAME(stage.document.alarm(), std::optional{start + 130ms});

    Timer moved = std::move(same);

    stage.clock->advance(30ms);
    stage.document.ring();
    SAME(rang.back(), std::string{"same"});
    SAME(rang.size(), std::size_t{4});
    OUGHT(moved.waiting());

    moved = Timer{};
    stage.clock->advance(1s);
    stage.document.ring();
    SAME(rang.size(), std::size_t{4});
    OUGHT(!stage.document.alarm().has_value());
}

}
