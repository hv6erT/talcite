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

// **Dostępność to jedna domieszka, a jej zmiana — zdarzenie.** `EnabledNode` trzyma znacznik,
// z niego liczy `focusable()` i przy wyłączeniu oddaje ognisko. Kto chce na zmianę odpowiedzieć —
// widżet przez `handle`, wołający przez `on<>` — słucha `EnableEvent` i `DisableEvent`.

namespace {

// Klawisze aktywne rozdaje dialog, więc przyciski stoją w dialogu, jak w każdym oknie.
struct Stage {
    ui::Root top;
    std::shared_ptr<ui::Modals> window = std::make_shared<ui::Modals>();
    std::shared_ptr<ui::Dialog> dialog = std::make_shared<ui::Dialog>();
    std::shared_ptr<ui::Column> column = std::make_shared<ui::Column>();
    std::shared_ptr<ui::Button> one = std::make_shared<ui::Button>("Tak");
    std::shared_ptr<ui::Button> other = std::make_shared<ui::Button>("Nie");

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

TRIAL("EnabledNode: zmiana ogłasza się raz, ta sama wartość nie ogłasza nic") {
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

TRIAL("EnabledNode: wyłączony nie jest do wzięcia, włączony znów jest") {
    Stage stage;

    ought(stage.one->focusable());

    stage.one->enabled(false);

    ought(!stage.one->focusable());

    stage.one->enabled(true);

    ought(stage.one->focusable());
}

TRIAL("EnabledNode: wyłączenie bąbelkuje i mówi, skąd przyszło") {
    Stage stage;

    const ui::Handler* from = nullptr;

    (void)stage.column->on<ui::DisableEvent>([&from](ui::DisableEvent& what) {
        from = what.target();

        return true;
    });

    stage.other->enabled(false);

    same(static_cast<const void*>(from), static_cast<const void*>(stage.other.get()));
}

// Kolejność jest umową, na której stoi `Select`: zdarzenie przychodzi, **póki ognisko jeszcze jest**
// na wyłączanym węźle, a oddanie ogniska przychodzi po nim.
TRIAL("EnabledNode: zdarzenie przychodzi, zanim ognisko odejdzie") {
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

// Uchwyt, który w odpowiedzi włącza z powrotem, wygrywa: ognisko zostaje, bo węzeł znów jest
// do wzięcia.
TRIAL("EnabledNode: przywrócony w uchwycie zostaje z ogniskiem") {
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

TRIAL("EnabledNode: wyłączony nie bierze klawisza aktywnego, włączony bierze") {
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
