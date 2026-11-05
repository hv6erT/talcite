#include "proving.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "talcite/canvas.hpp"
#include "talcite/column.hpp"
#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/view.hpp"

using namespace talcite;

namespace {

// The text of row `y` of the view, without trailing spaces; a cleared cell nothing was painted on
// reads as a dot, so a row left alone can be told from a row of spaces.
std::string read(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const Cell& cell = one.at({x, y});

        if (cell.shown == Shown::none) made += '.';
        else if (cell.letter != 0) made += talcite::spelled(cell.letter);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

// A sheet of `size` with nothing on it, as the root clears the rows it paints.
View cleared(ui::Size size) {
    View made{size};

    made.clear({{0, 0}, size});

    return made;
}

// A row of one letter over the whole width, `high` rows tall. The letter can change with the
// announcement (`letter`) or without it (`unannounced`), which only the check of frames should see.
class Letter : public ui::Node {
public:
    explicit Letter(char32_t what) : what_(what) {}

    // A new letter, announced.
    void letter(char32_t what) {
        what_ = what;

        changed();
    }

    // A new letter nobody is told about.
    void unannounced(char32_t what) { what_ = what; }

    // Another height, announced: the column lays the letters under it out anew.
    void high(int given) {
        high_ = given;

        changed();
    }

    // No children.
    [[nodiscard]] int capacity() const override { return 0; }

    // The whole width, `high` rows.
    [[nodiscard]] ui::Size wants(ui::Size room) const override { return {room.wide, std::min(high_, room.high)}; }

    // How many times the node was painted.
    [[nodiscard]] int painted() const { return *painted_; }

protected:
    // The letter over the whole rectangle.
    void paint(Canvas& onto) const override {
        ++*painted_;

        const ui::Size given = onto.size();

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x) onto.put({x, y}, what_, Style{});
    }

private:
    char32_t what_;
    int high_ = 1;
    std::shared_ptr<int> painted_ = std::make_shared<int>(0);
};

// The check of frames off for one trial and on again after it, also when the trial fails.
struct Unchecked {
    // The check off.
    Unchecked() { ui::frames_checked(false); }

    // And on again.
    ~Unchecked() { ui::frames_checked(true); }

    Unchecked(const Unchecked&) = delete;
    Unchecked& operator=(const Unchecked&) = delete;
};

// Three letters one under another.
struct Three {
    std::shared_ptr<ui::Column> column = std::make_shared<ui::Column>();
    std::shared_ptr<Letter> a = std::make_shared<Letter>(U'a');
    std::shared_ptr<Letter> b = std::make_shared<Letter>(U'b');
    std::shared_ptr<Letter> c = std::make_shared<Letter>(U'c');

    // The column with its letters.
    Three() { column->children().add({a, b, c}); }
};

// What a node lays out from another one laid out after it: the source turns what it was asked
// into what it shows only in its layout, and the echo, standing first, repeats what the source
// shows. So a change of the source reaches the echo only in a second pass of the layout.
class Source : public ui::Node {
public:
    // What the source will show from its next layout on, announced.
    void asked(char32_t what) {
        asked_ = what;

        changed();
    }

    // What the source showed at its last layout.
    [[nodiscard]] char32_t laid() const { return laid_; }

    // No children.
    [[nodiscard]] int capacity() const override { return 0; }

    // One row.
    [[nodiscard]] ui::Size wants(ui::Size room) const override { return {room.wide, std::min(1, room.high)}; }

protected:
    // Takes what was asked, and says so when it differs from what it showed.
    void layout(ui::Size) override {
        if (laid_ == asked_) return;

        laid_ = asked_;

        changed();
    }

    // What it laid out.
    void paint(Canvas& onto) const override { onto.put({0, 0}, laid_, Style{}); }

private:
    char32_t asked_ = U'1';
    char32_t laid_ = U'1';
};

// The echo of a source (`Source`).
class Echo : public ui::Node {
public:
    explicit Echo(std::shared_ptr<const Source> source) : source_(std::move(source)) {}

    // No children.
    [[nodiscard]] int capacity() const override { return 0; }

    // One row.
    [[nodiscard]] ui::Size wants(ui::Size room) const override { return {room.wide, std::min(1, room.high)}; }

protected:
    // Repeats what the source showed, and says so when it differs from what it repeated.
    void layout(ui::Size) override {
        if (shown_ == source_->laid()) return;

        shown_ = source_->laid();

        changed();
    }

