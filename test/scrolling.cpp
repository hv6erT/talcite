#include "proving.hpp"

#include <memory>
#include <vector>
#include <string>

#include "talcite/box.hpp"
#include "talcite/canvas.hpp"
#include "talcite/column.hpp"
#include "talcite/encoding.hpp"
#include "talcite/event.hpp"
#include "talcite/field.hpp"
#include "talcite/scroll.hpp"
#include "talcite/root.hpp"
#include "talcite/row.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"

#include "scrollbar.hpp"
#include "sheet.hpp"

using namespace talcite;

namespace {

// A node that fills itself with `ground` and paints its only child over it, as a window or a hint
// line lies under what stands on it.
class Grounded : public ui::Node {
public:
    explicit Grounded(Style ground) : ground_(ground) {}

protected:
    // The ground first, then the child.
    void paint(Canvas& onto) const override {
        onto.fill({{0, 0}, onto.size()}, ground_);

        Node::paint(onto);
    }

private:
    Style ground_;
};

// How many times and with what visibility a node was drawn, kept apart from the node, since drawing
// does not change the node.
struct Tally {
    int drawn = 0;
    ui::Area last;
};

// A node that records how many times and with what visibility it was drawn. It shows whether the
// saving really works: an invisible child should not get paint at all.
class Sheet : public ui::Node {
public:
    explicit Sheet(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    const std::shared_ptr<Tally> tally = std::make_shared<Tally>();

    // The size given in the constructor, whatever room is offered.
    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

protected:
    // Counts the call, remembers the visible part, and fills the size with letters that change by
    // row, starting from the letter given in the constructor.
    void paint(Canvas& onto) const override {
        const ui::Size given = onto.size();
        const ui::Area seen = onto.seen();

        ++tally->drawn;
        tally->last = seen;

        onto.fill({{0, 0}, given}, Style{});

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                onto.put({x, y},
                            static_cast<char32_t>(what_ + static_cast<char32_t>(y)), Style{});
    }

private:
    ui::Size wants_;
    char32_t what_ = U'a';
};

// The text of row `y` of the view, with trailing spaces removed.
std::string read(const Snapshot& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter != 0) made += talcite::spelled(letter);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

// A column of `much` spans named "wiersz 0", "wiersz 1"….
std::shared_ptr<ui::Column> rows(int much) {
    auto made = std::make_shared<ui::Column>();

    for (int which = 0; which < much; ++which)
        made->children().add(std::make_shared<ui::Span>("wiersz " + std::to_string(which)));

    return made;
}

}

TRIAL("Visibility: the whole rectangle when nobody says otherwise") {
    const auto one = std::make_shared<Sheet>(ui::Size{4, 4}, U'a');

    (void)one->render({{0, 0}, {4, 4}});

    same(one->tally->last.at.x, 0);
    same(one->tally->last.at.y, 0);
    same(one->tally->last.size.wide, 4);
    same(one->tally->last.size.high, 4);
}

TRIAL("Visibility: what cannot be seen is not drawn") {
    ui::Column top;

    const auto first = std::make_shared<Sheet>(ui::Size{4, 2}, U'a');
    const auto second = std::make_shared<Sheet>(ui::Size{4, 2}, U'a');

    top.children().add(first);
    top.children().add(second);

    (void)top.render({{0, 0}, {4, 2}});

    same(first->tally->drawn, 1);
    same(second->tally->drawn, 0);
}

TRIAL("Visibility: the intersection is computed in both directions") {
    const ui::Area one = ui::Area::crossed(ui::Area{{0, 0}, {10, 10}}, ui::Area{{5, 5}, {10, 10}});

    same(one.at.x, 5);
    same(one.at.y, 5);
    same(one.size.wide, 5);
    same(one.size.high, 5);

    const ui::Area apart = ui::Area::crossed(ui::Area{{0, 0}, {4, 4}}, ui::Area{{9, 9}, {2, 2}});

    same(apart.size.wide, 0);
    same(apart.size.high, 0);

    const ui::Area over = ui::Area::crossed(ui::Area{{0, 0}, {4, 4}}, ui::Area{{-2, -2}, {3, 3}});

    same(over.at.x, 0);
    same(over.at.y, 0);
    same(over.size.wide, 1);
    same(over.size.high, 1);
}

TRIAL("Vertical scroll: without scrolling the start of the content is visible") {
    ui::Scroll one{ui::Axis::vertical};

    one.children().add(rows(10));

    const Snapshot made = one.render({{0, 0}, {10, 3}});

    same(read(made, 0), std::string{"wiersz 0"});
    same(read(made, 2), std::string{"wiersz 2"});
    same(one.shift().y, 0);
}

TRIAL("Vertical scroll: scrolling moves the content up") {
    ui::Scroll one{ui::Axis::vertical};

    one.children().add(rows(10));

    one.shift({0, 4});

    const Snapshot made = one.render({{0, 0}, {10, 3}});

    same(read(made, 0), std::string{"wiersz 4"});
    same(read(made, 1), std::string{"wiersz 5"});
}

TRIAL("Vertical scroll: cannot scroll past the end of the content") {
    ui::Scroll one{ui::Axis::vertical};

    one.children().add(rows(5));

    one.shift({0, 100});

    const Snapshot made = one.render({{0, 0}, {10, 3}});

    same(one.shift().y, 2);
    same(read(made, 0), std::string{"wiersz 2"});
}

TRIAL("Vertical scroll: content shorter than the window does not scroll at all") {
    ui::Scroll one{ui::Axis::vertical};

    one.children().add(rows(2));

    one.shift({0, 5});

    (void)one.render({{0, 0}, {10, 6}});

    same(one.shift().y, 0);
}

TRIAL("Vertical scroll: a negative scroll goes down to zero") {
    ui::Scroll one{ui::Axis::vertical};

    one.shift({0, -5});

    same(one.shift().y, 0);
}

TRIAL("Vertical scroll: draws only the rows that are visible") {
    ui::Scroll one{ui::Axis::vertical};

    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Sheet>(ui::Size{6, 2}, U'a');
    const auto second = std::make_shared<Sheet>(ui::Size{6, 2}, U'b');
    const auto third = std::make_shared<Sheet>(ui::Size{6, 2}, U'c');

    column->children().add(first);
    column->children().add(second);
    column->children().add(third);

    one.children().add(column);

    one.shift({0, 2});

    (void)one.render({{0, 0}, {6, 2}});

    same(first->tally->drawn, 0);
    same(second->tally->drawn, 1);
    same(third->tally->drawn, 0);
}

TRIAL("Vertical scroll: the child gets its visibility in its own coordinates") {
    ui::Scroll one{ui::Axis::vertical};

    const auto inner = std::make_shared<Sheet>(ui::Size{6, 10}, U'a');

    one.children().add(inner);

    one.shift({0, 4});

    (void)one.render({{0, 0}, {6, 3}});

    same(inner->tally->last.at.y, 4);
    same(inner->tally->last.size.high, 3);
}

TRIAL("Vertical scroll: the wheel scrolls, and at the end passes the event up") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::vertical);

    scroll->children().add(rows(10));
    top.children().add(scroll);

    (void)top.render({{0, 0}, {10, 3}});

    ui::WheelEvent down{{1, 1}, -1};

    ought(top.dispatch(down));
    same(scroll->shift().y, 3);

    scroll->shift({0, 7});

    (void)top.render({{0, 0}, {10, 3}});

    ui::WheelEvent more{{1, 1}, -1};

    ought(!top.dispatch(more));
    same(scroll->shift().y, 7);
}

