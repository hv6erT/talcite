#include "tal/testing.hpp"

#include <memory>
#include <string>

#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/event.hpp"
#include "talcite/root.hpp"
#include "talcite/modals.hpp"

using namespace talcite;

// Being enabled is one mixin, and a change of it is an event. `EnabledMixin` holds the flag, derives
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

TRIAL("EnabledMixin: a change is announced once, the same value announces nothing") {
    Stage stage;

    int off = 0;
    int on = 0;

    const ui::Hook heard_disable = stage.one->on<ui::DisableEvent>([&off](ui::DisableEvent&) {
        ++off;

        return true;
    });
    const ui::Hook heard_enable = stage.one->on<ui::EnableEvent>([&on](ui::EnableEvent&) {
        ++on;

        return true;
    });

    stage.one->enabled(true);

    SAME(on, 0);

    stage.one->enabled(false);
    stage.one->enabled(false);

    SAME(off, 1);
    OUGHT(!stage.one->enabled());

    stage.one->enabled(true);
    stage.one->enabled(true);

    SAME(on, 1);
    OUGHT(stage.one->enabled());
}

TRIAL("EnabledMixin: a disabled node is not focusable, an enabled one is again") {
    Stage stage;

    OUGHT(stage.one->focusable());

    stage.one->enabled(false);

    OUGHT(!stage.one->focusable());

    stage.one->enabled(true);

    OUGHT(stage.one->focusable());
}

TRIAL("EnabledMixin: disabling bubbles up and tells where it came from") {
    Stage stage;

    const ui::Handler* from = nullptr;

    const ui::Hook heard_disable = stage.column->on<ui::DisableEvent>([&from](ui::DisableEvent& what) {
        from = what.target();

        return true;
    });

    stage.other->enabled(false);

    SAME(static_cast<const void*>(from), static_cast<const void*>(stage.other.get()));
}

// The order is a contract that `Select` relies on: the event arrives while the focus is still on
// the node being disabled, and giving up the focus comes after it.
TRIAL("EnabledMixin: the event arrives before the focus leaves") {
    Stage stage;

    stage.one->focus();

    bool held = false;

    const ui::Hook heard_disable = stage.one->on<ui::DisableEvent>([&held, &stage](ui::DisableEvent&) {
        held = stage.one->focused();

        return true;
    });

    stage.one->enabled(false);

    OUGHT(held);
    OUGHT(!stage.one->focused());
}

// A handler that enables the node again in response wins: the focus stays, because the node is
// focusable again.
TRIAL("EnabledMixin: re-enabled in a handler, it keeps the focus") {
    Stage stage;

    stage.one->focus();

    int on = 0;

    const ui::Hook heard_enable = stage.one->on<ui::EnableEvent>([&on](ui::EnableEvent&) {
        ++on;

        return true;
    });
    const ui::Hook heard_disable = stage.one->on<ui::DisableEvent>([&stage](ui::DisableEvent&) {
        stage.one->enabled(true);

        return true;
    });

    stage.one->enabled(false);

    OUGHT(stage.one->enabled());
    OUGHT(stage.one->focused());
    SAME(on, 1);
}

TRIAL("EnabledMixin: a disabled node does not take its hot key, an enabled one does") {
    Stage stage;

    int picked = 0;

    stage.other->focus();
    stage.one->keys().add(ui::Key{ui::Code::f2});
    const ui::Hook heard_pick = stage.one->on<ui::PickEvent>([&picked](ui::PickEvent&) {
        ++picked;

        return true;
    });

    stage.one->enabled(false);

    ui::KeyEvent key{ui::Key{ui::Code::f2}};

    (void)stage.top.dispatch(key);

    SAME(picked, 0);

    stage.one->enabled(true);

    ui::KeyEvent again{ui::Key{ui::Code::f2}};

    (void)stage.top.dispatch(again);

    SAME(picked, 1);
}
