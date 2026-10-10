#include "proving.hpp"

#include <memory>

#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/root.hpp"
#include "talcite/modal.hpp"
#include "talcite/modals.hpp"
#include "talcite/row.hpp"

using namespace talcite;

namespace {

// A focusable node of a fixed size that paints its area with one letter and counts the clicks it
// receives.
class Box : public ui::Node {
public:
    Box(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    // Always focusable, so that it can hold the focus under and inside veils.
    [[nodiscard]] bool focusable() const override { return true; }

    // The fixed size, whatever space is offered.
    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

    int hit = 0;

protected:
    // Fills the whole given size with the letter, shifted to the visible part.
    View paint(ui::Size given, ui::Area seen) override {
        View made{seen.size};

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                made.put({x - seen.at.x, y - seen.at.y}, what_, Style{});

        return made;
    }

    // Counts a click that reached this box.
    bool handle(ui::ClickEvent&) override {
        ++hit;

        return true;
    }

private:
    ui::Size wants_;
    char32_t what_ = U'?';
};

// A veil that places its box where it is told, instead of in the middle. That is why `seat()` is
// virtual: it is the only thing in which veils differ from one another.
class Aside : public ui::Modal {
public:
    explicit Aside(ui::Spot where) : ui::Modal(ui::ModalShape{false}), where_(where) {}

protected:
    // The box at the given spot, in the size its only child wants; nothing when there is no child.
    [[nodiscard]] ui::Area seat(ui::Size given) const override {
        if (children().empty()) return {};

        ui::Size wants = children().front()->wants(given);

        return {where_, wants};
    }

private:
    ui::Spot where_;
};

// The smallest surroundings needed: a window with content underneath and a veil above it. The
// content is a row, so that a trial can put a second node next to the first.
struct Stage {
    ui::Root top;
    std::shared_ptr<ui::Modals> window = std::make_shared<ui::Modals>();
    std::shared_ptr<ui::Row> under = std::make_shared<ui::Row>();
    // The focusable content under the veils; it has the focus whenever no veil is open.
    std::shared_ptr<Box> body = std::make_shared<Box>(ui::Size{40, 10}, U'.');

