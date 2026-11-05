#include "proving.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "talcite/encoding.hpp"
#include "talcite/column.hpp"
#include "talcite/frame.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"
#include "talcite/line.hpp"
#include "talcite/span.hpp"
#include "talcite/scroll.hpp"

using namespace talcite;

namespace {

// A node of a fixed size that paints its area with one letter.
class Box : public ui::Node {
public:
    Box(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    // The fixed size, whatever space is offered.
    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

protected:
    // Fills the whole given size with the letter, shifted to the visible part.
    void paint(Canvas& onto) const override {
        const ui::Size given = onto.size();

        onto.fill({{0, 0}, given}, Style{});

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                onto.put({x, y}, what_, Style{});
    }

private:
    ui::Size wants_;
    char32_t what_ = U'?';
};

// Reads one row of a view as text, with trailing spaces removed.
std::string read(const Snapshot& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter != 0) made += talcite::spelled(letter);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

}

TRIAL("Frame: corners, sides and interior") {
    ui::Frame one;

    one.children().add(std::make_shared<Box>(ui::Size{4, 2}, U'x'));

    const Snapshot made = one.render({{0, 0}, {6, 4}});

    same(read(made, 0), std::string{"┌────┐"});
    same(read(made, 1), std::string{"│xxxx│"});
    same(read(made, 2), std::string{"│xxxx│"});
    same(read(made, 3), std::string{"└────┘"});
}

TRIAL("Frame: double and thick lines") {
    ui::Frame one;

    one.border(ui::Border::twin);

    same(read(one.render({{0, 0}, {4, 2}}), 0), std::string{"╔══╗"});

    one.border(ui::Border::thick);

    same(read(one.render({{0, 0}, {4, 2}}), 0), std::string{"┏━━┓"});
}

TRIAL("Frame: without a line there is no frame and the child gets everything") {
    ui::Frame one{"tytuł", ui::FrameShape{ui::Border::none}};

    const auto inner = std::make_shared<Box>(ui::Size{4, 2}, U'x');

    one.children().add(inner);

    const Snapshot made = one.render({{0, 0}, {6, 4}});

    same(inner->area().size.wide, 6);
    same(inner->area().size.high, 4);
    same(read(made, 0), std::string{"xxxxxx"});
}

TRIAL("Frame: the frame takes one cell from the child on each side") {
    ui::Frame one;

    const auto inner = std::make_shared<Box>(ui::Size{10, 10}, U'x');

    one.children().add(inner);

    (void)one.render({{0, 0}, {8, 5}});

    same(inner->area().at.x, 1);
    same(inner->area().at.y, 1);
    same(inner->area().size.wide, 6);
    same(inner->area().size.high, 3);
}

TRIAL("Frame: wants as much as the child, plus two in both dimensions") {
    ui::Frame one;

    one.children().add(std::make_shared<Box>(ui::Size{4, 2}, U'x'));

    const ui::Size wants = one.wants({80, 25});

    same(wants.wide, 6);
    same(wants.high, 4);
}

TRIAL("Frame: wants no more than it got from the parent") {
    ui::Frame one;

    one.children().add(std::make_shared<Box>(ui::Size{40, 40}, U'x'));

    const ui::Size wants = one.wants({8, 5});

    same(wants.wide, 8);
    same(wants.high, 5);
}

TRIAL("Frame: the title stands in the top edge, centered") {
    ui::Frame one{"kot"};

    const Snapshot made = one.render({{0, 0}, {11, 3}});

    same(read(made, 0), std::string{"┌── kot ──┐"});
}

TRIAL("Frame: the title to the left and to the right") {
    ui::Frame one{"kot"};

    one.justify(ui::Justify::left);

    same(read(one.render({{0, 0}, {11, 3}}), 0), std::string{"┌ kot ────┐"});

    one.justify(ui::Justify::right);

    same(read(one.render({{0, 0}, {11, 3}}), 0), std::string{"┌──── kot ┐"});
}

// The title and the footer have separate alignments: in MC the path stands in the middle of the top
// edge, and the status line to the left, which one `justify` for both could not express.
TRIAL("Frame: the footer has its own alignment, independent of the title") {
    ui::Frame one{"kot"};

    one.footer("pies");

    same(one.footing(), ui::Justify::center);

    one.footing(ui::Justify::left);

    const Snapshot made = one.render({{0, 0}, {11, 3}});

    same(read(made, 0), std::string{"┌── kot ──┐"});
    same(read(made, 2), std::string{"└ pies ───┘"});
    same(one.justify(), ui::Justify::center);

    one.justify(ui::Justify::right);

    same(read(one.render({{0, 0}, {11, 3}}), 2), std::string{"└ pies ───┘"});
}

TRIAL("Frame: a title that is too long is shortened with an ellipsis") {
    ui::Frame one{"bardzo długi tytuł"};

    const Snapshot made = one.render({{0, 0}, {10, 3}});

    same(read(made, 0), std::string{"┌ bardzo…┐"});
}

TRIAL("Frame: the footer stands in the bottom edge") {
    ui::Frame one{"kot"};

    one.footer("3 pliki");

    const Snapshot made = one.render({{0, 0}, {13, 3}});

    same(read(made, 0), std::string{"┌─── kot ───┐"});
    same(read(made, 2), std::string{"└─ 3 pliki ─┘"});
}

TRIAL("Frame: the texts are exchanged through getters and setters") {
    ui::Frame one{"kot"};

    same(one.title(), std::string{"kot"});
    same(one.footer(), std::string{""});

    one.title("pies");
    one.footer("dwa");

    same(one.title(), std::string{"pies"});
    same(one.footer(), std::string{"dwa"});
    same(one.border(), ui::Border::thin);
}

// The lines and the title have separate roles, so the skin decides about each of them on its own.
TRIAL("Frame: the lines and the text take their color from separate roles") {
    ui::Root top;

    const auto one = std::make_shared<ui::Frame>("kot");

    top.children().add(one);

    Style edge{};
    Style told{};

    edge.foreground = Color{false, 10, 20, 30};
    told.foreground = Color{false, 40, 50, 60};

    // The title follows the frame's role root: `talcite.frame.title`, not a shared `talcite.title`.
    top.skin(ui::Skin{{{"talcite.frame", edge}, {"talcite.frame.title", told}}});

    const Snapshot made = top.render({{0, 0}, {11, 3}});

    same(made.at({0, 0}).style.foreground.red, static_cast<std::uint8_t>(10));
    same(made.at({4, 0}).style.foreground.red, static_cast<std::uint8_t>(40));

    // A changed root takes the title along with it: that is the whole point of deriving the title's
    // role from the root.
    Style other{};

    other.foreground = Color{false, 70, 80, 90};

    one->role("app.panel");
    top.skin(ui::Skin{{{"app.panel.title", other}}});

    same(top.render({{0, 0}, {11, 3}}).at({4, 0}).style.foreground.red, static_cast<std::uint8_t>(70));
}

// The footer is in the color of the lines, not of the title, like the status line under the file
// list in MC, which uses the panel's ordinary color although the title of the active panel is
// inverted.
TRIAL("Frame: the footer takes the color of the lines, not of the title") {
    ui::Root top;

    const auto one = std::make_shared<ui::Frame>("kot");

    one->footer("pies");

    top.children().add(one);

    Style edge{};
    Style told{};

    edge.foreground = Color{false, 10, 20, 30};
    told.foreground = Color{false, 40, 50, 60};

    top.skin(ui::Skin{{{"talcite.frame", edge}, {"talcite.frame.title", told}}});

    const Snapshot made = top.render({{0, 0}, {11, 3}});

    same(read(made, 2), std::string{"└─ pies ──┘"});
    same(made.at({4, 0}).style.foreground.red, static_cast<std::uint8_t>(40));
    same(made.at({4, 2}).style.foreground.red, static_cast<std::uint8_t>(10));
}

TRIAL("Frame: a tight area breaks nothing") {
    ui::Frame one{"kot"};

    one.children().add(std::make_shared<Box>(ui::Size{4, 4}, U'x'));

    const Snapshot first = one.render({{0, 0}, {1, 1}});

    same(first.size().wide, 1);

    const Snapshot second = one.render({{0, 0}, {2, 2}});

    same(read(second, 0), std::string{"┌┐"});
    same(read(second, 1), std::string{"└┘"});
}

TRIAL("Frame: an empty frame is drawn without a child") {
    ui::Frame one{"kot"};

    const Snapshot made = one.render({{0, 0}, {7, 3}});

    same(read(made, 1), std::string{"│     │"});

    // Empty, it still fits its title: three letters, a cell of padding on each side and one more
    // beyond, and the two edges.
    same(one.wants({80, 25}).wide, 9);
}

TRIAL("Frame: without the top edge the line runs across the full width") {
    ui::Frame one{"kot"};

    one.top(false);

    const Snapshot made = one.render({{0, 0}, {6, 3}});

    same(read(made, 0), std::string{"│    │"});
    same(read(made, 2), std::string{"└────┘"});
}

TRIAL("Frame: without the top edge there is no title") {
    ui::Frame one{"kot"};

    one.footer("stopka");
    one.top(false);

    const Snapshot made = one.render({{0, 0}, {12, 3}});

    ought(read(made, 0).find("kot") == std::string::npos);
    ought(read(made, 2).find("stopka") != std::string::npos);

    // A title that does not show takes no room: the frame is as wide as its edges.
    same(one.wants({80, 25}).wide, 2);
}

TRIAL("Frame: without the bottom edge there is no footer") {
    ui::Frame one{"kot"};

    one.footer("stopka");
    one.bottom(false);

    const Snapshot made = one.render({{0, 0}, {12, 3}});

    ought(read(made, 0).find("kot") != std::string::npos);
    ought(read(made, 2).find("stopka") == std::string::npos);
}

TRIAL("Frame: without a side, the column and the corners on that side disappear") {
    ui::Frame one;

    one.left(false);

    const Snapshot made = one.render({{0, 0}, {5, 3}});

    same(read(made, 0), std::string{"────┐"});
    same(read(made, 1), std::string{"    │"});
    same(read(made, 2), std::string{"────┘"});
}

TRIAL("Frame: a disabled edge takes no room from the content") {
    ui::Frame one;

    const auto inner = std::make_shared<Box>(ui::Size{10, 10}, U'x');

    one.children().add(inner);

    one.top(false);
    one.right(false);

    (void)one.render({{0, 0}, {8, 5}});

    same(inner->area().at.x, 1);
    same(inner->area().at.y, 0);
    same(inner->area().size.wide, 7);
    same(inner->area().size.high, 4);

    const ui::Size wants = one.wants({80, 25});

    same(wants.wide, 11);
    same(wants.high, 11);
}

TRIAL("Frame: the top edge alone is a plain line with a title") {
    ui::Frame one{"kot"};

    one.bottom(false);
    one.left(false);
    one.right(false);

    const auto inner = std::make_shared<Box>(ui::Size{9, 2}, U'x');

    one.children().add(inner);

    const Snapshot made = one.render({{0, 0}, {9, 3}});

    same(read(made, 0), std::string{"── kot ──"});
    same(read(made, 1), std::string{"xxxxxxxxx"});
    same(inner->area().at.y, 1);
}

TRIAL("Frame: all edges disabled is the same as no line") {
    ui::Frame one{"kot"};

    one.top(false);
    one.bottom(false);
    one.left(false);
    one.right(false);

    const auto inner = std::make_shared<Box>(ui::Size{4, 2}, U'x');

    one.children().add(inner);

    const Snapshot made = one.render({{0, 0}, {4, 2}});

    same(read(made, 0), std::string{"xxxx"});
    same(inner->area().size.wide, 4);
}

TRIAL("Frame: a scrolled frame draws the right piece") {
    ui::Scroll scroll{ui::Axis::vertical};

    const auto frame = std::make_shared<ui::Frame>("kot");

    frame->children().add(std::make_shared<Box>(ui::Size{4, 6}, U'x'));

    scroll.children().add(frame);

    scroll.shift({0, 2});

    const Snapshot made = scroll.render({{0, 0}, {6, 3}});

    same(read(made, 0), std::string{"│xxxx│"});
    same(read(made, 2), std::string{"│xxxx│"});
}

TRIAL("Frame: a frame within a frame") {
    ui::Frame outer{"z zewnątrz"};

    const auto inner = std::make_shared<ui::Frame>();

    inner->border(ui::Border::twin);
    inner->children().add(std::make_shared<Box>(ui::Size{2, 1}, U'x'));

    outer.children().add(inner);

    const Snapshot made = outer.render({{0, 0}, {8, 5}});

    same(read(made, 1), std::string{"│╔════╗│"});
    same(read(made, 2), std::string{"│║xxxx║│"});
    same(read(made, 3), std::string{"│╚════╝│"});
}

TRIAL("Line: a bare line across the whole given width") {
    ui::Line one;

    const Snapshot made = one.render({{0, 0}, {8, 1}});

    same(read(made, 0), std::string{"────────"});
}

// A separator does not push its parent wider: it asks for the width of its text alone, and without
// text for zero. The column stretches it, because it gives every child its whole width.
TRIAL("Line: asks for its text, and without text for nothing") {
    ui::Line bare;

    same(bare.wants({20, 3}), (ui::Size{0, 1}));

    ui::Line said{"Opcje"};

    // Five letters and a space on each side.
    same(said.wants({20, 3}), (ui::Size{7, 1}));
    same(said.wants({4, 3}), (ui::Size{4, 1}));

    // A separator that wraps is not a separator.
    same(said.wants({20, 0}), (ui::Size{7, 0}));
}

TRIAL("Line: the text stands in the line, in the middle by default") {
    ui::Line one{"Opcje"};

    Snapshot made = one.render({{0, 0}, {13, 1}});

    same(read(made, 0), std::string{"─── Opcje ───"});

    one.justify(ui::Justify::left);

    made = one.render({{0, 0}, {13, 1}});

    same(read(made, 0), std::string{" Opcje ──────"});

    one.justify(ui::Justify::right);

    made = one.render({{0, 0}, {13, 1}});

    same(read(made, 0), std::string{"────── Opcje"});
}

TRIAL("Line: a text that is too long is cut with an ellipsis") {
    ui::Line one{"Ustawienia panelu"};

    const Snapshot made = one.render({{0, 0}, {8, 1}});

    same(read(made, 0), std::string{" Ustawi…"});
}

TRIAL("Line: the weight comes from the style, and no line leaves only the text") {
    ui::Line twin{"Opcje", ui::Border::twin};

    same(read(twin.render({{0, 0}, {13, 1}}), 0), std::string{"═══ Opcje ═══"});

    ui::Line thick{std::string{}, ui::Border::thick};

    same(read(thick.render({{0, 0}, {4, 1}}), 0), std::string{"━━━━"});

    ui::Line none{"Opcje", ui::Border::none};

    same(read(none.render({{0, 0}, {13, 1}}), 0), std::string{"    Opcje"});
}

TRIAL("Line: takes no children") {
    ui::Line one;

    same(one.capacity(), 0);
}

// A separator is a piece of a frame: without a role it looks like `talcite.frame`, with a role like
// that role, with the text in `.title`. That is how the line above a panel's status row follows the
// active and the inactive panel.
TRIAL("Line: the line and the text take their color from the role, by default talcite.frame") {
    ui::Root top;

    const auto one = std::make_shared<ui::Line>("kot");

    top.children().add(one);

    same(one->role(), std::string{"talcite.frame"});

    Style edge{};
    Style told{};
    Style other{};

    edge.foreground = Color{false, 10, 20, 30};
    told.foreground = Color{false, 40, 50, 60};
    other.foreground = Color{false, 70, 80, 90};

    top.skin(ui::Skin{{{"talcite.frame", edge},
                       {"talcite.frame.title", told},
                       {"app.panel", other},
                       {"app.panel.title", other}}});

    const Snapshot first = top.render({{0, 0}, {9, 1}});

    same(read(first, 0), std::string{"── kot ──"});
    same(first.at({0, 0}).style.foreground.red, static_cast<std::uint8_t>(10));
    same(first.at({3, 0}).style.foreground.red, static_cast<std::uint8_t>(40));

    one->role("app.panel");

    const Snapshot second = top.render({{0, 0}, {9, 1}});

    same(second.at({0, 0}).style.foreground.red, static_cast<std::uint8_t>(70));
    same(second.at({3, 0}).style.foreground.red, static_cast<std::uint8_t>(70));
}

// A separator reaches the frame. A child does not paint outside its area, so the joint is placed by
// the frame, which recognizes the separator from the composed picture, not from the node's type.
// Thanks to that, it works through any nesting.
TRIAL("Frame: a separator inside gets joints") {
    ui::Frame frame;

    const auto column = std::make_shared<ui::Column>();

    column->children().add(std::make_shared<Box>(ui::Size{6, 1}, U'x'));
    column->children().add(std::make_shared<ui::Line>());
    column->children().add(std::make_shared<Box>(ui::Size{6, 1}, U'y'));

    frame.children().add(column);

    const Snapshot made = frame.render({{0, 0}, {8, 5}});

    same(read(made, 0), std::string{"┌──────┐"});
    same(read(made, 1), std::string{"│xxxxxx│"});
    same(read(made, 2), std::string{"├──────┤"});
    same(read(made, 3), std::string{"│yyyyyy│"});
    same(read(made, 4), std::string{"└──────┘"});
}

TRIAL("Frame: a joint goes in only when the weights match") {
    ui::Frame frame{{}, ui::FrameShape{ui::Border::twin}};

    const auto column = std::make_shared<ui::Column>();

    column->children().add(std::make_shared<ui::Line>());
    column->children().add(std::make_shared<ui::Line>(std::string{}, ui::Border::twin));

    frame.children().add(column);

    const Snapshot made = frame.render({{0, 0}, {8, 4}});

    // A thin line in a double frame does not join, as there is nothing to join it with.
    same(read(made, 1), std::string{"║──────║"});
    same(read(made, 2), std::string{"╠══════╣"});
}

TRIAL("Frame: only content touching both sides gets joints") {
    ui::Frame frame;

    const auto column = std::make_shared<ui::Column>();

    column->children().add(std::make_shared<ui::Line>("Opcje"));
    column->children().add(std::make_shared<Box>(ui::Size{12, 1}, U'x'));

    frame.children().add(column);

    const Snapshot made = frame.render({{0, 0}, {14, 4}});

    // The text does not reach the sides, but the line does, so there are joints.
    same(read(made, 1), std::string{"├── Opcje ───┤"});

    // Ordinary content does not.
    same(read(made, 2), std::string{"│xxxxxxxxxxxx│"});
}

// A text wider than the interior eats the line at the edges, and then there are no joints, because
// there is nothing to join. It is the same rule, seen from the other side.
TRIAL("Frame: a separator filled with text gets no joints") {
    ui::Frame frame;

    const auto column = std::make_shared<ui::Column>();

    column->children().add(std::make_shared<ui::Line>("Ustawienia"));

    frame.children().add(column);

    const Snapshot made = frame.render({{0, 0}, {8, 3}});

    same(read(made, 1), std::string{"│ Usta…│"});
}

// --- Frame background ----------------------------------------------------------------------------
//
// The interior takes the background of the frame's role, so a box in a colored skin is not
// transparent wherever the child does not cover it. Every window in Midnight Commander looks that
// way: the margin at the edge, the gap beside a shorter text and the row under the last child are
// in the window's color.

namespace {

// A skin with a navy frame and yellow ordinary text.
ui::Skin painted() {
    Style frame{};
    Style normal{};

    frame.background = Color{false, 0, 0, 160};
    frame.foreground = Color{false, 200, 200, 200};

    normal.foreground = Color{false, 255, 255, 0};

    return ui::Skin{{{"talcite.frame", frame}, {"talcite.normal", normal}}};
}

}

TRIAL("Frame: the interior takes the color of the frame's role, also where nothing stands") {
    ui::Root top;

    top.skin(painted());

    const auto frame = std::make_shared<ui::Frame>("Tytuł");

    frame->children().add(std::make_shared<ui::Span>("kot"));

    top.children().add(frame);

    const Snapshot made = top.render({{0, 0}, {12, 5}});

    const Color background = Color{false, 0, 0, 160};

    // The edge, the gap beside the shorter text and the row under the last child: in the window's
    // background.
    same(made.at({0, 0}).style.background, background);
    same(made.at({8, 1}).style.background, background);
    same(made.at({5, 3}).style.background, background);

    // A cell has one style, not two. Under a letter of the text stands the text's role, not the
    // frame's background: the background is what is visible where nobody painted anything, not a
    // layer underneath. In a colored skin the text's role has to give its own background, and the
    // demo skin does exactly that.
    same(read(made, 1), std::string{"│kot       │"});
    same(made.at({1, 1}).letter, U'k');
    same(made.at({1, 1}).style.foreground, (Color{false, 255, 255, 0}));
    ought(made.at({1, 1}).style.background.transparent);
}

TRIAL("Frame: without edges the background is still there") {
    ui::Root top;

    top.skin(painted());

    const auto frame = std::make_shared<ui::Frame>(std::string{},
                                                   ui::FrameShape{ui::Border::none});

    top.children().add(frame);

    const Snapshot made = top.render({{0, 0}, {6, 2}});

    same(made.at({0, 0}).style.background, (Color{false, 0, 0, 160}));
    same(made.at({5, 1}).style.background, (Color{false, 0, 0, 160}));
}

// The built-in skin has no colors, so the fill writes exactly what was on the sheet anyway.
TRIAL("Frame: under the built-in skin the background changes nothing") {
    ui::Frame frame;

    frame.children().add(std::make_shared<ui::Span>("kot"));

    const Snapshot made = frame.render({{0, 0}, {9, 4}});

    same(made.at({4, 2}), (Cell{U' ', Style{}, Shown::own}));
    same(made.at({7, 1}), (Cell{U' ', Style{}, Shown::own}));
}

// --- Transparency of containers ------------------------------------------------------------------
//
// A container has no look of its own: it lays out its children and nothing more. Painting its area
// with its own spaces would hide the parent's background wherever a child does not cover it, and
// that is most of the window.

TRIAL("Frame: the background passes through a column and a row into the gaps") {
    ui::Root top;

    top.skin(painted());

    const auto frame = std::make_shared<ui::Frame>();
    const auto column = std::make_shared<ui::Column>();

    column->children().add(std::make_shared<ui::Span>("ab"));
    column->children().add(std::make_shared<ui::Span>("cd"));

    frame->children().add(column);
    top.children().add(frame);

    const Snapshot made = top.render({{0, 0}, {10, 6}});

    const Color background = Color{false, 0, 0, 160};

    // Beside the shorter text, under the last child and in the corner of the interior.
    same(made.at({5, 1}).style.background, background);
    same(made.at({3, 3}).style.background, background);
    same(made.at({8, 4}).style.background, background);
}

// An empty node is not a spacer: without children it wants zero by zero, so the column gives it no
// row. A spacer needs a node that asks for a height, and the library has no such thing. We note
// this here, because a trial is the right place for such knowledge.
TRIAL("Frame: an empty node takes no room, and the background is still there") {
    ui::Root top;

    top.skin(painted());

    const auto frame = std::make_shared<ui::Frame>();
    const auto column = std::make_shared<ui::Column>();

    const auto apart = std::make_shared<ui::Node>();

    column->children().add(std::make_shared<ui::Span>("ab"));
    column->children().add(apart);
    column->children().add(std::make_shared<ui::Span>("cd"));

    frame->children().add(column);
    top.children().add(frame);

    const Snapshot made = top.render({{0, 0}, {10, 6}});

    same(read(made, 1), std::string{"│ab      │"});
    same(read(made, 2), std::string{"│cd      │"});

    // The gap beside the text and the row under the last child: in the window's background.
    same(made.at({5, 1}).style.background, (Color{false, 0, 0, 160}));
    same(made.at({3, 3}).style.background, (Color{false, 0, 0, 160}));
}


// --- A content over the right side ---------------------------------------------------------------
//
// A frame whose content lies over its right side (`overlaid`) gives the content the side's column
// as its last one. The side is drawn first and the content over it, so the side shows wherever the
// content leaves that column clear: a table keeps its scrollbar column there, and the bar, when it
// is drawn, stands on the side.

namespace {

// A node that paints its area with one letter but its last column, which is clear except on the
// rows `marked`, where it has another letter.
class Edged : public ui::Node {
public:
    Edged(char32_t what, char32_t mark, std::vector<int> marked) : what_(what), mark_(mark), marked_(std::move(marked)) {}

