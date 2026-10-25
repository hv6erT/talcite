#include "proving.hpp"

#include <chrono>
#include <memory>
#include <string>

#include "talcite/box.hpp"
#include "talcite/focusholder.hpp"
#include "talcite/frame.hpp"
#include "talcite/keybar.hpp"
#include "talcite/keybaritem.hpp"
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
    // the key bar gets its keys from below, not from here.
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

TRIAL("FocusHolder: takes focus by default, since that is what it is for") {
    Sheet top;

    const auto one = std::make_shared<ui::FocusHolder>();

    ought(one->focusable());

    top.children().add(one);
    (void)top.render({{0, 0}, {20, 10}});

    clicked(top, {5, 5});

    same(static_cast<const void*>(top.focused_node()), static_cast<const void*>(one.get()));
    ought(one->focused());
}

TRIAL("FocusHolder: a disabled one does not take focus") {
    Sheet top;

    const auto one = std::make_shared<ui::FocusHolder>();

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
TRIAL("FocusHolder: a click taken by the child does not reach the container") {
    Sheet top;

    const auto one = std::make_shared<ui::FocusHolder>();
    const auto said = std::make_shared<Noted>(true);

    one->children().add(said);
    top.children().add(one);
    (void)top.render({{0, 0}, {20, 10}});

    clicked(top, {5, 5});

    same(said->clicks, 1);
    same(static_cast<const void*>(top.focused_node()), static_cast<const void*>(nullptr));
    same(top.clicks, 0);
}

TRIAL("FocusHolder: a click refused by the child gives focus to the container") {
    Sheet top;

    const auto one = std::make_shared<ui::FocusHolder>();
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
TRIAL("FocusHolder: a disabled one passes the click further up") {
    Sheet top;

    const auto one = std::make_shared<ui::FocusHolder>();
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

// This is the real reason this element exists. A key bubbles up from the focus, and the key bar is
// an ancestor of the content, so without something to hold the focus `Root::dispatch` hands the key
// to the root and the path ends there. A window without a button and without a text field would
// then have no working F keys.
TRIAL("FocusHolder: the key bar gets keys only with focus") {
    proving::Hooks heard;

    Sheet top;

    const auto bar = std::make_shared<ui::KeyBar>();
    const auto one = std::make_shared<ui::FocusHolder>();

    int hits = 0;

    one->children().add(std::make_shared<ui::Frame>("treść"));
    bar->content(one);

    (void)proving::doing(heard, bar->add(ui::Code::f5, "Coś"), [&hits] { ++hits; });

    top.children().add(bar);
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

// The room its child wants, as any node with one child, and none without a child: sizing is the
// job of `Box`.
TRIAL("FocusHolder: wants what its child wants, and nothing without one") {
    const auto one = std::make_shared<ui::FocusHolder>();

    same(one->wants({20, 10}).wide, 0);
    same(one->wants({20, 10}).high, 0);

    const auto said = std::make_shared<ui::Span>(std::string{"trzy"});

    one->children().add(said);

    same(one->wants({20, 10}), said->wants({20, 10}));
}

// In a `Box` it covers the whole room, however small its content: a click beside the content
// takes the focus, which is how a window gives its F-keys to a key bar.
TRIAL("FocusHolder: in a box it covers the room, a click beside the content takes the focus") {
    Sheet top;

    const auto room = std::make_shared<ui::Box>();
    const auto one = std::make_shared<ui::FocusHolder>();

    one->children().add(std::make_shared<ui::Span>(std::string{"trzy"}));
    room->children().add(one);
    top.children().add(room);
    (void)top.render({{0, 0}, {20, 10}});

    clicked(top, {15, 8});

    same(static_cast<const void*>(top.focused_node()), static_cast<const void*>(one.get()));
}

// --- Painting -----------------------------------------------------------------------------------

// The container paints nothing of its own: it inherits the transparent pass-through of a child and
// with it the rule "paint what you are, not the rectangle you were given".
TRIAL("FocusHolder: an empty one leaves no trace on the sheet") {
    Sheet top;

    const auto one = std::make_shared<ui::FocusHolder>();

    top.children().add(one);

    const View made = top.render({{0, 0}, {6, 3}});

    bool held = true;

    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 6; ++x) held = held && made.at({x, y}).shown == Shown::none;

    ought(held);
}

TRIAL("FocusHolder: the child gets the whole assigned rectangle") {
    Sheet top;

    const auto one = std::make_shared<ui::FocusHolder>();
    const auto said = std::make_shared<ui::Span>(std::string{"abc"});

    one->children().add(said);
    top.children().add(one);

    const View made = top.render({{0, 0}, {6, 3}});

    same(made.at({0, 0}).letter, U'a');
    same(made.at({2, 0}).letter, U'c');
}

// Disabling gives up the focus, as in the button, the field and the list row; a node that says it
// is not willing must not keep the focus.
TRIAL("FocusHolder: a disabled one under focus hands it to the ancestor") {
    Sheet top;

    const auto up = std::make_shared<ui::FocusHolder>();
    const auto one = std::make_shared<ui::FocusHolder>();

    up->children().add(one);
    top.children().add(up);

    (void)top.render({{0, 0}, {20, 10}});

    one->focus();

    ought(one->focused());

    one->enabled(false);

    ought(!one->focused());
    ought(up->focused());
}

TRIAL("FocusHolder: a disabled one without a willing ancestor clears the focus") {
    Sheet top;

    const auto one = std::make_shared<ui::FocusHolder>();

    top.children().add(one);

    (void)top.render({{0, 0}, {20, 10}});

    one->focus();

    one->enabled(false);

    same(static_cast<const void*>(top.focused_node()), static_cast<const void*>(nullptr));
}
