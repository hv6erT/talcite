#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/encoding.hpp"
#include "talcite/modals.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"
#include "talcite/tab.hpp"
#include "talcite/tabs.hpp"

using namespace talcite;

namespace {

// The text of row `y` of the view, without trailing spaces.
std::string read(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter != 0) made += talcite::spelled(letter);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

// The line under the names: `before` thin cells, `under` thick ones, `after` thin ones.
std::string line(int before, int under, int after) {
    std::string made;

    for (int at = 0; at < before + under + after; ++at) made += at >= before && at < before + under ? "━" : "─";

    return made;
}

// A tab named `name` with the key `key`, whose page is a column with a span and a button.
struct Paged {
    std::shared_ptr<ui::Tab> tab;
    std::shared_ptr<ui::Button> button;
};

// A tab whose page says `said` and holds a button `pressed`, the button's key being `letter`.
Paged paged(const std::string& name, const std::string& said, const std::string& pressed, char32_t letter) {
    Paged made{std::make_shared<ui::Tab>(name), std::make_shared<ui::Button>(pressed)};

    const auto page = std::make_shared<ui::Column>();

    page->children().add(std::make_shared<ui::Span>(said));
    page->children().add(made.button);

    made.button->keys().add(letter);
    made.tab->children().add(page);

    return made;
}

// Three tabs, Alpha with Ala, Beta with Bob, Gamma with Gil, and their pages; keys A, B and G.
struct Three {
    std::shared_ptr<ui::Tabs> tabs = std::make_shared<ui::Tabs>();
    Paged alpha = paged("Alpha", "page of Alpha", "Ala", U'L');
    Paged beta = paged("Beta", "page of Beta", "Bob", U'O');
    Paged gamma = paged("Gamma", "page of Gamma", "Gil", U'I');

    // The tabs with their keys.
    Three() {
        alpha.tab->keys().add(U'A');
        beta.tab->keys().add(U'B');
        gamma.tab->keys().add(U'G');

        tabs->children().add({alpha.tab, beta.tab, gamma.tab});
    }
};

}

TRIAL("Tabs: the names in the middle, a stroke under the one shown, its page below") {
    ui::Root top;
    Three three;

    top.children().add(three.tabs);

    const View made = top.render({{0, 0}, {40, 6}});

    same(read(made, 0), std::string{"        Alpha     Beta     Gamma"});
    same(read(made, 1), line(7, 7, 26));
    same(read(made, 2), std::string{"page of Alpha"});
    same(three.tabs->which(), std::size_t{0});

    // The name shown in its role, the others plain, and each key letter in the key role.
    ought(made.at({9, 0}).style == top.skin().style("talcite.tab.current"));
    ought(made.at({19, 0}).style == top.skin().style("talcite.tab"));
    ought(made.at({8, 0}).style != top.skin().style("talcite.tab.current"));
    ought(made.at({18, 0}).style != top.skin().style("talcite.tab"));
}

TRIAL("Tabs: they ask for the strip and the largest page") {
    Three three;

    three.gamma.tab->children().front()->children().add(std::make_shared<ui::Span>("a longer line on the page of Gamma"));

    const ui::Size asked = three.tabs->wants({80, 20});

    same(asked.wide, static_cast<int>(std::string{"a longer line on the page of Gamma"}.size()));
    same(asked.high, 2 + 3);

    same(three.tabs->wants({20, 4}).wide, 20);
    same(three.tabs->wants({20, 4}).high, 4);

    // Drawn once, the pages of Beta and Gamma are kept aside, and Gamma's still counts.
    ui::Root top;

    top.children().add(three.tabs);

    (void)top.render({{0, 0}, {80, 20}});

    ought(three.gamma.button->root() == nullptr);
    same(three.tabs->wants({80, 20}).wide, asked.wide);
}