    // Puts the content into the window, the window into the root, and gives the content the focus.
    Stage() {
        under->children().add(body);
        window->content(under);
        top.children().add(window);
        body->focus();
    }
};

}

// --- What a veil knows about itself --------------------------------------------------------------

// A veil is the screen, although only its box is visible on it. All of modality comes from this one
// fact, so we guard it directly.
TRIAL("Modal: it takes everything it was given and lies above its siblings") {
    const ui::Modal one;

    same(one.wants({40, 10}), (ui::Size{40, 10}));
    same(one.capacity(), 1);
    ought(one.focusable());
}

TRIAL("Modal: the box stands in the middle") {
    Stage stage;

    const auto one = std::make_shared<ui::Modal>(ui::ModalShape{false});

    one->children().add(std::make_shared<Box>(ui::Size{10, 4}, U'o'));

    stage.window->open(one);

    const View made = stage.top.render({{0, 0}, {40, 10}});

    // (40−10)/2 = 15, (10−4)/2 = 3.
    same(made.at({15, 3}).letter, U'o');
    same(made.at({24, 6}).letter, U'o');
    same(made.at({14, 3}).letter, U'.');
}

// --- Modality ------------------------------------------------------------------------------------

TRIAL("Modal: a click beside the box does not reach what is underneath") {
    Stage stage;

    const auto one = std::make_shared<ui::Modal>(ui::ModalShape{false});

    one->children().add(std::make_shared<Box>(ui::Size{10, 4}, U'o'));

    stage.window->open(one);

    (void)stage.top.render({{0, 0}, {40, 10}});

    ui::ClickEvent what{{1, 1}, ui::Mouse::left};

    ought(stage.top.dispatch(what));
    same(stage.body->hit, 0);
}

// A veil does not take Escape. Closing the window would perform an action the caller has no way to
// cancel. It swallows Escape like any other key, because swallowing does nothing, and modality
// cannot be expressed otherwise.
TRIAL("Modal: Esc closes nothing, it is only swallowed") {
    Stage stage;

    const auto one = std::make_shared<ui::Modal>();
    const auto inside = std::make_shared<Box>(ui::Size{6, 3}, U'o');

    one->children().add(inside);

    stage.window->open(one);

    same(stage.top.focused_node(), static_cast<ui::Node*>(inside.get()));

    ui::KeyEvent what{ui::Code::escape};

    ought(stage.top.dispatch(what));

    ought(!stage.window->empty());
    same(stage.top.focused_node(), static_cast<ui::Node*>(inside.get()));
}

// Whoever wants "Esc closes" attaches a hook, and then it works, because a hook sees the key
// regardless of the veil swallowing it.
TRIAL("Modal: closing with Escape can be added with a hook") {
    Stage stage;

    const auto one = std::make_shared<ui::Modal>();
    const auto inside = std::make_shared<Box>(ui::Size{6, 3}, U'o');

    one->children().add(inside);

    stage.window->open(one);

    const std::weak_ptr<ui::Modal> weak = one;

    (void)one->on<ui::KeyEvent>([weak](ui::KeyEvent& what) {
        if (what.key().code() != ui::Code::escape) return false;

        if (const auto mine = weak.lock()) mine->close();

        return true;
    });

    ui::KeyEvent what{ui::Code::escape};

    ought(stage.top.dispatch(what));

    ought(stage.window->empty());
    same(stage.top.focused_node(), static_cast<ui::Node*>(stage.body.get()));
}

TRIAL("Modal: Tab moves the focus over the willing nodes and wraps around") {
    Stage stage;

    const auto one = std::make_shared<ui::Modal>();
    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Box>(ui::Size{6, 1}, U'a');
    const auto second = std::make_shared<Box>(ui::Size{6, 1}, U'b');

    column->children().add(first);
    column->children().add(second);
    one->children().add(column);

    stage.window->open(one);

    same(stage.top.focused_node(), static_cast<ui::Node*>(first.get()));

    ui::KeyEvent tab{ui::Code::tab};

    ought(stage.top.dispatch(tab));
    same(stage.top.focused_node(), static_cast<ui::Node*>(second.get()));

    ought(stage.top.dispatch(tab));
    same(stage.top.focused_node(), static_cast<ui::Node*>(first.get()));
}

// The down and up arrows move the focus like Tab and Shift+Tab when nothing lower down took them,
// as in the dialogs of Midnight Commander; with a modifier they are swallowed as before.
TRIAL("Modal: the down and up arrows move the focus like Tab") {
    Stage stage;

    const auto one = std::make_shared<ui::Modal>();
    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Box>(ui::Size{6, 1}, U'a');
    const auto second = std::make_shared<Box>(ui::Size{6, 1}, U'b');

    column->children().add(first);
    column->children().add(second);
    one->children().add(column);

    stage.window->open(one);

    ui::KeyEvent down{ui::Code::down};

    ought(stage.top.dispatch(down));
    same(stage.top.focused_node(), static_cast<ui::Node*>(second.get()));

    ui::KeyEvent up{ui::Code::up};

    ought(stage.top.dispatch(up));
    same(stage.top.focused_node(), static_cast<ui::Node*>(first.get()));

    ui::KeyEvent shifted{ui::Key{ui::Code::down, ui::Mods{.shift = true}}};

    ought(stage.top.dispatch(shifted));
    same(stage.top.focused_node(), static_cast<ui::Node*>(first.get()));
}

// --- Division of roles ---------------------------------------------------------------------------

// This is the trial the whole division of roles exists for. Enter pressing the primary button is a
// rule of a form, not of a veil. A bare veil is to swallow it and do nothing; otherwise an open
// list would confirm someone else's window with the same key it uses to choose an item.
TRIAL("Modal: Enter does not press the primary button") {
    Stage stage;

    const auto one = std::make_shared<ui::Modal>();
    const auto press = std::make_shared<ui::Button>("OK");

    int done = 0;

    press->primary(true);

    (void)press->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });

    one->children().add(press);

    stage.window->open(one);

    // We move the focus from the button to the veil itself: a button with the focus knows Enter on
    // its own, and the trial is about what the window does with Enter.
    one->focus();

    ui::KeyEvent what{ui::Code::enter};

    ought(stage.top.dispatch(what));
    same(done, 0);
}