    // What it repeats.
    void paint(Canvas& onto) const override { onto.put({0, 0}, shown_, Style{}); }

private:
    std::shared_ptr<const Source> source_;
    char32_t shown_ = U'1';
};

// A node whose layout announces a change every time, so the layout never settles.
class Restless : public ui::Node {
public:
    // No children.
    [[nodiscard]] int capacity() const override { return 0; }

protected:
    // Always something new.
    void layout(ui::Size) override { changed(); }

    // Nothing.
    void paint(Canvas&) const override {}
};

}

// A canvas paints in the node's own coordinates and cuts what it paints to the node's rectangle and
// to the rectangles of all above it.
TRIAL("Canvas: a child paints from its own corner, cut to its rectangle and to its parent's") {
    View sheet = cleared({10, 3});

    Canvas whole{sheet};
    Canvas parent = whole.within({{1, 1}, {6, 1}});
    Canvas child = parent.within({{2, 0}, {8, 1}});

    child.put({0, 0}, std::string_view{"abcdefgh"}, Style{});

    same(read(sheet, 0), std::string{".........."});
    same(read(sheet, 1), std::string{"...abcd..."});
    same(child.size(), (ui::Size{8, 1}));
    same(child.seen(), (ui::Area{{0, 0}, {4, 1}}));
}

// A clip cuts the painting without moving the corner: the window of a scroller without its bars.
TRIAL("Canvas: clipped keeps the corner and paints only inside the clip") {
    View sheet = cleared({6, 2});

    Canvas whole{sheet};
    Canvas cut = whole.within({{1, 0}, {5, 2}}).clipped({{0, 1}, {3, 1}});

    cut.fill({{0, 0}, {5, 2}}, Style{});
    cut.put({0, 1}, U'x', Style{});

    same(read(sheet, 0), std::string{"......"});
    same(read(sheet, 1), std::string{".x  .."});
    same(cut.seen(), (ui::Area{{0, 1}, {3, 1}}));
}

// The edge of a clip is an edge as that of a sheet: half a double-width character does not stand
// beside it, and a mark that joins the glyph to its left joins nothing outside.
TRIAL("Canvas: a double-width character cut by the edge becomes a space") {
    View sheet = cleared({4, 1});

    Canvas whole{sheet};
    Canvas left = whole.within({{0, 0}, {3, 1}});

    left.put({2, 0}, U'界', Style{});

    same(sheet.at({2, 0}).letter, U' ');
    ought(sheet.at({3, 0}).shown == Shown::none);

    Canvas right = whole.within({{3, 0}, {1, 1}});

    right.put({0, 0}, U'́', Style{});

    ought(sheet.at({3, 0}).shown == Shown::none);
}

// A shade dims what lies under it and keeps its character; over nothing it stays a shade, which the
// terminal shows as an empty cell in the shade's style.
TRIAL("Canvas: a shade keeps the character under it, and over nothing it is a shade") {
    View sheet = cleared({3, 1});

    Canvas whole{sheet};

    const Style dim{.foreground = *Color::indexed(8)};

    whole.put({0, 0}, U'a', Style{});
    whole.shade({{0, 0}, {2, 1}}, dim);

    same(sheet.at({0, 0}).letter, U'a');
    ought(sheet.at({0, 0}).style == dim);
    ought(sheet.at({1, 0}).shown == Shown::shade);
    ought(sheet.at({2, 0}).shown == Shown::none);
}

// A foreground character without a background of its own takes that of what lies under it, and
// over nothing it stays a foreground cell, which takes its background from what comes later.
TRIAL("Canvas: a foreground character takes the background from below") {
    View sheet = cleared({2, 1});

    Canvas whole{sheet};

    const Style ground{.background = *Color::indexed(4)};

    whole.fill({{0, 0}, {1, 1}}, ground);
    whole.foreground({0, 0}, U'x', Style{});
    whole.foreground({1, 0}, U'y', Style{});

    same(sheet.at({0, 0}).letter, U'x');
    ought(sheet.at({0, 0}).style.background == ground.background);
    ought(sheet.at({0, 0}).shown == Shown::own);
    ought(sheet.at({1, 0}).shown == Shown::foreground);
}