TRIAL("Vertical scroll: a click hits what is visible after scrolling") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::vertical);
    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Sheet>(ui::Size{6, 2}, U'a');
    const auto second = std::make_shared<Sheet>(ui::Size{6, 2}, U'b');

    column->children().add(first);
    column->children().add(second);
    scroll->children().add(column);
    top.children().add(scroll);

    scroll->shift({0, 2});

    (void)top.render({{0, 0}, {6, 2}});

    same(first->area().at.y, 0);
    same(second->area().at.y, 2);
    same(scroll->children().front()->area().at.y, -2);
}

TRIAL("Horizontal scroll: scrolling moves the content left") {
    ui::Scroll one{ui::Axis::horizontal};

    one.children().add(std::make_shared<ui::Span>("abcdefghij"));

    one.shift({3, 0});

    const Snapshot made = one.render({{0, 0}, {4, 1}});

    same(read(made, 0), std::string{"defg"});
}

TRIAL("Horizontal scroll: cannot scroll past the end of the content") {
    ui::Scroll one{ui::Axis::horizontal};

    one.children().add(std::make_shared<ui::Span>("abcdefghij"));

    one.shift({100, 0});

    (void)one.render({{0, 0}, {4, 1}});

    same(one.shift().x, 6);
}

TRIAL("Horizontal scroll: wants at most as much as it got") {
    ui::Scroll one{ui::Axis::horizontal};

    one.children().add(std::make_shared<ui::Span>("abcdefghij"));

    same(one.wants({4, 1}).wide, 4);
    same(one.wants({40, 1}).wide, 10);
}