// And in a dialog, that is, in a form-like veil, it is the same. Marking a button primary draws the
// bracket with arrows, and that is where its role ends; the window does not search the tree for the
// button and does not press it for the caller. Enter belongs to whoever has the focus.
TRIAL("Dialog: Enter does not press the primary button either") {
    Stage stage;

    const auto one = std::make_shared<ui::Dialog>();
    const auto press = std::make_shared<ui::Button>("OK");

    int done = 0;

    press->primary(true);

    (void)press->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });

    one->children().add(press);

    stage.window->open(one);

    one->focus();

    ui::KeyEvent what{ui::Code::enter};

    // The veil swallows the key, because it is modal, but presses nothing with it.
    ought(stage.top.dispatch(what));
    same(done, 0);
}

// --- A custom position of the box ----------------------------------------------------------------

// A derived veil places the box where `seat()` says, and nothing else changes: transparency,
// modality and closing stay.
TRIAL("Modal: a derived veil places the box where seat says") {
    Stage stage;

    const auto one = std::make_shared<Aside>(ui::Spot{3, 6});

    one->children().add(std::make_shared<Box>(ui::Size{8, 2}, U'o'));

    stage.window->open(one);

    const View made = stage.top.render({{0, 0}, {40, 10}});

    same(made.at({3, 6}).letter, U'o');
    same(made.at({10, 7}).letter, U'o');

    // Outside the box, what is underneath is still visible.
    same(made.at({2, 6}).letter, U'.');
    same(made.at({3, 5}).letter, U'.');
}

TRIAL("Modal: a derived veil still swallows clicks beside it and Escape") {
    Stage stage;

    const auto one = std::make_shared<Aside>(ui::Spot{3, 6});

    one->children().add(std::make_shared<Box>(ui::Size{8, 2}, U'o'));

    stage.window->open(one);

    (void)stage.top.render({{0, 0}, {40, 10}});

    ui::ClickEvent what{{30, 1}, ui::Mouse::left};

    ought(stage.top.dispatch(what));
    same(stage.body->hit, 0);

    ui::KeyEvent esc{ui::Code::escape};

    ought(stage.top.dispatch(esc));
    ought(!stage.window->empty());
}

// A `CloseEvent` hook has the right to open the next veil: a screen does that when, after OK in the
// options window, the skin cannot be loaded and this has to be said. Opening shifts the vector of
// veils in the middle of removal, and the new veil has remembered the focus in a window that is
// just leaving the tree. Both have to be survived: the closed veil leaves, the new one stays with
// the focus, and after it closes the focus returns to where the first window took it from.
TRIAL("Modal: a close hook may open the next veil, and the focus returns after both") {
    Stage stage;

    // Several veils opened and closed in a row, so that the vector has something to shift.
    for (int round = 0; round < 3; ++round) {
        const auto first = std::make_shared<ui::Modal>();
        const auto inside = std::make_shared<Box>(ui::Size{6, 3}, U'o');

        first->children().add(inside);

        stage.window->open(first);

        same(stage.top.focused_node(), static_cast<ui::Node*>(inside.get()));

        std::shared_ptr<ui::Modal> second;

        (void)first->on<ui::CloseEvent>([&stage, &second](ui::CloseEvent&) {
            second = std::make_shared<ui::Modal>();
            second->children().add(std::make_shared<Box>(ui::Size{4, 2}, U'x'));

            stage.window->open(second);

            return false;
        });

        first->close();

        ought(second != nullptr);
        same(stage.window->many(), std::size_t{1});
        same(stage.window->current(), second.get());
        ought(first->parent() == nullptr);
        ought(stage.top.focused_node() != nullptr);
        ought(stage.top.focused_node() != static_cast<ui::Node*>(stage.body.get()));

        second->close();

        ought(stage.window->empty());
        same(stage.top.focused_node(), static_cast<ui::Node*>(stage.body.get()));
    }
}