TRIAL("Tabs: only the page shown is in the tree, so a hidden page's button does not answer its key") {
    ui::Root top;
    Three three;
    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();

    dialog->children().add(three.tabs);
    top.children().add(window);
    window->open(dialog);

    (void)top.render({{0, 0}, {40, 10}});

    ought(three.alpha.button->root() != nullptr);
    ought(three.beta.button->root() == nullptr);
    ought(three.gamma.button->root() == nullptr);

    int bob = 0;
    const ui::Hook counted = three.beta.button->on<ui::PickEvent>([&bob](ui::PickEvent&) {
        ++bob;

        return true;
    });

    ui::KeyEvent hidden{ui::Key{U'O'}};

    (void)top.dispatch(hidden);

    same(bob, 0);

    // Its tab's key shows the page, and then the button answers.
    ui::KeyEvent beta{ui::Key{U'B'}};

    ought(top.dispatch(beta));
    same(three.tabs->which(), std::size_t{1});
    ought(three.alpha.button->root() == nullptr);
    ought(three.beta.button->root() != nullptr);

    ui::KeyEvent again{ui::Key{U'O'}};

    ought(top.dispatch(again));
    same(bob, 1);
}

TRIAL("Tabs: a click on a name shows its page and the tab says so; a click below is the page's") {
    ui::Root top;
    Three three;

    top.children().add(three.tabs);

    int picked = 0;
    const ui::Hook heard = three.gamma.tab->on<ui::PickEvent>([&picked](ui::PickEvent&) {
        ++picked;

        return false;
    });

    (void)top.render({{0, 0}, {40, 6}});

    ui::ClickEvent on_gamma{{28, 0}, ui::Mouse::left};

    ought(top.dispatch(on_gamma));
    same(three.tabs->which(), std::size_t{2});
    same(picked, 1);

    const View made = top.render({{0, 0}, {40, 6}});

    same(read(made, 2), std::string{"page of Gamma"});

    // The line under a name counts too, and a click on the name shown changes nothing: its tab does
    // not say it again.
    int beta = 0;
    const ui::Hook betas = three.beta.tab->on<ui::PickEvent>([&beta](ui::PickEvent&) {
        ++beta;

        return false;
    });

    ui::ClickEvent under_beta{{19, 1}, ui::Mouse::left};

    ought(top.dispatch(under_beta));
    same(three.tabs->which(), std::size_t{1});
    same(beta, 1);

    ui::ClickEvent shown{{19, 0}, ui::Mouse::left};

    ought(top.dispatch(shown));
    same(three.tabs->which(), std::size_t{1});
    same(picked, 1);
    same(beta, 1);

    // Below the strip the page's button takes the click, not a hidden page that stood there before.
    (void)top.render({{0, 0}, {40, 6}});

    int bob = 0;
    const ui::Hook counted = three.beta.button->on<ui::ClickEvent>([&bob](ui::ClickEvent&) {
        ++bob;

        return true;
    });

    ui::ClickEvent on_bob{{2, 3}, ui::Mouse::left};

    ought(top.dispatch(on_bob));
    same(bob, 1);

    // Back on Alpha: Beta, later among the tabs, would win the spot if it kept the place it had.
    three.tabs->which(0);

    (void)top.render({{0, 0}, {40, 6}});

    int ala = 0;
    const ui::Hook alas = three.alpha.button->on<ui::ClickEvent>([&ala](ui::ClickEvent&) {
        ++ala;

        return true;
    });

    ui::ClickEvent on_ala{{2, 3}, ui::Mouse::left};

    ought(top.dispatch(on_ala));
    same(ala, 1);
    same(bob, 1);

    // An empty spot of the page under Beta's name is not Beta's name: nobody takes the click.
    (void)top.render({{0, 0}, {40, 6}});

    ui::ClickEvent below{{19, 4}, ui::Mouse::left};

    ought(!top.dispatch(below));
    same(three.tabs->which(), std::size_t{0});
}

