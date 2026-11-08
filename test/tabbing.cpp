#include "tal/testing.hpp"

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
std::string read(const Snapshot& one, int y) {
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

    const Snapshot made = top.render({{0, 0}, {40, 6}});

    SAME(read(made, 0), std::string{"        Alpha     Beta     Gamma"});
    SAME(read(made, 1), line(7, 7, 26));
    SAME(read(made, 2), std::string{"page of Alpha"});
    SAME(three.tabs->which(), std::size_t{0});

    // The name shown in its role, the others plain, and each key letter in the key role.
    OUGHT(made.at({9, 0}).style == top.skin().style("talcite.tab.current"));
    OUGHT(made.at({19, 0}).style == top.skin().style("talcite.tab"));
    OUGHT(made.at({8, 0}).style != top.skin().style("talcite.tab.current"));
    OUGHT(made.at({18, 0}).style != top.skin().style("talcite.tab"));
}

TRIAL("Tabs: they ask for the strip and the largest page") {
    Three three;

    three.gamma.tab->children().front()->children().add(std::make_shared<ui::Span>("a longer line on the page of Gamma"));

    const ui::Size asked = three.tabs->wants({80, 20});

    SAME(asked.wide, static_cast<int>(std::string{"a longer line on the page of Gamma"}.size()));
    SAME(asked.high, 2 + 3);

    SAME(three.tabs->wants({20, 4}).wide, 20);
    SAME(three.tabs->wants({20, 4}).high, 4);

    // Drawn once, the pages of Beta and Gamma are kept aside, and Gamma's still counts.
    ui::Root top;

    top.children().add(three.tabs);

    (void)top.render({{0, 0}, {80, 20}});

    OUGHT(three.gamma.button->root() == nullptr);
    SAME(three.tabs->wants({80, 20}).wide, asked.wide);
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

    OUGHT(three.alpha.button->root() != nullptr);
    OUGHT(three.beta.button->root() == nullptr);
    OUGHT(three.gamma.button->root() == nullptr);

    int bob = 0;
    const ui::Hook counted = three.beta.button->on<ui::PickEvent>([&bob](ui::PickEvent&) {
        ++bob;

        return true;
    });

    ui::KeyEvent hidden{ui::Key{U'O'}};

    (void)top.dispatch(hidden);

    SAME(bob, 0);

    // Its tab's key shows the page, and then the button answers.
    ui::KeyEvent beta{ui::Key{U'B'}};

    OUGHT(top.dispatch(beta));
    SAME(three.tabs->which(), std::size_t{1});
    OUGHT(three.alpha.button->root() == nullptr);
    OUGHT(three.beta.button->root() != nullptr);

    ui::KeyEvent again{ui::Key{U'O'}};

    OUGHT(top.dispatch(again));
    SAME(bob, 1);
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

    OUGHT(top.dispatch(on_gamma));
    SAME(three.tabs->which(), std::size_t{2});
    SAME(picked, 1);

    const Snapshot made = top.render({{0, 0}, {40, 6}});

    SAME(read(made, 2), std::string{"page of Gamma"});

    // The line under a name counts too, and a click on the name shown changes nothing: its tab does
    // not say it again.
    int beta = 0;
    const ui::Hook betas = three.beta.tab->on<ui::PickEvent>([&beta](ui::PickEvent&) {
        ++beta;

        return false;
    });

    ui::ClickEvent under_beta{{19, 1}, ui::Mouse::left};

    OUGHT(top.dispatch(under_beta));
    SAME(three.tabs->which(), std::size_t{1});
    SAME(beta, 1);

    ui::ClickEvent shown{{19, 0}, ui::Mouse::left};

    OUGHT(top.dispatch(shown));
    SAME(three.tabs->which(), std::size_t{1});
    SAME(picked, 1);
    SAME(beta, 1);

    // Below the strip the page's button takes the click, not a hidden page that stood there before.
    (void)top.render({{0, 0}, {40, 6}});

    int bob = 0;
    const ui::Hook counted = three.beta.button->on<ui::ClickEvent>([&bob](ui::ClickEvent&) {
        ++bob;

        return true;
    });

    ui::ClickEvent on_bob{{2, 3}, ui::Mouse::left};

    OUGHT(top.dispatch(on_bob));
    SAME(bob, 1);

    // Back on Alpha: Beta, later among the tabs, would win the spot if it kept the place it had.
    three.tabs->which(0);

    (void)top.render({{0, 0}, {40, 6}});

    int ala = 0;
    const ui::Hook alas = three.alpha.button->on<ui::ClickEvent>([&ala](ui::ClickEvent&) {
        ++ala;

        return true;
    });

    ui::ClickEvent on_ala{{2, 3}, ui::Mouse::left};

    OUGHT(top.dispatch(on_ala));
    SAME(ala, 1);
    SAME(bob, 1);

    // An empty spot of the page under Beta's name is not Beta's name: nobody takes the click.
    (void)top.render({{0, 0}, {40, 6}});

    ui::ClickEvent below{{19, 4}, ui::Mouse::left};

    OUGHT(!top.dispatch(below));
    SAME(three.tabs->which(), std::size_t{0});
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

    OUGHT(top.dispatch(next));
    SAME(three.tabs->which(), std::size_t{1});

    // The focus under the tabs: the dialog passes the key round.
    outside->focus();

    ui::KeyEvent back{ui::Key{ui::Code::pageup, ui::Mods{.ctrl = true}}};

    OUGHT(top.dispatch(back));
    SAME(three.tabs->which(), std::size_t{0});

    ui::KeyEvent round{ui::Key{ui::Code::pageup, ui::Mods{.ctrl = true}}};

    OUGHT(top.dispatch(round));
    SAME(three.tabs->which(), std::size_t{2});

    // Without Ctrl, or with more than Ctrl, the keys are not the tabs'.
    three.tabs->which(0);
    three.alpha.button->focus();

    ui::KeyEvent plain{ui::Key{ui::Code::pagedown}};
    ui::KeyEvent shifted{ui::Key{ui::Code::pagedown, ui::Mods{.ctrl = true, .shift = true}}};

    (void)top.dispatch(plain);
    (void)top.dispatch(shifted);
    SAME(three.tabs->which(), std::size_t{0});
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

    OUGHT(second->focused());

    three.alpha.button->focus();
    three.tabs->which(1);

    OUGHT(three.beta.button->focused());

    outside->focus();
    three.tabs->which(2);

    OUGHT(outside->focused());
}

