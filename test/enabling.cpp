#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/event.hpp"
#include "talcite/root.hpp"
#include "talcite/modals.hpp"

using namespace talcite;

// Being enabled is one mixin, and a change of it is an event. `EnabledNode` holds the flag, derives
// `focusable()` from it and gives up the focus when disabled. Whoever wants to react to the change
// (a widget through `handle`, a caller through `on<>`) listens for `EnableEvent` and
// `DisableEvent`.

namespace {

// Hot keys are handed out by the dialog, so the buttons stand in a dialog, as in any window.
struct Stage {
    ui::Root top;
    std::shared_ptr<ui::Modals> window = std::make_shared<ui::Modals>();
    std::shared_ptr<ui::Dialog> dialog = std::make_shared<ui::Dialog>();
    std::shared_ptr<ui::Column> column = std::make_shared<ui::Column>();
    std::shared_ptr<ui::Button> one = std::make_shared<ui::Button>("Tak");
    std::shared_ptr<ui::Button> other = std::make_shared<ui::Button>("Nie");

    // Builds the window with the dialog and its two buttons, and renders it once.
    Stage() {
        column->children().add(one);
        column->children().add(other);
        dialog->children().add(column);
        top.children().add(window);
        window->open(dialog);
        (void)top.render({{0, 0}, {20, 10}});
    }
};

}

TRIAL("EnabledNode: a change is announced once, the same value announces nothing") {
    Stage stage;

    int off = 0;
    int on = 0;

    (void)stage.one->on<ui::DisableEvent>([&off](ui::DisableEvent&) {
        ++off;

        return true;
    });
    (void)stage.one->on<ui::EnableEvent>([&on](ui::EnableEvent&) {
        ++on;

        return true;
    });

    stage.one->enabled(true);

    same(on, 0);

    stage.one->enabled(false);
    stage.one->enabled(false);

    same(off, 1);
    ought(!stage.one->enabled());

    stage.one->enabled(true);
    stage.one->enabled(true);

    same(on, 1);
    ought(stage.one->enabled());
}

TRIAL("EnabledNode: a disabled node is not focusable, an enabled one is again") {
    Stage stage;

    ought(stage.one->focusable());

    stage.one->enabled(false);

    ought(!stage.one->focusable());

    stage.one->enabled(true);

    ought(stage.one->focusable());
}

TRIAL("EnabledNode: disabling bubbles up and tells where it came from") {
    Stage stage;

    const ui::Handler* from = nullptr;

    (void)stage.column->on<ui::DisableEvent>([&from](ui::DisableEvent& what) {
        from = what.target();

        return true;
    });

    stage.other->enabled(false);

    same(static_cast<const void*>(from), static_cast<const void*>(stage.other.get()));
}

// The order is a contract that `Select` relies on: the event arrives while the focus is still on
// the node being disabled, and giving up the focus comes after it.
TRIAL("EnabledNode: the event arrives before the focus leaves") {
    Stage stage;

    stage.one->focus();

    bool held = false;

    (void)stage.one->on<ui::DisableEvent>([&held, &stage](ui::DisableEvent&) {
        held = stage.one->focused();

        return true;
    });

    stage.one->enabled(false);

    ought(held);
    ought(!stage.one->focused());
}

// A handler that enables the node again in response wins: the focus stays, because the node is
// focusable again.
TRIAL("EnabledNode: re-enabled in a handler, it keeps the focus") {
    Stage stage;

    stage.one->focus();

    int on = 0;

    (void)stage.one->on<ui::EnableEvent>([&on](ui::EnableEvent&) {
        ++on;

        return true;
    });
    (void)stage.one->on<ui::DisableEvent>([&stage](ui::DisableEvent&) {
        stage.one->enabled(true);

        return true;
    });

    stage.one->enabled(false);

    ought(stage.one->enabled());
    ought(stage.one->focused());
    same(on, 1);
}

TRIAL("EnabledNode: a disabled node does not take its hot key, an enabled one does") {
    Stage stage;

    int picked = 0;

    stage.other->focus();
    stage.one->keys().add(ui::Key{ui::Code::f2});
    (void)stage.one->on<ui::PickEvent>([&picked](ui::PickEvent&) {
        ++picked;

        return true;
    });

    stage.one->enabled(false);

    ui::KeyEvent key{ui::Key{ui::Code::f2}};

    (void)stage.top.dispatch(key);

    same(picked, 0);

    stage.one->enabled(true);

    ui::KeyEvent again{ui::Key{ui::Code::f2}};

    (void)stage.top.dispatch(again);

    same(picked, 1);
}