// Removing a lower veil in the stack leaves the focus in the top one; it does not pull it down to
// the content underneath, because that is still covered.
TRIAL("Modal: removing a lower veil leaves the focus with the top one") {
    Stage stage;

    const auto lower = std::make_shared<ui::Modal>();
    const auto upper = std::make_shared<ui::Modal>();
    const auto top = std::make_shared<Box>(ui::Size{4, 2}, U'x');

    lower->children().add(std::make_shared<Box>(ui::Size{6, 3}, U'o'));
    upper->children().add(top);

    stage.window->open(lower);
    stage.window->open(upper);

    same(stage.top.focused_node(), static_cast<ui::Node*>(top.get()));

    lower->close();

    same(stage.window->many(), std::size_t{1});
    same(stage.top.focused_node(), static_cast<ui::Node*>(top.get()));

    upper->close();

    same(stage.top.focused_node(), static_cast<ui::Node*>(stage.body.get()));
}

// --- A veil is modal for the focus too -----------------------------------------------------------

// The same content a second time is not a change. Taking it out and putting it back would take its
// focus away, and a caller that sets the content "to be sure" expects nothing to happen.
TRIAL("Modals: the same content a second time does not take the focus away") {
    Stage stage;

    stage.window->content(stage.under);

    same(stage.window->content(), static_cast<ui::Node*>(stage.under.get()));
    same(stage.top.focused_node(), static_cast<ui::Node*>(stage.body.get()));
}

// A node under an open veil may ask for the focus at any moment: in twa the preview sheet does so
// when a file arrives from the pool, though an error message already hangs above it. The request is
// not granted at once, because the keys would go under a window that could then be closed neither
// with Enter nor with Escape. Instead, it becomes where the focus returns after the veil closes.
TRIAL("Modal: a focus request from under a veil takes effect only after it closes") {
    Stage stage;

    const auto other = std::make_shared<Box>(ui::Size{5, 2}, U'y');

    stage.under->children().add(other);

    const auto one = std::make_shared<ui::Modal>();
    const auto inside = std::make_shared<Box>(ui::Size{4, 2}, U'x');

    one->children().add(inside);
    stage.window->open(one);

    same(stage.top.focused_node(), static_cast<ui::Node*>(inside.get()));

    // A request from the content: the focus stays in the veil.
    other->focus();

    same(stage.top.focused_node(), static_cast<ui::Node*>(inside.get()));

    // After closing, the focus goes where the last willing node asked, not where it was taken from.
    one->close();

    same(stage.top.focused_node(), static_cast<ui::Node*>(other.get()));
}

// The covered node gets no focus event at all meanwhile: a flash of `FocusEvent` and `UnfocusEvent`
// would switch the active panel, because panels track it exactly that way.
TRIAL("Modal: a deferred request sends no focus events") {
    Stage stage;

    const auto other = std::make_shared<Box>(ui::Size{5, 2}, U'y');

    stage.under->children().add(other);

    int came = 0;
    int went = 0;

    (void)other->on<ui::FocusEvent>([&came](ui::FocusEvent&) {
        ++came;

        return false;
    });

    (void)other->on<ui::UnfocusEvent>([&went](ui::UnfocusEvent&) {
        ++went;

        return false;
    });

    const auto one = std::make_shared<ui::Modal>();

    one->children().add(std::make_shared<Box>(ui::Size{4, 2}, U'x'));
    stage.window->open(one);

    other->focus();

    same(came, 0);
    same(went, 0);

    one->close();

    same(came, 1);
    same(went, 0);
}

