#include "proving.hpp"

#include <memory>

#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/root.hpp"
#include "talcite/veil.hpp"
#include "talcite/veilwindow.hpp"

using namespace talcite;

namespace {

class Box : public ui::Node {
public:
    Box(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    [[nodiscard]] bool focusable() const override { return true; }

    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

    int hit = 0;

protected:
    View paint(ui::Size given, ui::Area seen) override {
        View made{seen.size};

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                made.letter({x - seen.at.x, y - seen.at.y}, what_, Style{});

        return made;
    }

    bool react(ui::ClickEvent&) override {
        ++hit;

        return true;
    }

private:
    ui::Size wants_;
    char32_t what_ = U'?';
};

// Zasłona, która stawia pudełko tam, gdzie jej każą, zamiast na środku. Po to właśnie `box()` jest
// wirtualne: to jedyne, czym zasłony różnią się między sobą.
class Aside : public ui::Veil {
public:
    explicit Aside(ui::Spot where) : ui::Veil(ui::VeilStyle{false}), where_(where) {}

protected:
    [[nodiscard]] ui::Area box(ui::Size given) const override {
        if (children().empty()) return {};

        ui::Size wants = children().front()->wants(given);

        return {where_, wants};
    }

private:
    ui::Spot where_;
};

// Najmniejsze potrzebne otoczenie: okno z treścią pod spodem i zasłona nad nią.
struct Stage {
    ui::Root top;
    std::shared_ptr<ui::VeilWindow> window = std::make_shared<ui::VeilWindow>();
    std::shared_ptr<Box> body = std::make_shared<Box>(ui::Size{40, 10}, U'.');

    Stage() {
        window->body()->children().add(body);
        top.children().add(window);
        body->focus();
    }
};

}

// --- Co zasłona wie o sobie ----------------------------------------------------------------------

// Zasłona **jest ekranem**, choć widać na niej samo pudełko. Z tego jednego faktu bierze się cała
// modalność, więc pilnujemy go wprost.
TRIAL("Veil: zajmuje wszystko, co dostała, i leży nad rodzeństwem") {
    const ui::Veil one;

    same(one.wants({40, 10}), (ui::Size{40, 10}));
    same(one.layer(), 1);
    same(one.takes(), 1);
    ought(one.focusable());
}

TRIAL("Veil: pudełko staje na środku") {
    Stage stage;

    const auto one = std::make_shared<ui::Veil>(ui::VeilStyle{false});

    one->children().add(std::make_shared<Box>(ui::Size{10, 4}, U'o'));

    stage.window->open(one);

    const View made = stage.top.render({{0, 0}, {40, 10}});

    // (40−10)/2 = 15, (10−4)/2 = 3.
    same(made.at({15, 3}).letter, U'o');
    same(made.at({24, 6}).letter, U'o');
    same(made.at({14, 3}).letter, U'.');
}

// --- Modalność -----------------------------------------------------------------------------------

TRIAL("Veil: kliknięcie obok pudełka nie dochodzi pod spód") {
    Stage stage;

    const auto one = std::make_shared<ui::Veil>(ui::VeilStyle{false});

    one->children().add(std::make_shared<Box>(ui::Size{10, 4}, U'o'));

    stage.window->open(one);

    (void)stage.top.render({{0, 0}, {40, 10}});

    ui::ClickEvent what{{1, 1}, ui::Mouse::left};

    ought(stage.top.deal(what));
    same(stage.body->hit, 0);
}

// **Escape'a zasłona nie bierze.** Zamykał okno — czyli wykonywał czynność, której wołający nie
// miał jak odwołać. Połyka go jak każdy inny klawisz, bo połykanie niczego nie robi, a modalność
// inaczej się nie wyrazi.
TRIAL("Veil: Esc niczego nie zamyka, tylko zostaje połknięty") {
    Stage stage;

    const auto one = std::make_shared<ui::Veil>();
    const auto inside = std::make_shared<Box>(ui::Size{6, 3}, U'o');

    one->children().add(inside);

    stage.window->open(one);

    same(stage.top.focus(), static_cast<ui::Node*>(inside.get()));

    ui::KeyEvent what{ui::Code::escape};

    ought(stage.top.deal(what));

    ought(!stage.window->veils().empty());
    same(stage.top.focus(), static_cast<ui::Node*>(inside.get()));
}

// Kto chce „Esc zamyka", wiesza uchwyt — i wtedy działa, bo uchwyt widzi klawisz niezależnie od
// tego, że zasłona go połyka.
TRIAL("Veil: zamykanie Escapem da się dołożyć uchwytem") {
    Stage stage;

    const auto one = std::make_shared<ui::Veil>();
    const auto inside = std::make_shared<Box>(ui::Size{6, 3}, U'o');

    one->children().add(inside);

    stage.window->open(one);

    const std::weak_ptr<ui::Veil> weak = one;

    (void)one->on<ui::KeyEvent>([weak](ui::KeyEvent& what) {
        if (what.key().code() != ui::Code::escape) return false;

        if (const auto mine = weak.lock()) mine->close();

        return true;
    });

    ui::KeyEvent what{ui::Code::escape};

    ought(stage.top.deal(what));

    ought(stage.window->veils().empty());
    same(stage.top.focus(), static_cast<ui::Node*>(stage.body.get()));
}

TRIAL("Veil: Tab wodzi ogniskiem po chętnych i zawija") {
    Stage stage;

    const auto one = std::make_shared<ui::Veil>();
    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Box>(ui::Size{6, 1}, U'a');
    const auto second = std::make_shared<Box>(ui::Size{6, 1}, U'b');

    column->children().add(first);
    column->children().add(second);
    one->children().add(column);

    stage.window->open(one);

    same(stage.top.focus(), static_cast<ui::Node*>(first.get()));

    ui::KeyEvent tab{ui::Code::tab};

    ought(stage.top.deal(tab));
    same(stage.top.focus(), static_cast<ui::Node*>(second.get()));

    ought(stage.top.deal(tab));
    same(stage.top.focus(), static_cast<ui::Node*>(first.get()));
}

// --- Podział ------------------------------------------------------------------------------------

// **To jest ta próba, dla której cały podział.** Enter naciskający przycisk podstawowy to reguła
// formularza, nie zasłony. Goła zasłona ma go połknąć i nie robić nic — inaczej rozwinięta lista
// zatwierdzałaby cudze okno tym samym klawiszem, którym wybiera pozycję.
TRIAL("Veil: Enter nie naciska przycisku podstawowego") {
    Stage stage;

    const auto one = std::make_shared<ui::Veil>();
    const auto press = std::make_shared<ui::Button>("OK");

    int done = 0;

    press->primary(true);

    (void)press->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;

        return true;
    });

    one->children().add(press);

    stage.window->open(one);

    // Ognisko odsuwamy z przycisku na samą zasłonę: przycisk z ogniskiem zna Enter sam z siebie,
    // a próba jest o tym, co robi z Enterem **okno**.
    one->focus();

    ui::KeyEvent what{ui::Code::enter};

    ought(stage.top.deal(what));
    same(done, 0);
}