TRIAL("Scroll: scrolls in both directions at once") {
    ui::Scroll one{ui::Axis::both};

    const auto inner = std::make_shared<Sheet>(ui::Size{20, 20}, U'a');

    one.children().add(inner);

    one.shift({3, 4});

    (void)one.render({{0, 0}, {5, 5}});

    same(one.shift().x, 3);
    same(one.shift().y, 4);
    same(inner->tally->last.at.x, 3);
    same(inner->tally->last.at.y, 4);
    same(inner->tally->last.size.wide, 5);
    same(inner->tally->last.size.high, 5);
}

TRIAL("Scroll: the wheel scrolls vertically, and with Shift horizontally") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::both);

    scroll->children().add(std::make_shared<Sheet>(ui::Size{20, 20}, U'a'));
    top.children().add(scroll);

    (void)top.render({{0, 0}, {5, 5}});

    ui::WheelEvent down{{1, 1}, -1};

    ought(top.dispatch(down));

    same(scroll->shift().y, 3);
    same(scroll->shift().x, 0);

    ui::Mods with;
    with.shift = true;

    ui::WheelEvent aside{{1, 1}, -1, with};

    ought(top.dispatch(aside));

    same(scroll->shift().x, 3);
    same(scroll->shift().y, 3);
}

// The wheel moves the scroller by hand, a call from the program does not.
TRIAL("Scroll: a move says whether it came by hand") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::both);

    scroll->children().add(std::make_shared<Sheet>(ui::Size{20, 20}, U'a'));
    top.children().add(scroll);

    (void)top.render({{0, 0}, {5, 5}});

    std::vector<bool> heard;

    const ui::Hook heard_scroll = scroll->on<ui::ScrollEvent>([&heard](ui::ScrollEvent& what) {
        heard.push_back(what.by_hand());

        return false;
    });

    ui::WheelEvent down{{1, 1}, -1};

    ought(top.dispatch(down));

    scroll->shift({1, 1});

    same(heard, (std::vector<bool>{true, false}));
}

TRIAL("Scroll: scrolling does not go beyond the content") {
    ui::Scroll one{ui::Axis::both};

    one.children().add(std::make_shared<Sheet>(ui::Size{8, 8}, U'a'));

    one.shift({100, 100});

    (void)one.render({{0, 0}, {5, 5}});

    same(one.shift().x, 3);
    same(one.shift().y, 3);
}

TRIAL("Scroll: a span in a scroller does not wrap at all") {
    ui::Scroll one{ui::Axis::both};

    one.children().add(std::make_shared<ui::Span>("ala ma kota", ui::SpanShape{true}));

    same(one.wants({4, 10}).high, 1);
}

TRIAL("Scrollbar: without turning it on there is none") {
    ui::Scroll one{ui::Axis::vertical};

    one.children().add(rows(10));

    const Snapshot made = one.render({{0, 0}, {10, 3}});

    ought(!one.scrollbar());
    same(read(made, 0), std::string{"wiersz 0"});
}

// The bar is a thin line with a solid slider on it, from edge to edge, without arrows.
TRIAL("Scrollbar: a line from edge to edge, the slider on it") {
    ui::Scroll one{ui::Axis::vertical, ui::ScrollShape{true}};

    one.children().add(rows(20));

    const Snapshot made = one.render({{0, 0}, {10, 5}});

    ought(one.scrollbar());
    same(made.at({9, 0}).letter, U'█');
    same(made.at({9, 1}).letter, U'│');
    same(made.at({9, 4}).letter, U'│');
}

TRIAL("Scrollbar: the slider moves down together with scrolling") {
    ui::Scroll one{ui::Axis::vertical, ui::ScrollShape{true}};

    one.children().add(rows(20));

    one.shift({0, 100});

    const Snapshot made = one.render({{0, 0}, {10, 5}});

    same(made.at({9, 4}).letter, U'█');
    same(made.at({9, 0}).letter, U'│');
}