// A request from a lower veil goes to the top one: that one leaves first and gives back the focus.
// A request from the top veil is an ordinary request; nothing defers it.
TRIAL("Modal: a request from a lower veil waits, one from the top veil works at once") {
    Stage stage;

    const auto lower = std::make_shared<ui::Modal>();
    const auto under = std::make_shared<Box>(ui::Size{6, 3}, U'o');
    const auto beside = std::make_shared<Box>(ui::Size{6, 3}, U'p');

    const auto twain = std::make_shared<ui::Column>();

    twain->children().add(under);
    twain->children().add(beside);
    lower->children().add(twain);

    const auto upper = std::make_shared<ui::Modal>();
    const auto top = std::make_shared<Box>(ui::Size{4, 2}, U'x');
    const auto next = std::make_shared<Box>(ui::Size{4, 2}, U'z');

    const auto pair = std::make_shared<ui::Column>();

    pair->children().add(top);
    pair->children().add(next);
    upper->children().add(pair);

    stage.window->open(lower);
    stage.window->open(upper);

    same(stage.top.focused_node(), static_cast<ui::Node*>(top.get()));

    // From the lower veil: it waits.
    beside->focus();

    same(stage.top.focused_node(), static_cast<ui::Node*>(top.get()));

    // In the top one: at once.
    next->focus();

    same(stage.top.focused_node(), static_cast<ui::Node*>(next.get()));

    // After the top veil leaves, the focus goes where the lower one asked.
    upper->close();

    same(stage.top.focused_node(), static_cast<ui::Node*>(beside.get()));

    lower->close();

    same(stage.top.focused_node(), static_cast<ui::Node*>(stage.body.get()));
}

// --- Layers: what veils and windows have in common -----------------------------------------------

// A veil and a window are layers of one kind: their container closes them, and the current one is
// the one on top. The content under the veils is not a layer; it neither counts toward `many()` nor
// is ever current.
TRIAL("Layers: the current one is on top, the content does not count") {
    Stage stage;

    ought(stage.window->empty());
    same(stage.window->many(), std::size_t{0});
    ought(stage.window->current() == nullptr);

    const auto first = std::make_shared<ui::Modal>();
    const auto second = std::make_shared<ui::Modal>();

    stage.window->open(first);
    stage.window->open(second);

    ought(!stage.window->empty());
    same(stage.window->many(), std::size_t{2});
    same(stage.window->current(), static_cast<ui::Modal*>(second.get()));
    ought(second->current());
    ought(!first->current());
}

TRIAL("Layers: close without an argument closes the top one and leaves a foreign layer alone") {
    Stage stage;

    const auto first = std::make_shared<ui::Modal>();
    const auto second = std::make_shared<ui::Modal>();
    const auto stranger = std::make_shared<ui::Modal>();

    stage.window->open(first);
    stage.window->open(second);

    stage.window->close(*stranger);

    same(stage.window->many(), std::size_t{2});

    stage.window->close();

    same(stage.window->many(), std::size_t{1});
    same(stage.window->current(), static_cast<ui::Modal*>(first.get()));
    ought(second->parent() == nullptr);

    stage.window->close();
    stage.window->close();

    ought(stage.window->empty());
}

// A layer outside a container has nothing to close and is not current. It does not throw, because
// closing is sometimes called from hooks that do not know whether the window still hangs.
TRIAL("Layer: outside a container close does nothing and current is false") {
    const auto alone = std::make_shared<ui::Modal>();

    int closed = 0;

    (void)alone->on<ui::CloseEvent>([&closed](ui::CloseEvent&) {
        ++closed;

        return true;
    });

    alone->close();

    same(closed, 0);
    ought(!alone->current());
}

TRIAL("Layer: closing is announced once and the layer leaves the tree") {
    Stage stage;

    const auto one = std::make_shared<ui::Modal>();

    int closed = 0;

    (void)one->on<ui::CloseEvent>([&closed, &one](ui::CloseEvent&) {
        ++closed;

        ought(one->parent() != nullptr);

        return true;
    });

    stage.window->open(one);

    one->close();
    one->close();

    same(closed, 1);
    ought(one->parent() == nullptr);
    ought(stage.window->empty());
}

// An opening that failed leaves no trace: a layer that already hangs elsewhere cannot be attached a
// second time, and the list of openings must not count it then.
TRIAL("Layers: a failed opening does not stay on the list") {
    Stage stage;

    const auto elsewhere = std::make_shared<ui::Modals>();
    const auto one = std::make_shared<ui::Modal>();

    elsewhere->open(one);

    threw(stage.window->open(one));

    ought(stage.window->empty());
    ought(stage.window->current() == nullptr);
}