// What lies under a node can be read in its own coordinates; outside the clip there is nothing.
TRIAL("Canvas: at reads the sheet under the node, and nothing outside the clip") {
    View sheet = cleared({4, 1});

    Canvas whole{sheet};

    whole.put({0, 0}, std::string_view{"abcd"}, Style{});

    const Canvas inner = whole.within({{1, 0}, {2, 1}});

    same(inner.at({0, 0}).letter, U'b');
    ought(inner.at({2, 0}) == Cell{});
}

// Only the rows of an announced change are painted again; the rest of the screen stays as the last
// frame left it, even where something changed without a word, which the check of frames, off here,
// would find.
TRIAL("Root: a frame paints only the rows of an announced change") {
    const Unchecked off;

    ui::Root top;
    Three three;

    top.children().add(three.column);

    View made = top.render({{0, 0}, {3, 3}});

    same(read(made, 0), std::string{"aaa"});

    const int before = three.b->painted();

    three.a->unannounced(U'x');
    three.c->letter(U'z');

    made = top.render({{0, 0}, {3, 3}});

    same(read(made, 0), std::string{"aaa"});
    same(read(made, 1), std::string{"bbb"});
    same(read(made, 2), std::string{"zzz"});
    same(three.b->painted(), before);

    top.repaint();

    same(read(top.render({{0, 0}, {3, 3}}), 0), std::string{"xxx"});
}

// A frame without any announcement paints nothing at all. The check of frames, which paints the
// whole tree once more after every frame, is off, so the count is the frame's own.
TRIAL("Root: a frame after no change paints nothing") {
    const Unchecked off;

    ui::Root top;
    Three three;

    top.children().add(three.column);

    (void)top.render({{0, 0}, {3, 3}});

    const int before = three.a->painted() + three.b->painted() + three.c->painted();

    (void)top.render({{0, 0}, {3, 3}});

    same(three.a->painted() + three.b->painted() + three.c->painted(), before);
}

// With the check of frames on, a change nobody announced fails the frame that should show it.
TRIAL("Root: the check of frames finds a change nobody announced") {
    ui::Root top;
    Three three;

    top.children().add(three.column);

    (void)top.render({{0, 0}, {3, 3}});

    three.b->unannounced(U'y');

    threw((void)top.render({{0, 0}, {3, 3}}));
}

// A node that moves need not announce it: the layout finds that it stands elsewhere and repaints
// both its old place and the new one.
TRIAL("Root: a node pushed down by its neighbour is painted again in both places") {
    ui::Root top;
    Three three;

    top.children().add(three.column);

    (void)top.render({{0, 0}, {3, 4}});

    three.a->high(2);

    const View made = top.render({{0, 0}, {3, 4}});

    same(read(made, 0), std::string{"aaa"});
    same(read(made, 1), std::string{"aaa"});
    same(read(made, 2), std::string{"bbb"});
    same(read(made, 3), std::string{"ccc"});

    three.a->high(1);

    const View back = top.render({{0, 0}, {3, 4}});

    same(read(back, 2), std::string{"ccc"});
    same(read(back, 3), std::string{"..."});
}

// A change announced in the middle of the layout is laid out again before the frame is painted: the
// echo, laid out before its source, repeats what the source took in the same layout.
TRIAL("Root: a change announced in the layout is laid out again in the same frame") {
    ui::Root top;

    const auto column = std::make_shared<ui::Column>();
    const auto source = std::make_shared<Source>();
    const auto echo = std::make_shared<Echo>(source);

    column->children().add({echo, source});
    top.children().add(column);

    (void)top.render({{0, 0}, {1, 2}});

    source->asked(U'2');

    const View made = top.render({{0, 0}, {1, 2}});

    same(read(made, 0), std::string{"2"});
    same(read(made, 1), std::string{"2"});
}

// A layout that announces a change every time never settles: the frame gives up after a few passes
// rather than hanging, and the check of frames reports the node.
TRIAL("Root: a layout that never settles ends the frame, and the check reports it") {
    ui::Root top;

    top.children().add(std::make_shared<Restless>());

    threw((void)top.render({{0, 0}, {2, 2}}));

    const Unchecked off;

    (void)top.render({{0, 0}, {2, 2}});
}

