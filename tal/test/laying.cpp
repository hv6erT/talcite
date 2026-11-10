#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/document.hpp"
#include "tal/geometry.hpp"
#include "tal/node.hpp"
#include "tal/style.hpp"
#include "tal/surface.hpp"
#include "tal/text.hpp"

#include "tal/testing.hpp"

// The tree of tal::ui: children owned by their parent, the box of margin, border and padding,
// measuring remembered until something changes, nodes arranged only when they or something under
// them changed, and painting only where the screen was damaged, in order, clipped to the parent.

namespace {

using tal::Area;
using tal::Canvas;
using tal::Colors;
using tal::Document;
using tal::Effect;
using tal::Limits;
using tal::Look;
using tal::Node;
using tal::Property;
using tal::Size;
using tal::Spot;
using tal::State;
using tal::StyleSheet;
using tal::Surface;
using tal::Tally;

// A line of text, as wide as the text and one row high, painted in the node's look.
class Label : public Node {
public:
    explicit Label(std::string said) : text(*this, Effect::layout, std::move(said)) {}

    Property<std::string> text;

    [[nodiscard]] int measures() const { return *measures_; }

    int arranges = 0;

    void lit(bool on) { state(State::focus, on); }

    void again() { relayout(); }

    [[nodiscard]] std::string_view key_glyph() const { return glyph("elide"); }

    [[nodiscard]] Look key_look() const { return look("key"); }

protected:
    // As wide as the text, one row.
    Size measure(Limits room) const override {
        ++*measures_;

        return Size{std::max(room.least.wide, tal::text::width(*text)), std::max(room.least.high, 1)};
    }

    // Nothing inside; only counted.
    void arrange(Area inside) override {
        ++arranges;

        Node::arrange(inside);
    }

    // The text at the start of the inside.
    void paint(Canvas& onto) const override { (void)onto.put({0, 0}, *text, look()); }

private:
    std::shared_ptr<int> measures_ = std::make_shared<int>(0);
};

// Children side by side, each as wide as it measures, all as high as the row.
class Row : public Node {
protected:
    // The widths added up, the highest child's height.
    Size measure(Limits room) const override {
        Size made{0, 0};

        for (Node& one : children()) {
            const Size wanted = one.measured(Limits{{0, 0}, room.most});

            made = Size{made.wide + wanted.wide, std::max(made.high, wanted.high)};
        }

        return Size{std::max(made.wide, room.least.wide), std::max(made.high, room.least.high)};
    }

    // Each child after the one before it.
    void arrange(Area inside) override {
        int x = inside.at.x;

        for (Node& one : children()) {
            const Size wanted = one.measured(Limits{{0, 0}, inside.size});

            one.placed(Area{{x, inside.at.y}, {wanted.wide, inside.size.high}});
            x += wanted.wide;
        }
    }
};

// A node that breaks a rule of layout on purpose, in the way `how` says.
class Rogue : public Node {
public:
    explicit Rogue(int how) : how_(how), self_(this) {}

protected:
    // Changes a class while measured, when asked to.
    Size measure(Limits room) const override {
        if (how_ == 0) self_->add_class("bad");

        return room.least;
    }

    // Asks for a new layout while arranged, or arranges its child twice, when asked to.
    void arrange(Area inside) override {
        if (how_ == 1) relayout();

        if (how_ == 2)
            for (int twice = 0; twice < 2; ++twice)
                for (Node& one : children()) one.placed(Area{{0, twice}, inside.size});
    }

private:
    int how_;
    Rogue* self_;
};

// A node that lets its children spill out: each is placed twenty cells wide.
class Spilling : public Node {
protected:
    // Every child twenty cells wide, whatever room there is.
    void arrange(Area inside) override {
        for (Node& one : children()) one.placed(Area{inside.at, {20, 1}});
    }
};

// The letters of row `y` of a surface.
std::string row(const Surface& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) made += one.at({x, y}).cluster;

    return made;
}

// A document of `wide` by `high` cells on a terminal with all colors and Unicode, with `sheet`.
std::unique_ptr<Document> documented(int wide, int high, std::string_view sheet = R"({"rules": []})") {
    auto made = std::make_unique<Document>();

    made->sheet(StyleSheet::parsed(sheet));
    made->display(Size{wide, high}, Colors::rgb, true);

    return made;
}

