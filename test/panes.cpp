#include "proving.hpp"

#include <chrono>
#include <memory>
#include <string>

#include "talcite/frame.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legend.hpp"
#include "talcite/pane.hpp"
#include "talcite/root.hpp"
#include "talcite/span.hpp"

using namespace talcite;

#include "doing.hpp"

namespace {

class Sheet : public ui::Root {
public:
    int clicks = 0;

protected:
    // The root swallows every key, as in every demo and as the application will. This shows that
    // the legend gets its keys from below, not from here.
    bool handle(ui::KeyEvent&) override { return true; }

    // Counts the clicks that reach the root and takes each of them.
    bool handle(ui::ClickEvent&) override {
        ++clicks;

        return true;
    }
};

// A child that counts clicks and says whether it takes them. It fills all the room, so it stands
// closer to the cursor than the container, and that is the heart of the mouse's path: the event
// starts at the deepest node under the cursor and goes up, so the container sees only what the
// child did not take.
class Noted : public ui::Node {
public:
    explicit Noted(bool takes) : takes_(takes) {}

    int clicks = 0;

    // All the room offered, so that the child covers the whole container.
    [[nodiscard]] ui::Size wants(ui::Size room) const override { return room; }

protected:
    // Counts the click and takes it or not, as set in the constructor.
    bool handle(ui::ClickEvent&) override {
        ++clicks;

        return takes_;
    }

private:
    bool takes_ = true;
};

// Each click comes a second after the previous one, so two in a row on the same cell do not make a
// double click; the trials ask about single ones.
void clicked(ui::Root& onto, ui::Spot where) {
    static std::chrono::steady_clock::time_point moment{};

    moment += std::chrono::seconds{1};

    ui::MouseEvent down{where, ui::Press::down, ui::Mouse::left, {}, moment};
    ui::MouseEvent up{where, ui::Press::up, ui::Mouse::left, {}, moment};

    (void)onto.dispatch(down);
    (void)onto.dispatch(up);
}

}

// --- Focus --------------------------------------------------------------------------------------

TRIAL("Pane: takes focus by default, since that is what it is for") {
    Sheet top;

    const auto one = std::make_shared<ui::Pane>();

    ought(one->focusable());

    top.children().add(one);
    (void)top.render({{0, 0}, {20, 10}});

    clicked(top, {5, 5});

    same(static_cast<const void*>(top.focused_node()), static_cast<const void*>(one.get()));
    ought(one->focused());
}

TRIAL("Pane: a disabled one does not take focus") {
    Sheet top;

    const auto one = std::make_shared<ui::Pane>();

    one->enabled(false);

    ought(!one->focusable());

    top.children().add(one);
    (void)top.render({{0, 0}, {20, 10}});

    clicked(top, {5, 5});

    same(static_cast<const void*>(top.focused_node()), static_cast<const void*>(nullptr));
}

// The container does not take clicks away from its children. The mouse goes from the deepest node
// under the cursor upward, so the child comes first; whoever takes the click ends the path. The
// container gets only what nobody below wanted, and only then takes focus.
TRIAL("Pane: a click taken by the child does not reach the container") {
    Sheet top;

    const auto one = std::make_shared<ui::Pane>();
    const auto said = std::make_shared<Noted>(true);

    one->children().add(said);
    top.children().add(one);
    (void)top.render({{0, 0}, {20, 10}});

    clicked(top, {5, 5});

    same(said->clicks, 1);
    same(static_cast<const void*>(top.focused_node()), static_cast<const void*>(nullptr));
    same(top.clicks, 0);
}

TRIAL("Pane: a click refused by the child gives focus to the container") {
    Sheet top;

    const auto one = std::make_shared<ui::Pane>();
    const auto said = std::make_shared<Noted>(false);

    one->children().add(said);
    top.children().add(one);
    (void)top.render({{0, 0}, {20, 10}});

    clicked(top, {5, 5});

    same(said->clicks, 1);
    same(static_cast<const void*>(top.focused_node()), static_cast<const void*>(one.get()));
    same(top.clicks, 0);
}