// Text of plain ASCII goes into its cells by the short way, and that way mends what it overwrites
// as the long one does: a letter over either half of a double-width character turns the other half
// into a space, so no half of a character stays behind.
TRIAL("Canvas: ASCII over a double-width character leaves no half of it") {
    View sheet = cleared({4, 1});

    Canvas whole{sheet};

    whole.put({0, 0}, U'界', Style{});
    whole.put({0, 0}, std::string_view{"a"}, Style{});

    same(sheet.at({0, 0}).letter, U'a');
    same(sheet.at({1, 0}).letter, U' ');

    whole.put({2, 0}, U'界', Style{});
    whole.put({3, 0}, std::string_view{"b"}, Style{});

    same(sheet.at({2, 0}).letter, U' ');
    same(sheet.at({3, 0}).letter, U'b');

    // A wide character below the CJK block: a Hangul jamo.
    whole.put({0, 0}, U'\u1100', Style{});
    whole.put({0, 0}, std::string_view{"c"}, Style{});

    same(sheet.at({0, 0}).letter, U'c');
    same(sheet.at({1, 0}).letter, U' ');
}

// A shadow keeps a double-width character whole, since only its style changes, while a letter over
// one half of a neighbour turns the other half into a space.
TRIAL("Canvas: a shadow keeps a double-width character whole and a letter cuts it") {
    View sheet = cleared({6, 1});

    Canvas whole{sheet};

    whole.put({0, 0}, U'映', Style{});
    whole.put({2, 0}, U'画', Style{});
    whole.put({4, 0}, U'名', Style{});

    const Style dark{.faint = true};

    whole.put({1, 0}, U'a', Style{});
    whole.shade({{2, 0}, {2, 1}}, dark);
    whole.put({4, 0}, U'b', Style{});

    same(sheet.at({0, 0}).letter, U' ');
    same(sheet.at({1, 0}).letter, U'a');
    same(sheet.at({2, 0}).letter, U'画');
    same(sheet.at({3, 0}).letter, char32_t{0});
    ought(sheet.at({2, 0}).style.faint);
    ought(sheet.at({3, 0}).style.faint);
    same(sheet.at({4, 0}).letter, U'b');
    same(sheet.at({5, 0}).letter, U' ');
}

// A foreground character with a background of its own is an ordinary cell, and so is a plain one
// in the same colors: it covers the ground with its transparent background, just like text. A zero
// character lays down nothing.
TRIAL("Canvas: a foreground with its own background and a plain cell cover the ground") {
    View sheet = cleared({3, 1});

    Canvas whole{sheet};

    const Style ground{.background = *Color::indexed(4)};
    const Style red{.foreground = *Color::indexed(1)};
    const Style papered{.foreground = *Color::indexed(1), .background = *Color::indexed(2)};

    whole.fill({{0, 0}, {3, 1}}, ground);
    whole.foreground({0, 0}, U'▐', papered);
    whole.put({1, 0}, U'▌', red);
    whole.foreground({2, 0}, 0, red);

    ought(sheet.at({0, 0}).style.background == papered.background);
    ought(sheet.at({0, 0}).shown == Shown::own);
    ought(sheet.at({1, 0}).style.background.transparent);
    ought(sheet.at({2, 0}).style.background == ground.background);
    same(sheet.at({2, 0}).letter, U' ');
}

// A link painted through a child's canvas marks the child's cells on the sheet; whatever is put
// over a cell later takes the mark away, and a shadow keeps it, since the text below it stays.
TRIAL("Canvas: a hyperlink marks what is painted, a cover takes it away and a shadow keeps it") {
    View sheet = cleared({8, 1});

    Canvas whole{sheet};
    Canvas child = whole.within({{2, 0}, {3, 1}});

    child.put({0, 0}, std::string_view{"abc"}, Style{});

    const Hyperlink made = hyperlink("https://example.org/");

    child.link({{0, 0}, {3, 1}}, made);

    same(sheet.at({2, 0}).link, made.mark);
    same(sheet.at({4, 0}).link, made.mark);
    same(sheet.at({5, 0}).link, std::uint32_t{0});
    same(sheet.links().size(), std::size_t{1});

    whole.put({2, 0}, U'x', Style{});

    same(sheet.at({2, 0}).link, std::uint32_t{0});

    whole.shade({{3, 0}, {1, 1}}, Style{});

    same(sheet.at({3, 0}).link, made.mark);
}