// The pager computes the bar in bytes, so a file larger than `int` is an ordinary case, not a
// curiosity. Three billion bytes, scrolled to the middle: the slider must stand in the middle of
// the track; twelve cells of track, one for the slider, eleven to move it in, so the seventh from
// the top.
TRIAL("Scrollbar: three billion at the middle give the slider in the middle") {
    const ui::Skin how;
    const std::string role = "talcite.scroll";

    detail::Sheet made{{1, 12}};

    Canvas onto{made};

    ui::Scrollbar{.track = ui::Track{.at = {0, 0}, .much = 12, .shift = 1'500'000'000, .whole = 3'000'000'000, .window = 1000},
                  .skin = how,
                  .role = role}
        .draw(onto);

    same(made.at({0, 0}).letter, U'│');
    same(made.at({0, 11}).letter, U'│');
    same(made.at({0, 5}).letter, U'│');
    same(made.at({0, 6}).letter, U'█');
    same(made.at({0, 7}).letter, U'│');
}

// The other half of the same trap: the slider's length is the track times the window over the
// whole, and with a window of the order of a billion the product alone does not fit in `int`. A
// window of half the whole gives a slider of half the track.
TRIAL("Scrollbar: a window of half of three billion gives a slider of half the track") {
    const ui::Skin how;
    const std::string role = "talcite.scroll";

    detail::Sheet made{{1, 12}};

    Canvas onto{made};

    ui::Scrollbar{.track = ui::Track{.at = {0, 0}, .much = 12, .shift = 0, .whole = 3'000'000'000, .window = 1'500'000'000},
                  .skin = how,
                  .role = role}
        .draw(onto);

    for (int y = 0; y <= 5; ++y) same(made.at({0, y}).letter, U'█');
    for (int y = 6; y <= 11; ++y) same(made.at({0, y}).letter, U'│');
}

TRIAL("Scrollbar: content that fits in the window gives a slider over the whole track") {
    ui::Scroll one{ui::Axis::vertical, ui::ScrollShape{true}};

    one.children().add(rows(2));

    const Snapshot made = one.render({{0, 0}, {10, 5}});

    for (int y = 0; y < 5; ++y) same(made.at({9, y}).letter, U'█');
}

// The scrollbar "when needed" (`needed`): content that fits in the window gets the full width and
// no bar; longer content has one as always. The scroller then wants as much as the content,
// without a column for the bar.
TRIAL("Scrollbar when needed: content that fits has no bar, longer content has one") {
    ui::Scroll one{ui::Axis::vertical, ui::ScrollShape{true, true}};

    const auto inner = std::make_shared<Sheet>(ui::Size{10, 4}, U'a');

    one.children().add(inner);

    const Snapshot fits = one.render({{0, 0}, {10, 5}});

    same(inner->area().size.wide, 10);
    same(fits.at({9, 0}).letter, U'a');
    same(one.wants({80, 5}).wide, 10);

    const Snapshot tight = one.render({{0, 0}, {10, 3}});

    same(inner->area().size.wide, 9);
    same(tight.at({9, 0}).letter, U'█');
    same(tight.at({9, 1}).letter, U'█');
    same(tight.at({9, 2}).letter, U'│');
    same(one.wants({80, 3}).wide, 11);

    // Content equal to the window fits too; the last column belongs to the content (row 3 is "d").
    const Snapshot exact = one.render({{0, 0}, {10, 4}});

    same(exact.at({9, 3}).letter, U'd');
}

// Both bars "when needed": the vertical one appears because the content is too tall, and takes a
// column, so content exactly as wide as the window stops fitting horizontally. The second pass
// sees it.
TRIAL("Scrollbar when needed: the vertical one brings the horizontal one when it took a column") {
    ui::Scroll one{ui::Axis::both, ui::ScrollShape{true, true}};

    one.children().add(std::make_shared<Sheet>(ui::Size{6, 20}, U'a'));

    const Snapshot made = one.render({{0, 0}, {6, 6}});

    same(made.at({5, 0}).letter, U'█');
    same(made.at({5, 4}).letter, U'│');
    same(made.at({0, 5}).letter, U'█');
    same(made.at({4, 5}).letter, U'─');

    ui::Scroll wide{ui::Axis::both, ui::ScrollShape{true, true}};

    wide.children().add(std::make_shared<Sheet>(ui::Size{5, 5}, U'a'));

    const Snapshot none = wide.render({{0, 0}, {6, 6}});

    same(none.at({5, 0}).letter, U' ');
    same(none.at({0, 5}).letter, U' ');

    // Content exactly as wide as the window fits horizontally.
    ui::Scroll exact{ui::Axis::both, ui::ScrollShape{true, true}};

    exact.children().add(std::make_shared<Sheet>(ui::Size{6, 5}, U'a'));

    same(exact.render({{0, 0}, {6, 6}}).at({0, 5}).letter, U' ');

    // A horizontal-only scroller has no vertical bar, even if the content is taller than the
    // window.
    ui::Scroll across{ui::Axis::horizontal, ui::ScrollShape{true, true}};

    across.children().add(std::make_shared<Sheet>(ui::Size{20, 5}, U'a'));

    const Snapshot flat = across.render({{0, 0}, {6, 3}});

    same(flat.at({5, 0}).letter, U'a');
    same(flat.at({0, 2}).letter, U'█');
    same(flat.at({5, 2}).letter, U'─');
}