TRIAL("Children: added, inserted, removed and cleared, with their parent and document") {
    auto top = documented(10, 2);
    Row& line = top->children().add<Row>();
    Label& b = line.children().add<Label>("b");
    Node& a = line.children().insert(0, std::make_shared<Label>("a"));
    std::vector<Node*> order;

    for (Node& one : line.children()) order.push_back(&one);

    SAME(order, (std::vector<Node*>{&a, &b}));
    OUGHT(b.parent() == &line);
    OUGHT(b.document() == top.get());
    OUGHT(top->document() == top.get());

    const std::shared_ptr<Node> gone = line.children().remove(a);

    OUGHT(gone.get() == &a);
    OUGHT(a.parent() == nullptr);
    OUGHT(a.document() == nullptr);
    SAME(line.children().size(), std::size_t{1});

    const std::shared_ptr<Node> kept = b.shared_from_this();

    line.children().clear();

    OUGHT(line.children().empty());
    OUGHT(kept->parent() == nullptr);
    OUGHT(kept->document() == nullptr);
}

TRIAL("Children: a node stands in one place of one tree, and only a child can be removed") {
    auto top = documented(10, 2);
    Row& line = top->children().add<Row>();
    Label& one = line.children().add<Label>("x");
    auto stray = std::make_shared<Label>("y");

    THREW(top->children().add(one.shared_from_this()));
    THREW(one.children().add(line.shared_from_this()));
    THREW(line.children().add(std::make_shared<Document>()));
    THREW(line.children().add(std::shared_ptr<Node>{}));
    THREW(line.children().insert(5, std::make_shared<Label>("z")));
    THREW((void)line.children().remove(*stray));
    SAME(line.children().size(), std::size_t{1});

    auto loose = std::make_shared<Row>();
    Label& inner = loose->children().add<Label>("in");

    THREW(inner.children().add(loose));
    THREW(stray->children().add(stray));
    OUGHT(loose->parent() == nullptr);
    OUGHT(stray->children().empty());
}

TRIAL("Children: a child kept elsewhere outlives its parent without a parent or document") {
    std::shared_ptr<Node> kept;

    {
        auto top = documented(10, 2);
        Row& line = top->children().add<Row>();

        kept = line.children().add<Label>("x").shared_from_this();
        kept->children().add<Label>("inner");
    }

    OUGHT(kept->parent() == nullptr);
    OUGHT(kept->document() == nullptr);

    for (Node& one : kept->children()) OUGHT(one.document() == nullptr);
}

TRIAL("Node: find looks through the whole document by name and kind") {
    auto top = documented(10, 2);
    Row& line = top->children().add<Row>();
    Label& deep = line.children().add<Label>("x");

    deep.id("deep");
    line.id("line");

    OUGHT(deep.find<Row>("line") == &line);
    OUGHT(line.find<Label>("deep") == &deep);
    OUGHT(top->find<Label>("line") == nullptr);
    OUGHT(top->find<Node>("nobody") == nullptr);
}

TRIAL("Box: margin, border and padding around the content, bounded by the room") {
    auto top = documented(20, 6, R"({"rules": [{"select": "boxed", "style": {"border": "thin", "padding": [0, 1, 0, 1],
                                                                             "margin": [1, 0, 0, 2]}}]})");
    Label& one = top->children().add<Label>("abc");

    one.classes({"boxed"});
    (void)top->frame();

    SAME(one.measured(Limits{{0, 0}, {tal::endless, tal::endless}}), (Size{9, 4}));
    SAME(one.measured(Limits{{12, 0}, {tal::endless, 3}}), (Size{12, 3}));
    SAME(one.area(), (Area{{2, 1}, {18, 5}}));

    const Surface& screen = top->frame();

    SAME(screen.at({2, 1}).cluster, std::string{"\u250c"});
    SAME(screen.at({3, 1}).cluster, std::string{"\u2500"});
    SAME(screen.at({19, 1}).cluster, std::string{"\u2510"});
    SAME(screen.at({1, 2}).cluster, std::string{" "});
    SAME(screen.at({2, 2}).cluster, std::string{"\u2502"});
    SAME(screen.at({3, 2}).cluster, std::string{" "});
    SAME(screen.at({4, 2}).cluster, std::string{"a"});
    SAME(screen.at({2, 5}).cluster, std::string{"\u2514"});

    Node& holder = top->children().add<Node>();
    Label& held = holder.children().add<Label>("in");

    top->children().remove(one);
    holder.classes({"boxed"});
    (void)top->frame();

    SAME(holder.area(), (Area{{2, 1}, {18, 5}}));
    SAME(held.area(), (Area{{2, 1}, {14, 3}}));
}