// A disabled container is transparent to the mouse too: the click goes further up, as if the
// container were not there.
TRIAL("Pane: a disabled one passes the click further up") {
    Sheet top;

    const auto one = std::make_shared<ui::Pane>();
    const auto said = std::make_shared<Noted>(false);

    one->enabled(false);
    one->children().add(said);
    top.children().add(one);
    (void)top.render({{0, 0}, {20, 10}});

    clicked(top, {5, 5});

    same(said->clicks, 1);
    same(top.clicks, 1);
    same(static_cast<const void*>(top.focused_node()), static_cast<const void*>(nullptr));
}

// This is the real reason this element exists. A key bubbles up from the focus, and the legend is
// an ancestor of the content, so without something to hold the focus `Root::dispatch` hands the key
// to the root and the path ends there. A window without a button and without a text field would
// then have no working F keys.
TRIAL("Pane: the legend gets keys only with focus") {
    Sheet top;

    const auto legend = std::make_shared<ui::Legend>();
    const auto one = std::make_shared<ui::Pane>();

    int hits = 0;

    one->children().add(std::make_shared<ui::Frame>("treść"));
    legend->content(one);

    (void)proving::doing(legend->add(ui::Code::f5, "Coś"), [&hits] { ++hits; });

    top.children().add(legend);
    (void)top.render({{0, 0}, {30, 10}});

    ui::KeyEvent before{ui::Code::f5};

    (void)top.dispatch(before);

    same(hits, 0);

    one->focus();

    ui::KeyEvent after{ui::Code::f5};

    (void)top.dispatch(after);

    same(hits, 1);
}

// --- Room ---------------------------------------------------------------------------------------

// The whole room, even when the child wants less, and that is the container's entire part in
// sizing. It is fixed, not a setting; setting a size is what `Box` is for.
TRIAL("Pane: asks for all the room, even if the child wants less") {
    const auto one = std::make_shared<ui::Pane>();
    const auto said = std::make_shared<ui::Span>(std::string{"trzy"});

    one->children().add(said);

    ought(said->wants({20, 10}).wide < 20);

    same(one->wants({20, 10}).wide, 20);
    same(one->wants({20, 10}).high, 10);
}

// An empty one takes everything too, unlike `Node`, which asks for nothing without a child.
TRIAL("Pane: an empty one asks for all the room") {
    const auto one = std::make_shared<ui::Pane>();

    same(one->wants({20, 10}).wide, 20);
    same(one->wants({20, 10}).high, 10);
}

// --- Painting -----------------------------------------------------------------------------------

// The container paints nothing of its own: it inherits the transparent pass-through of a child and
// with it the rule "paint what you are, not the rectangle you were given".
TRIAL("Pane: an empty one leaves no trace on the sheet") {
    Sheet top;

    const auto one = std::make_shared<ui::Pane>();

    top.children().add(one);

    const View made = top.render({{0, 0}, {6, 3}});

    bool held = true;

    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 6; ++x) held = held && made.at({x, y}).shown == Shown::none;

    ought(held);
}

TRIAL("Pane: the child gets the whole assigned rectangle") {
    Sheet top;

    const auto one = std::make_shared<ui::Pane>();
    const auto said = std::make_shared<ui::Span>(std::string{"abc"});

    one->children().add(said);
    top.children().add(one);

    const View made = top.render({{0, 0}, {6, 3}});

    same(made.at({0, 0}).letter, U'a');
    same(made.at({2, 0}).letter, U'c');
}

// Disabling gives up the focus, as in the button, the field and the list row; a node that says it
// is not willing must not keep the focus.
TRIAL("Pane: a disabled one under focus hands it to the ancestor") {
    Sheet top;

    const auto up = std::make_shared<ui::Pane>();
    const auto one = std::make_shared<ui::Pane>();

    up->children().add(one);
    top.children().add(up);

    (void)top.render({{0, 0}, {20, 10}});

    one->focus();

    ought(one->focused());

    one->enabled(false);

    ought(!one->focused());
    ought(up->focused());
}

TRIAL("Pane: a disabled one without a willing ancestor clears the focus") {
    Sheet top;

    const auto one = std::make_shared<ui::Pane>();

    top.children().add(one);

    (void)top.render({{0, 0}, {20, 10}});

    one->focus();

    one->enabled(false);

    same(static_cast<const void*>(top.focused_node()), static_cast<const void*>(nullptr));
}