TRIAL("Scrollbar: takes one column from the content") {
    ui::Scroll one{ui::Axis::vertical};

    const auto inner = std::make_shared<Sheet>(ui::Size{20, 4}, U'a');

    one.children().add(inner);

    (void)one.render({{0, 0}, {10, 4}});

    same(inner->area().size.wide, 10);

    one.scrollbar(true);

    (void)one.render({{0, 0}, {10, 4}});

    same(inner->area().size.wide, 9);
}

TRIAL("Scrollbar: also counts in what the scroller wants") {
    ui::Scroll one{ui::Axis::vertical, ui::ScrollShape{true}};

    one.children().add(std::make_shared<ui::Span>("kot"));

    same(one.wants({80, 5}).wide, 4);
}

TRIAL("Horizontal scrollbar: a line in the bottom row, the slider on it") {
    ui::Scroll one{ui::Axis::horizontal, ui::ScrollShape{true}};

    one.children().add(std::make_shared<ui::Span>("abcdefghijklmnoprst"));

    const Snapshot made = one.render({{0, 0}, {6, 2}});

    same(made.at({0, 1}).letter, U'█');
    same(made.at({1, 1}).letter, U'─');
    same(made.at({5, 1}).letter, U'─');
    same(read(made, 0), std::string{"abcdef"});
}

TRIAL("Scroll: two bars and an empty corner") {
    ui::Scroll one{ui::Axis::both, ui::ScrollShape{true}};

    one.children().add(std::make_shared<Sheet>(ui::Size{20, 20}, U'a'));

    const Snapshot made = one.render({{0, 0}, {6, 6}});

    same(made.at({5, 0}).letter, U'█');
    same(made.at({5, 4}).letter, U'│');
    same(made.at({0, 5}).letter, U'█');
    same(made.at({4, 5}).letter, U'─');
    same(made.at({5, 5}).letter, U' ');
}

TRIAL("Scroll: the bars take a column and a row from the content") {
    ui::Scroll one{ui::Axis::both, ui::ScrollShape{true}};

    const auto inner = std::make_shared<Sheet>(ui::Size{20, 20}, U'a');

    one.children().add(inner);

    (void)one.render({{0, 0}, {6, 6}});

    same(inner->tally->last.size.wide, 5);
    same(inner->tally->last.size.high, 5);
}

TRIAL("Scrollers take one child") {
    ui::Scroll first{ui::Axis::vertical};
    ui::Scroll second{ui::Axis::horizontal};
    ui::Scroll third{ui::Axis::both};

    same(first.capacity(), 1);
    same(second.capacity(), 1);
    same(third.capacity(), 1);
}

// The slider is drawn by free functions that have no node through which to reach a role, so they
// get the stem as an argument. Thanks to that the scroller, the list, the table and the pager can
// have sliders dressed separately, instead of one for the whole library.
TRIAL("Vertical scroll: the stem of the slider's role can be replaced") {
    ui::Root top;

    const auto one = std::make_shared<ui::Scroll>(ui::Axis::vertical, ui::ScrollShape{true});
    const auto body = std::make_shared<Sheet>(ui::Size{4, 20}, U'x');

    same(one->scrollbar_role(), std::string{"talcite.scroll"});

    one->children().add(body);
    top.children().add(one);

    ui::Styles these;

    Style look{};

    look.foreground = Color{false, 6, 6, 6};

    these["moj.suwak"] = look;

    top.skin(ui::Skin{these});
    one->scrollbar_role("moj.suwak");

    const Snapshot made = top.render({{0, 0}, {6, 5}});

    same(made.at({5, 4}).style.foreground, (Color{false, 6, 6, 6}));
}