TRIAL("Tabs: too narrow for all the names, the one shown stands alone between arrows") {
    ui::Root top;
    Three three;

    top.children().add(three.tabs);

    three.tabs->which(1);

    const Snapshot made = top.render({{0, 0}, {14, 5}});

    SAME(read(made, 0), std::string{"  <  Beta  >"});
    SAME(read(made, 1), line(4, 6, 4));

    ui::ClickEvent left{{3, 0}, ui::Mouse::left};

    OUGHT(top.dispatch(left));
    SAME(three.tabs->which(), std::size_t{0});

    (void)top.render({{0, 0}, {14, 5}});

    ui::ClickEvent right{{12, 0}, ui::Mouse::left};

    OUGHT(top.dispatch(right));
    SAME(three.tabs->which(), std::size_t{1});

    // The arrows turn from the name shown, whatever name would stand there were they all in a row.
    three.tabs->which(2);

    (void)top.render({{0, 0}, {14, 5}});

    ui::ClickEvent back{{1, 0}, ui::Mouse::left};

    OUGHT(top.dispatch(back));
    SAME(three.tabs->which(), std::size_t{1});
}

TRIAL("Tabs: only tabs go in, and a tab holds one page") {
    const auto tabs = std::make_shared<ui::Tabs>();
    const auto tab = std::make_shared<ui::Tab>("Alpha");

    OUGHT(tabs->accepts(*tab));
    OUGHT(!tabs->accepts(ui::Span{"x"}));
    SAME(tab->capacity(), 1);

    // A tab's key and a step past the end are nothing without tabs.
    tabs->step(1);
    tabs->which(3);

    SAME(tabs->which(), std::size_t{0});
}

// A tab's name stands in the strip, which the tabs paint, not over the tab's own page: so a renamed
// tab, shown or not, shows its new name at the next frame, and the names beside it move with it.
TRIAL("Tabs: a renamed tab shows its new name in the strip at the next frame") {
    ui::Root top;
    Three three;

    top.children().add(three.tabs);

    (void)top.render({{0, 0}, {40, 6}});

    three.beta.tab->text("Bravo");

    const std::string row = read(top.render({{0, 0}, {40, 6}}), 0);

    OUGHT(row.find("Beta") == std::string::npos);
    OUGHT(row.find("Alpha") < row.find("Bravo"));
    OUGHT(row.find("Bravo") < row.find("Gamma"));
    OUGHT(row.find("Gamma") != std::string::npos);
}

// A number past the last tab is cut to the last one, as in a list and a menu, so the program never
// leaves the strip pointing at a page that is not there.
TRIAL("Tabs: which past the last shows the last") {
    ui::Root top;
    Three three;

    top.children().add(three.tabs);

    three.tabs->which(99);

    SAME(three.tabs->which(), std::size_t{2});
    SAME(read(top.render({{0, 0}, {40, 6}}), 2), std::string{"page of Gamma"});
}

// Without tabs there is nothing to show, and nothing breaks.
TRIAL("Tabs: which without tabs does nothing") {
    ui::Root top;

    const auto tabs = std::make_shared<ui::Tabs>();

    top.children().add(tabs);

    tabs->which(3);

    SAME(tabs->which(), std::size_t{0});
}