    // As much as it gets.
    [[nodiscard]] ui::Size wants(ui::Size room) const override { return room; }

protected:
    // The letter in every cell but the last column, the mark in the last column on the marked rows,
    // the rest clear.
    void paint(Canvas& onto) const override {
        const ui::Size given = onto.size();

        for (int y = 0; y < given.high; ++y) {
            for (int x = 0; x + 1 < given.wide; ++x) onto.put({x, y}, what_, Style{});

            if (std::find(marked_.begin(), marked_.end(), y) != marked_.end())
                onto.put({given.wide - 1, y}, mark_, Style{});
        }
    }

private:
    char32_t what_ = U'?';
    char32_t mark_ = U'?';
    std::vector<int> marked_;
};

}

TRIAL("Frame: an overlaid content takes the right side, which shows where the content is clear") {
    ui::Frame one;

    one.overlaid(true);

    const auto inner = std::make_shared<Edged>(U'x', U'#', std::vector<int>{1});

    one.children().add(inner);

    const Snapshot made = one.render({{0, 0}, {7, 5}});

    same(inner->area().at.x, 1);
    same(inner->area().size.wide, 6);
    same(read(made, 0), std::string{"┌─────┐"});
    same(read(made, 1), std::string{"│xxxxx│"});
    same(read(made, 2), std::string{"│xxxxx#"});
    same(read(made, 3), std::string{"│xxxxx│"});
    same(read(made, 4), std::string{"└─────┘"});

    one.overlaid(false);

    (void)one.render({{0, 0}, {7, 5}});

    same(inner->area().size.wide, 5);
}

TRIAL("Frame: an overlaid content wants the side as its own cell") {
    ui::Frame one;

    one.overlaid(true);
    one.children().add(std::make_shared<Box>(ui::Size{4, 2}, U'x'));

    same(one.wants({80, 25}).wide, 5);
    same(one.wants({80, 25}).high, 4);
}

TRIAL("Frame: a separator over the side still gets its joints") {
    ui::Frame frame;

    frame.overlaid(true);

    const auto column = std::make_shared<ui::Column>();

    column->children().add(std::make_shared<Box>(ui::Size{6, 1}, U'x'));
    column->children().add(std::make_shared<ui::Line>());
    column->children().add(std::make_shared<Box>(ui::Size{6, 1}, U'y'));

    frame.children().add(column);

    const Snapshot made = frame.render({{0, 0}, {8, 5}});

    same(read(made, 2), std::string{"├──────┤"});
}

// --- Double-width characters ---------------------------------------------------------------------
//
// A skin may draw the frame with CJK characters, and those take two cells. A side is then two
// cells, the horizontal edge goes every two cells, and the joints stand at the width of the side.
// For ordinary characters everything comes out as one, which is why no other expected picture in
// this file may change.

namespace {

// A skin that draws thin lines with double-width CJK characters.
ui::Skin widened() {
    return ui::Skin{{},
                    {{"talcite.line.thin.upper.left", U'口'},
                     {"talcite.line.thin.upper.right", U'口'},
                     {"talcite.line.thin.lower.left", U'口'},
                     {"talcite.line.thin.lower.right", U'口'},
                     {"talcite.line.thin.across", U'一'},
                     {"talcite.line.thin.upright", U'丨'},
                     {"talcite.line.thin.left.joint", U'卜'},
                     {"talcite.line.thin.right.joint", U'卜'}}};
}

}

TRIAL("Frame: double-width characters, a side takes two cells and the interior starts further in") {
    ui::Root top;

    top.skin(widened());

    const auto frame = std::make_shared<ui::Frame>();
    const auto inner = std::make_shared<Box>(ui::Size{20, 20}, U'x');

    frame->children().add(inner);
    top.children().add(frame);

    (void)top.render({{0, 0}, {12, 5}});

    same(inner->area().at.x, 2);
    same(inner->area().at.y, 1);
    same(inner->area().size.wide, 8);
    same(inner->area().size.high, 3);

    same(frame->wants({80, 25}).wide, 24);
}

TRIAL("Frame: double-width characters, the edge goes every two cells and the rest stays background") {
    ui::Root top;

    top.skin(widened());

    const auto frame = std::make_shared<ui::Frame>();

    frame->children().add(std::make_shared<Box>(ui::Size{20, 20}, U'x'));
    top.children().add(frame);

    // The interior has five cells: two horizontal characters take four, and the fifth stays a
    // space, not half a character.
    const Snapshot made = top.render({{0, 0}, {9, 3}});

    same(read(made, 0), std::string{"口一一 口"});
    same(read(made, 1), std::string{"丨xxxxx丨"});
    same(read(made, 2), std::string{"口一一 口"});
}

TRIAL("Frame: double-width characters, a joint stands at the width of the side") {
    ui::Root top;

    top.skin(widened());

    const auto frame = std::make_shared<ui::Frame>();
    const auto column = std::make_shared<ui::Column>();

    column->children().add(std::make_shared<Box>(ui::Size{6, 1}, U'x'));
    column->children().add(std::make_shared<ui::Line>());
    column->children().add(std::make_shared<Box>(ui::Size{6, 1}, U'y'));

    frame->children().add(column);
    top.children().add(frame);

    const Snapshot made = top.render({{0, 0}, {10, 5}});

    same(read(made, 1), std::string{"丨xxxxxx丨"});
    same(read(made, 2), std::string{"卜一一一卜"});
    same(read(made, 3), std::string{"丨yyyyyy丨"});

    // An odd interior: the last character of the line does not fit, and the joint goes in anyway,
    // because the line got as far as it could.
    const Snapshot odd = top.render({{0, 0}, {9, 5}});

    same(read(odd, 2), std::string{"卜一一 卜"});
}

TRIAL("Frame: double-width characters, an overlaid content stays inside the side it cannot split") {
    ui::Root top;

    top.skin(widened());

    const auto frame = std::make_shared<ui::Frame>();
    const auto inner = std::make_shared<Box>(ui::Size{20, 20}, U'x');

    frame->overlaid(true);
    frame->children().add(inner);
    top.children().add(frame);

    (void)top.render({{0, 0}, {12, 5}});

    same(inner->area().size.wide, 8);
    same(frame->wants({80, 25}).wide, 24);
}