// The bar is drawn in the foreground only: a style that gives no background leaves the ground of
// what lies below, as a window or a panel colors it, and a style that gives one keeps it.
TRIAL("Scrollbar: a style without a background takes the ground below") {
    ui::Root top;

    const auto one = std::make_shared<ui::Scroll>(ui::Axis::vertical, ui::ScrollShape{true});

    one->children().add(rows(20));

    Style ground{};

    ground.background = Color{false, 0, 0, 9};

    const auto under = std::make_shared<Grounded>(ground);

    under->children().add(one);
    top.children().add(under);

    Style lit{};

    lit.foreground = Color{false, 7, 7, 7};

    top.skin(ui::Skin{ui::Styles{{"talcite.normal", ground}, {"talcite.scroll", lit}, {"talcite.scroll.thumb", lit}}});

    const Snapshot made = top.render({{0, 0}, {10, 5}});

    same(made.at({9, 0}).letter, U'█');
    same(made.at({9, 0}).style.background, (Color{false, 0, 0, 9}));
    same(made.at({9, 3}).style.background, (Color{false, 0, 0, 9}));
    same(made.at({9, 3}).style.foreground, (Color{false, 7, 7, 7}));

    Style own = lit;

    own.background = Color{false, 3, 3, 3};

    top.skin(ui::Skin{ui::Styles{{"talcite.scroll", own}, {"talcite.scroll.thumb", own}}});

    same(top.render({{0, 0}, {10, 5}}).at({9, 3}).style.background, (Color{false, 3, 3, 3}));
}

// The bar of a widget the keys move takes the .focus styles: a scroller's bar while the focus is
// on a field inside it, the track from `role`.focus and the slider from `role`.thumb.focus; with
// the focus elsewhere, the plain ones.
TRIAL("Scrollbar: with the focus inside, the bar takes the .focus styles") {
    ui::Root top;

    const auto one = std::make_shared<ui::Scroll>(ui::Axis::vertical, ui::ScrollShape{true});
    const auto inner = std::make_shared<ui::Column>();

    for (int which = 0; which < 20; ++which) inner->children().add(std::make_shared<ui::Field>(6, std::to_string(which)));

    const auto outside = std::make_shared<ui::Field>(6, "poza");
    const auto column = std::make_shared<ui::Column>();

    one->children().add(inner);
    column->children().add(one);
    column->children().add(outside);
    top.children().add(column);

    const auto colored = [](std::uint8_t level) {
        Style made{};

        made.foreground = Color{false, level, level, level};

        return made;
    };

    top.skin(ui::Skin{ui::Styles{{"talcite.scroll", colored(1)},
                                 {"talcite.scroll.thumb", colored(2)},
                                 {"talcite.scroll.focus", colored(3)},
                                 {"talcite.scroll.thumb.focus", colored(4)}}});

    outside->focus();

    Snapshot made = top.render({{0, 0}, {8, 6}});

    same(made.at({7, 0}).style.foreground, (Color{false, 2, 2, 2}));
    same(made.at({7, 4}).style.foreground, (Color{false, 1, 1, 1}));

    inner->children()[0]->focus();

    made = top.render({{0, 0}, {8, 6}});

    same(made.at({7, 0}).style.foreground, (Color{false, 4, 4, 4}));
    same(made.at({7, 4}).style.foreground, (Color{false, 3, 3, 3}));
}

// --- Following the focus --------------------------------------------------------------------

namespace {

// A column of `much` fields of width `wide` holding their numbers.
std::shared_ptr<ui::Column> fields(int much, int wide = 6) {
    auto made = std::make_shared<ui::Column>();

    for (int which = 0; which < much; ++which)
        made->children().add(std::make_shared<ui::Field>(wide, std::to_string(which)));

    return made;
}

}

// Focus in a field outside the window adjusts the scrolling in the next frame, in `paint`, as in
// the list, because only after layout is the field's rectangle known. Down when the field is
// lower; up when it is higher; a field inside the window moves nothing.
TRIAL("Vertical scroll: focus outside the window adjusts the scrolling, inside it moves nothing") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::vertical);
    const auto column = fields(30);

    scroll->children().add(column);
    top.children().add(scroll);

    (void)top.render({{0, 0}, {10, 5}});

    same(scroll->shift().y, 0);

    column->children()[19]->focus();

    Snapshot shown = top.render({{0, 0}, {10, 5}});

    same(scroll->shift().y, 15);
    same(read(shown, 4), std::string{"19"});

    column->children()[17]->focus();

    (void)top.render({{0, 0}, {10, 5}});

    same(scroll->shift().y, 15);

    column->children()[3]->focus();

    shown = top.render({{0, 0}, {10, 5}});

    same(scroll->shift().y, 3);
    same(read(shown, 0), std::string{"3"});

    column->children()[0]->focus();

    (void)top.render({{0, 0}, {10, 5}});

    same(scroll->shift().y, 0);

    column->children()[29]->focus();

    (void)top.render({{0, 0}, {10, 5}});

    same(scroll->shift().y, 25);
}

