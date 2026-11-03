#include "talcite/tabs.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"
#include "talcite/tab.hpp"

#include "marking.hpp"
#include "stops.hpp"

// Tabs: pages one at a time, under a strip with their names. The names stand in a row in the
// middle, and under them runs a thin line with a thick stroke under the name shown; when they do
// not fit side by side, even a cell apart, only the shown one stands, between two arrows, and a
// click on an arrow turns that way. A click on a name, its key (Alt and the letter in a dialog) or
// Ctrl+PgUp and Ctrl+PgDn change the page; the page that goes takes the focus with it to the first
// stop of the page that comes, when the focus was there. The tab that comes up sends a `PickEvent`.
//
// Only the page shown is in the tree (`Tab::shown`): the others' buttons must not answer their keys
// and the focus must not walk into them. Hidden tabs are drawn with no room, so that none keeps the
// place it had when it was shown and takes the mouse from the page now there. The tabs ask for the
// largest page, so the window keeps its size when the page changes.
//
// Roles: talcite.tab for a name, .current for the name shown and its stroke, .key for its letter;
// the line is talcite.frame, the arrows the glyphs talcite.tab.left and talcite.tab.right. The
// strip was the tabs of twa's About window first.
namespace talcite::ui {

namespace {

// The rows of the strip: the names and the line under them.
constexpr int strip_high = 2;

// The cells between two names when there is room; fewer, down to one, when there is not.
constexpr int spacing = 3;

// The tabs among the children, in order; `Tabs` accepts nothing else.
template <typename Of>
std::vector<Tab*> listed(const Of& children) {
    std::vector<Tab*> made;

    for (const std::shared_ptr<Node>& one : children) made.push_back(static_cast<Tab*>(one.get()));

    return made;
}

// The width of a name with a cell on both sides.
int named_wide(const Tab& one) { return measured(one.text()).wide + 2; }

// The width of all the names with their cells and `gap` cells between them.
int labels_wide(const std::vector<Tab*>& all, int gap) {
    int made = 0;

    for (const Tab* one : all) made += named_wide(*one);

    return made + gap * static_cast<int>(all.size() - 1);
}

// Whether the names stand side by side in `wide` cells, a cell apart at least.
bool fitting(const std::vector<Tab*>& all, int wide) { return labels_wide(all, 1) <= wide; }

// The gap between the names in `wide` cells: `spacing`, or less down to one cell.
int gapped(const std::vector<Tab*>& all, int wide) {
    const int much = static_cast<int>(all.size()) - 1;

    if (much <= 0) return spacing;

    return std::clamp((wide - labels_wide(all, 0)) / much, 1, spacing);
}

// Where the shown name stands alone in `wide` cells, with its cells: from and up to, in the middle,
// two cells clear of the edge for the arrow.
std::pair<int, int> lone(const Tab& shown, int wide) {
    const int label = named_wide(shown);
    const int from = std::max(2, (wide - label) / 2);

    return {from, from + label};
}

// Whether `one` is `under` or lies under it.
bool within(const Node* one, const Node& under) {
    for (const Handler* at = one; at != nullptr; at = at->parent())
        if (at == &under) return true;

    return false;
}

}

// Tabs without tabs yet, in the role talcite.tab.
Tabs::Tabs() { role("talcite.tab"); }

// Shows the page of tab `at` (counted from zero); nothing for a tab that is not there. The focus on
// the page that goes moves to the first stop of the page that comes, and the tab that comes up
// sends a `PickEvent`; the tab shown already only puts the pages where they belong.
void Tabs::which(std::size_t at) {
    const std::vector<Tab*> all = listed(children());

    if (at >= all.size()) return;

    const bool moved = at != which_;
    const Root* top = root();
    const Node* had = top != nullptr ? top->focused_node() : nullptr;
    const bool carried = moved && had != nullptr && which_ < all.size() && within(had, *all[which_]);

    for (std::size_t one = 0; one < all.size(); ++one) all[one]->shown(one == at);

    which_ = at;

    changed();

    if (carried)
        if (const std::vector<Node*> these = stops(*all[at]); !these.empty()) these.front()->focus();

    if (moved) (void)all[at]->trigger(PickEvent{});
}

// Shows the page `by` tabs further, to the right for a positive `by`; past the last it comes back
// to the first, and the other way round.
void Tabs::step(int by) {
    const int much = static_cast<int>(children().many());

    if (much == 0) return;

    which(static_cast<std::size_t>(((static_cast<int>(which_) + by) % much + much) % much));
}

// Any number of tabs.
int Tabs::capacity() const { return plenty; }

// Only tabs.
bool Tabs::accepts(const Node& one) const { return dynamic_cast<const Tab*>(&one) != nullptr; }

// The strip and the largest page, as wide as the wider of the two.
Size Tabs::wants(Size room) const {
    const std::vector<Tab*> all = listed(children());

    if (all.empty()) return {0, 0};

    const Size under{room.wide, std::max(0, room.high - strip_high)};

    Size page{0, 0};

    for (const Tab* one : all) {
        const Size asked = one->wants(under);

        page = {std::max(page.wide, asked.wide), std::max(page.high, asked.high)};
    }

    return {std::min(std::max(labels_wide(all, spacing), page.wide), room.wide), std::min(strip_high + page.high, room.high)};
}

// The page shown under the strip; the hidden tabs get no room. Before the first `which` the first
// tab is shown, and the pages of the others go aside here. The names in the strip are the tabs'
// own, and a tab that is renamed says so over its page, not over the strip: so the strip compares
// the names and their letters with the last layout's.
void Tabs::layout(Size given) {
    const std::vector<Tab*> all = listed(children());

    if (all.empty()) return;

    if (which_ >= all.size()) which_ = 0;

    for (std::size_t one = 0; one < all.size(); ++one) all[one]->shown(one == which_);

    for (std::size_t at = 0; at < all.size(); ++at)
        if (at != which_) all[at]->arrange({{0, 0}, {0, 0}});

    all[which_]->arrange({{0, strip_high}, {given.wide, std::max(0, given.high - strip_high)}});

    std::vector<std::pair<std::string, int>> labels;

    for (const Tab* one : all) labels.emplace_back(one->text(), shortcut_column(one->keys(), one->text()));

    if (labels == labels_) return;

    labels_ = std::move(labels);

    changed({{0, 0}, {given.wide, strip_high}});
}

// The strip with the names, then the page shown under it.
void Tabs::paint(Canvas& onto) const {
    const std::vector<Tab*> all = listed(children());

    if (which_ >= all.size()) return;

    const Size given = onto.size();

    const Skin& how = skin();
    const Style other = how.style(role());
    const Style current = how.style(role() + ".current");
    const Style key = how.style(role() + ".key");
    const Style frame = how.style("talcite.frame");
    const char32_t thin = how.lines(Border::thin).across;
    const char32_t thick = how.lines(Border::thick).across;

    // The line under the names, with the stroke from `from` up to `upto`.
    const auto stroked = [&](int from, int upto) {
        if (given.high < strip_high) return;

        for (int x = 0; x < given.wide; ++x) {
            const bool under = x >= from && x < upto;

            onto.put({x, 1}, under ? thick : thin, under ? current : frame);
        }
    };

    // A name with its cells at `from`, and its letter.
    const auto named = [&](const Tab& one, int from, Style look) {
        onto.put({from, 0}, " " + one.text() + " ", look);
        marking(onto, {from + 1, 0}, one.text(), shortcut_column(one.keys(), one.text()), look, key);
    };

    if (fitting(all, given.wide)) {
        const int gap = gapped(all, given.wide);

        int from = std::max(0, (given.wide - labels_wide(all, gap)) / 2);

        for (std::size_t at = 0; at < all.size(); ++at) {
            const int label = named_wide(*all[at]);

            if (at == which_) stroked(from, from + label);

            named(*all[at], from, at == which_ ? current : other);

            from += label + gap;
        }
    } else {
        const auto [from, upto] = lone(*all[which_], given.wide);

        onto.put({from - 2, 0}, how.glyph(role() + ".left"), other);
        named(*all[which_], from, current);
        onto.put({upto + 1, 0}, how.glyph(role() + ".right"), other);

        stroked(from, upto);
    }

    all[which_]->render(onto);
}

// Ctrl+PgUp shows the page before, Ctrl+PgDn the page after, while the focus is on a page.
bool Tabs::handle(KeyEvent& what) {
    const Key one = what.key();
    const Mods mods = one.mods();

    if (!mods.ctrl || mods.alt || mods.shift) return false;

    if (one.code() == Code::pageup) {
        step(-1);

        return true;
    }

    if (one.code() == Code::pagedown) {
        step(1);

        return true;
    }

    return false;
}

// The same keys when a dialog passes them round (the focus outside the pages, on a button under
// them).
bool Tabs::handle(ShortcutEvent& what) {
    KeyEvent same{what.key()};

    return handle(same);
}

// A click on a name, or under it, shows its page; in a narrow strip a click left of the shown name
// turns left, right of it turns right. A click below the strip is the page's, not ours.
bool Tabs::handle(ClickEvent& what) {
    const Spot at = local(what.where());

    if (at.y >= strip_high) return false;

    const std::vector<Tab*> all = listed(children());

    if (all.empty()) return false;

    const int wide = area().size.wide;

    if (!fitting(all, wide)) {
        const auto [from, upto] = lone(*all[std::min(which_, all.size() - 1)], wide);

        if (at.x < from) step(-1);
        if (at.x >= upto) step(1);

        return true;
    }

    const int gap = gapped(all, wide);

    int from = std::max(0, (wide - labels_wide(all, gap)) / 2);

    for (std::size_t one = 0; one < all.size(); ++one) {
        const int label = named_wide(*all[one]);

        if (at.x >= from && at.x < from + label) {
            which(one);

            return true;
        }

        from += label + gap;
    }

    return true;
}

}