TRIAL("Layout: a measure is remembered for the same room until the node changes") {
    Label one{"abcd"};

    SAME(one.measured(Limits{{0, 0}, {10, 10}}), (Size{4, 1}));
    SAME(one.measured(Limits{{0, 0}, {10, 10}}), (Size{4, 1}));
    SAME(one.measures(), 1);

    SAME(one.measured(Limits{{6, 0}, {10, 10}}), (Size{6, 1}));
    SAME(one.measures(), 2);

    one.text = "abcdefgh";

    SAME(one.measured(Limits{{6, 0}, {10, 10}}), (Size{8, 1}));
    SAME(one.measures(), 3);

    one.text = "abcdefgh";
    (void)one.measured(Limits{{6, 0}, {10, 10}});
    SAME(one.measures(), 3);
}

TRIAL("Frame: nothing changed, nothing done; one leaf changed, only its line is laid out again") {
    auto top = documented(20, 3);
    Row& line = top->children().add<Row>();
    Label& left = line.children().add<Label>("left");
    Label& right = line.children().add<Label>("right");

    (void)top->frame();
    OUGHT(!top->due());

    (void)top->frame();
    SAME(top->tally(), Tally{});

    left.text = "LEFT";
    OUGHT(top->due());

    const Surface& screen = top->frame();

    SAME(top->tally().measured, std::uint64_t{3});
    SAME(top->tally().arranged, std::uint64_t{3});
    SAME(left.arranges, 2);
    SAME(right.arranges, 1);
    SAME(row(screen, 0), std::string{"LEFTright           "});
    SAME((std::vector<Area>{screen.damaged().begin(), screen.damaged().end()}), (std::vector<Area>{{{0, 0}, {4, 1}}}));
}

TRIAL("Frame: only nodes that meet the damage are painted, and only there") {
    auto top = documented(20, 3);
    Row& line = top->children().add<Row>();
    Label& left = line.children().add<Label>("left");
    Label& right = line.children().add<Label>("right");

    (void)top->frame();
    SAME(top->tally().painted, std::uint64_t{4});

    right.text = "RIGHT";

    (void)top->frame();
    SAME(top->tally().painted, std::uint64_t{3});

    right.again();

    const Surface& screen = top->frame();

    SAME(top->tally().painted, std::uint64_t{3});
    SAME(top->tally().measured, std::uint64_t{3});
    SAME((std::vector<Area>{screen.damaged().begin(), screen.damaged().end()}), (std::vector<Area>{{{4, 0}, {5, 1}}}));
    SAME(left.measures(), 1);
}

TRIAL("Frame: a node that grows moves the ones after it, and the old places are cleared") {
    auto top = documented(12, 1);
    Row& line = top->children().add<Row>();
    Label& left = line.children().add<Label>("ab");

    line.children().add<Label>("cd");

    SAME(row(top->frame(), 0), std::string{"abcd        "});

    left.text = "abcde";
    SAME(row(top->frame(), 0), std::string{"abcdecd     "});

    left.text = "a";
    SAME(row(top->frame(), 0), std::string{"acd         "});

    (void)line.children().remove(left);
    SAME(row(top->frame(), 0), std::string{"cd          "});
}

TRIAL("Paint: background and border first, then the content, then the children, the later on top") {
    auto top = documented(8, 4, R"({"rules": [{"select": "pane", "style": {"border": "twin", "paper": "blue"}}]})");
    Node& pane = top->children().add<Node>();

    pane.classes({"pane"});
    pane.children().add<Label>("abcdefghij");
    pane.children().add<Label>("XY");

    const Surface& screen = top->frame();

    SAME(row(screen, 0), std::string{"\u2554\u2550\u2550\u2550\u2550\u2550\u2550\u2557"});
    SAME(row(screen, 1), std::string{"\u2551XY    \u2551"});
    SAME(row(screen, 2), std::string{"\u2551      \u2551"});
    SAME(row(screen, 3), std::string{"\u255a\u2550\u2550\u2550\u2550\u2550\u2550\u255d"});
    SAME(screen.at({1, 1}).look.paper, (std::optional<tal::Color>{tal::Color{0, 0, 128}}));
    OUGHT(!screen.at({1, 1}).look.ink.has_value());
}