// The wheel moves the view and the focus stays, and the next frame does not undo the wheel: the
// focus is followed only in the frame after it changes. Focus outside the scroller moves nothing.
TRIAL("Vertical scroll: the wheel does not fight the focus, and focus beside the scroller moves nothing") {
    ui::Root top;

    const auto whole = std::make_shared<ui::Column>();
    const auto box = std::make_shared<ui::Box>(ui::Size{10, 5});
    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::vertical);
    const auto column = fields(30);
    const auto outside = std::make_shared<ui::Field>(6, "obok");

    scroll->children().add(column);
    box->children().add(scroll);
    whole->children().add(box);
    whole->children().add(outside);
    top.children().add(whole);

    (void)top.render({{0, 0}, {10, 6}});

    column->children()[10]->focus();

    (void)top.render({{0, 0}, {10, 6}});

    same(scroll->shift().y, 6);

    ui::WheelEvent up{{1, 1}, 1};

    ought(top.dispatch(up));
    same(scroll->shift().y, 3);

    (void)top.render({{0, 0}, {10, 6}});

    same(scroll->shift().y, 3);

    outside->focus();

    (void)top.render({{0, 0}, {10, 6}});

    same(scroll->shift().y, 3);
}

// A node taller than the window is brought in by its top; the start matters more than the end.
TRIAL("Vertical scroll: a node taller than the window is brought in by its top") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::vertical);
    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<Sheet>(ui::Size{6, 4}, U'a');
    const auto tall = std::make_shared<ui::Field>(6, "x");
    const auto box = std::make_shared<ui::Column>();

    for (int which = 0; which < 6; ++which) box->children().add(std::make_shared<ui::Span>("w"));

    box->children().add(tall);

    column->children().add(first);
    column->children().add(box);
    scroll->children().add(column);
    top.children().add(scroll);

    (void)top.render({{0, 0}, {6, 3}});

    tall->focus();

    (void)top.render({{0, 0}, {6, 3}});

    same(scroll->shift().y, 8);
}

TRIAL("Horizontal scroll: focus outside the window adjusts the horizontal scrolling") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::horizontal);
    const auto row = std::make_shared<ui::Row>();

    for (int which = 0; which < 10; ++which)
        row->children().add(std::make_shared<ui::Field>(4, std::to_string(which)));

    scroll->children().add(row);
    top.children().add(scroll);

    (void)top.render({{0, 0}, {10, 1}});

    row->children()[5]->focus();

    (void)top.render({{0, 0}, {10, 1}});

    same(scroll->shift().x, 14);

    row->children()[0]->focus();

    (void)top.render({{0, 0}, {10, 1}});

    same(scroll->shift().x, 0);
}

TRIAL("Scroll: the focus adjusts the scrolling on both axes at once") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::both);
    const auto column = std::make_shared<ui::Column>();

    for (int y = 0; y < 8; ++y) {
        const auto row = std::make_shared<ui::Row>();

        for (int x = 0; x < 8; ++x)
            row->children().add(std::make_shared<ui::Field>(4, std::to_string(y) + std::to_string(x)));

        column->children().add(row);
    }

    scroll->children().add(column);
    top.children().add(scroll);

    (void)top.render({{0, 0}, {10, 3}});

    const auto& row = column->children()[6];

    row->children()[5]->focus();

    (void)top.render({{0, 0}, {10, 3}});

    same(scroll->shift().x, 14);
    same(scroll->shift().y, 4);

    column->children()[0]->children()[0]->focus();

    (void)top.render({{0, 0}, {10, 3}});

    same(scroll->shift().x, 0);
    same(scroll->shift().y, 0);
}

// --- Scrolling axis ------------------------------------------------------------------------------

namespace {

// A node that remembers what room it was asked about.
class Asked : public ui::Node {
public:
    mutable ui::Size room;