TRIAL("Tabs: Ctrl+PgDn and Ctrl+PgUp go round, from the page and from a dialog's broadcast") {
    ui::Root top;
    Three three;
    const auto window = std::make_shared<ui::Modals>();
    const auto dialog = std::make_shared<ui::Dialog>();
    const auto column = std::make_shared<ui::Column>();
    const auto outside = std::make_shared<ui::Button>("Close");

    column->children().add({three.tabs, outside});
    dialog->children().add(column);
    top.children().add(window);
    window->open(dialog);

    (void)top.render({{0, 0}, {40, 10}});

    // The focus on the page: the key comes up to the tabs.
    three.alpha.button->focus();

    ui::KeyEvent next{ui::Key{ui::Code::pagedown, ui::Mods{.ctrl = true}}};

    ought(top.dispatch(next));
    same(three.tabs->which(), std::size_t{1});

    // The focus under the tabs: the dialog passes the key round.
    outside->focus();

    ui::KeyEvent back{ui::Key{ui::Code::pageup, ui::Mods{.ctrl = true}}};

    ought(top.dispatch(back));
    same(three.tabs->which(), std::size_t{0});

    ui::KeyEvent round{ui::Key{ui::Code::pageup, ui::Mods{.ctrl = true}}};

    ought(top.dispatch(round));
    same(three.tabs->which(), std::size_t{2});

    // Without Ctrl, or with more than Ctrl, the keys are not the tabs'.
    three.tabs->which(0);
    three.alpha.button->focus();

    ui::KeyEvent plain{ui::Key{ui::Code::pagedown}};
    ui::KeyEvent shifted{ui::Key{ui::Code::pagedown, ui::Mods{.ctrl = true, .shift = true}}};

    (void)top.dispatch(plain);
    (void)top.dispatch(shifted);
    same(three.tabs->which(), std::size_t{0});
}

TRIAL("Tabs: the focus on the page that goes moves to the page that comes; elsewhere it stays") {
    ui::Root top;
    Three three;
    const auto column = std::make_shared<ui::Column>();
    const auto outside = std::make_shared<ui::Button>("Close");

    column->children().add({three.tabs, outside});
    top.children().add(column);

    (void)top.render({{0, 0}, {40, 10}});

    // The page shown again keeps the focus where it is, not on its first stop.
    const auto second = std::make_shared<ui::Button>("Second");

    three.alpha.tab->children().front()->children().add(second);

    (void)top.render({{0, 0}, {40, 10}});

    second->focus();
    three.tabs->which(0);

    ought(second->focused());

    three.alpha.button->focus();
    three.tabs->which(1);

    ought(three.beta.button->focused());

    outside->focus();
    three.tabs->which(2);

    ought(outside->focused());
}

TRIAL("Tabs: too narrow for all the names, the one shown stands alone between arrows") {
    ui::Root top;
    Three three;

    top.children().add(three.tabs);

    three.tabs->which(1);

    const View made = top.render({{0, 0}, {14, 5}});

    same(read(made, 0), std::string{"  <  Beta  >"});
    same(read(made, 1), line(4, 6, 4));

    ui::ClickEvent left{{3, 0}, ui::Mouse::left};

    ought(top.dispatch(left));
    same(three.tabs->which(), std::size_t{0});

    (void)top.render({{0, 0}, {14, 5}});

    ui::ClickEvent right{{12, 0}, ui::Mouse::left};

    ought(top.dispatch(right));
    same(three.tabs->which(), std::size_t{1});

    // The arrows turn from the name shown, whatever name would stand there were they all in a row.
    three.tabs->which(2);

    (void)top.render({{0, 0}, {14, 5}});

    ui::ClickEvent back{{1, 0}, ui::Mouse::left};

    ought(top.dispatch(back));
    same(three.tabs->which(), std::size_t{1});
}

TRIAL("Tabs: only tabs go in, and a tab holds one page") {
    const auto tabs = std::make_shared<ui::Tabs>();
    const auto tab = std::make_shared<ui::Tab>("Alpha");

    ought(tabs->accepts(*tab));
    ought(!tabs->accepts(ui::Span{"x"}));
    same(tab->capacity(), 1);

    // A tab's key and a step past the end are nothing without tabs.
    tabs->step(1);
    tabs->which(3);

    same(tabs->which(), std::size_t{0});
}