// A w dialogu — czyli w zasłonie formularzowej — jest **tak samo**. Oznaczenie podstawowego rysuje
// klamrę ze strzałkami i na tym kończy się jego rola; okno nie szuka przycisku po drzewie i nie
// naciska go za wołającego. Enter należy do tego, kto ma ognisko.
TRIAL("Dialog: Enter też nie naciska przycisku podstawowego") {
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

    // Zasłona klawisz połyka, bo jest modalna — ale nic nim nie naciska.
    ought(stage.top.deal(what));
    same(done, 0);
}

// --- Własne położenie pudełka --------------------------------------------------------------------

// Zasłona pochodna stawia pudełko tam, gdzie każe jej `box()` — i **nic poza tym się nie zmienia**:
// przezroczystość, modalność i zamykanie zostają.
TRIAL("Veil: pochodna stawia pudełko tam, gdzie każe box") {
    Stage stage;

    const auto one = std::make_shared<Aside>(ui::Spot{3, 6});

    one->children().add(std::make_shared<Box>(ui::Size{8, 2}, U'o'));

    stage.window->open(one);

    const View made = stage.top.render({{0, 0}, {40, 10}});

    same(made.at({3, 6}).letter, U'o');
    same(made.at({10, 7}).letter, U'o');

    // Poza pudełkiem dalej widać to, co pod spodem.
    same(made.at({2, 6}).letter, U'.');
    same(made.at({3, 5}).letter, U'.');
}

TRIAL("Veil: pochodna dalej połyka kliknięcia obok i Escape") {
    Stage stage;

    const auto one = std::make_shared<Aside>(ui::Spot{3, 6});

    one->children().add(std::make_shared<Box>(ui::Size{8, 2}, U'o'));

    stage.window->open(one);

    (void)stage.top.render({{0, 0}, {40, 10}});

    ui::ClickEvent what{{30, 1}, ui::Mouse::left};

    ought(stage.top.deal(what));
    same(stage.body->hit, 0);

    ui::KeyEvent esc{ui::Code::escape};

    ought(stage.top.deal(esc));
    ought(!stage.window->veils().empty());
}