    // Remembers the room and asks for four by four.
    [[nodiscard]] ui::Size wants(ui::Size given) const override {
        room = given;

        return {4, 4};
    }
};

// The room that a scroller with a bar on the given axis offers its child when asked for 20 by 10.
ui::Size asked(ui::Axis axis) {
    ui::Scroll one{axis, ui::ScrollShape{true}};

    const auto probe = std::make_shared<Asked>();

    one.children().add(probe);

    (void)one.wants({20, 10});

    return probe->room;
}

// The visible size that a scroller with a bar on the given axis gives a large child in a window of
// 10 by 5.
ui::Size window(ui::Axis axis) {
    ui::Scroll one{axis, ui::ScrollShape{true}};

    const auto sheet = std::make_shared<Sheet>(ui::Size{30, 30}, U'a');

    one.children().add(sheet);

    (void)one.render({{0, 0}, {10, 5}});

    return sheet->tally->last.size;
}

}

// An open axis asks for `plenty` (without the bar), a closed one for the room without the bar; that
// is why a span in a vertical scroller wraps to the window's width, and in a horizontal one stays
// one long line.
TRIAL("Scroll: an open axis asks the child for plenty, a closed one for the room") {
    const ui::Size vertical = asked(ui::Axis::vertical);
    same(vertical.wide, 19);
    same(vertical.high, ui::plenty);

    const ui::Size horizontal = asked(ui::Axis::horizontal);
    same(horizontal.wide, ui::plenty);
    same(horizontal.high, 9);

    const ui::Size both = asked(ui::Axis::both);
    same(both.wide, ui::plenty - 1);
    same(both.high, ui::plenty - 1);
}

TRIAL("Scroll: a bar stands only along an open axis") {
    same(window(ui::Axis::vertical).wide, 9);
    same(window(ui::Axis::vertical).high, 5);

    same(window(ui::Axis::horizontal).wide, 10);
    same(window(ui::Axis::horizontal).high, 4);

    same(window(ui::Axis::both).wide, 9);
    same(window(ui::Axis::both).high, 4);
}

TRIAL("Scroll: the component of a closed axis is always zero") {
    ui::Scroll vertical{ui::Axis::vertical};
    vertical.shift({5, 2});
    same(vertical.shift().x, 0);
    same(vertical.shift().y, 2);

    ui::Scroll horizontal{ui::Axis::horizontal};
    horizontal.shift({5, 2});
    same(horizontal.shift().x, 5);
    same(horizontal.shift().y, 0);

    ui::Scroll both{ui::Axis::both};
    both.shift({5, 2});
    same(both.shift().x, 5);
    same(both.shift().y, 2);

    same(both.axis(), ui::Axis::both);
}

TRIAL("Horizontal scroll: the wheel scrolls horizontally, without Shift") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::horizontal);

    scroll->children().add(std::make_shared<Sheet>(ui::Size{30, 2}, U'a'));
    top.children().add(scroll);

    (void)top.render({{0, 0}, {10, 2}});

    ui::WheelEvent down{{1, 1}, -1};

    ought(top.dispatch(down));
    same(scroll->shift().x, 3);
    same(scroll->shift().y, 0);
}

TRIAL("Vertical scroll: the wheel with Shift does not scroll horizontally") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::Axis::vertical);

    scroll->children().add(rows(10));
    top.children().add(scroll);

    (void)top.render({{0, 0}, {10, 3}});

    ui::WheelEvent down{{1, 1}, -1, ui::Mods{.shift = true}};

    ought(top.dispatch(down));
    same(scroll->shift().y, 3);
    same(scroll->shift().x, 0);
}

// The tilted wheel (a touchpad's sideways swipe) scrolls sideways wherever there is a horizontal
// axis, and a vertical-only scroller passes it on instead of scrolling down.
TRIAL("Scroll: the tilted wheel scrolls sideways, and a vertical scroller passes it on") {
    ui::Root top;

    const auto both = std::make_shared<ui::Scroll>(ui::Axis::both);

    both->children().add(std::make_shared<Sheet>(ui::Size{20, 20}, U'a'));
    top.children().add(both);

    (void)top.render({{0, 0}, {5, 5}});

    ui::WheelEvent right{{1, 1}, -1, {}, ui::Axis::horizontal};

    ought(top.dispatch(right));
    same(both->shift().x, 3);
    same(both->shift().y, 0);

    ui::Root other;

    const auto upright = std::make_shared<ui::Scroll>(ui::Axis::vertical);

    upright->children().add(rows(10));
    other.children().add(upright);

    (void)other.render({{0, 0}, {10, 3}});

    ui::WheelEvent tilted{{1, 1}, -1, {}, ui::Axis::horizontal};

    ought(!other.dispatch(tilted));
    same(upright->shift().y, 0);
}