TRIAL("Style: classes and states from the sheet, an ancestor's class restyling what is under it") {
    auto top = documented(10, 2, R"({"rules": [{"select": "label", "style": {"bold": true}},
                                               {"select": "label:focus", "style": {"reverse": true}},
                                               {"select": "loud label", "style": {"under": true}},
                                               {"select": "wide label", "style": {"padding": [0, 2, 0, 2]}},
                                               {"select": "label::key", "style": {"ink": "red"}}]})");
    Row& line = top->children().add<Row>();
    Label& one = line.children().add<Label>("x");

    one.classes({"label"});

    const Surface& screen = top->frame();

    OUGHT(screen.at({0, 0}).look.bold);
    SAME(one.key_look().ink, (std::optional<tal::Color>{tal::Color{128, 0, 0}}));
    SAME(one.key_glyph(), std::string_view{"\u2026"});

    one.lit(true);
    (void)top->frame();
    OUGHT(top->frame().at({0, 0}).look.reverse);

    line.add_class("loud");
    (void)top->frame();
    SAME(top->tally().measured, std::uint64_t{0});
    SAME(top->tally().styled, std::uint64_t{2});
    OUGHT(top->frame().at({0, 0}).look.under);

    line.drop_class("loud");
    line.add_class("wide");
    (void)top->frame();
    OUGHT(top->tally().measured > 0);
    SAME(row(top->frame(), 0).substr(0, 5), std::string{"  x  "});
    OUGHT(line.has_class("wide"));
    OUGHT(!line.has_class("loud"));
}

TRIAL("Property: a layout property lays out, the same value does nothing") {
    auto top = documented(10, 1);
    Label& one = top->children().add<Label>("ab");

    (void)top->frame();

    one.text = "ab";
    OUGHT(!top->due());

    one.text = "abc";
    OUGHT(top->due());
    SAME(*one.text, std::string{"abc"});
    SAME(one.text->size(), std::size_t{3});
}

TRIAL("Display: a new size or level makes the whole screen again") {
    auto top = documented(6, 1, R"({"rules": [{"select": "label", "style": {"mono": {"bold": true}, "16": {"ink": "red"}}}]})");
    Label& one = top->children().add<Label>("ab");

    one.classes({"label"});
    (void)top->frame();
    OUGHT(top->frame().at({0, 0}).look.ink.has_value());

    top->display(Size{8, 2}, Colors::mono, true);
    OUGHT(top->due());

    const Surface& screen = top->frame();

    SAME(screen.size(), (Size{8, 2}));
    OUGHT(screen.at({0, 0}).look.bold);
    OUGHT(!screen.at({0, 0}).look.ink.has_value());
    SAME((std::vector<Area>{screen.damaged().begin(), screen.damaged().end()}), (std::vector<Area>{{{0, 0}, {8, 2}}}));
}

TRIAL("Clip: a child shows only inside its parent's border") {
    auto top = documented(10, 2, R"({"rules": [{"select": "thin", "style": {"margin": [0, 6, 0, 0]}}]})");
    Spilling& narrow = top->children().add<Spilling>();

    narrow.classes({"thin"});
    narrow.children().add<Label>("abcdefgh");

    const Surface& screen = top->frame();

    SAME(narrow.area().size, (Size{4, 2}));
    SAME(row(screen, 0), std::string{"abcd      "});
}

#ifndef NDEBUG

TRIAL("Debug: changing the tree while it is laid out is refused, and the next frame works") {
    for (int how = 0; how < 3; ++how) {
        auto top = documented(10, 2);
        Rogue& bad = top->children().add<Rogue>(how);

        bad.children().add<Label>("x");

        THREW((void)top->frame());

        top->children().remove(bad);

        (void)top->frame();
        OUGHT(!top->due());
    }
}

#endif

}
