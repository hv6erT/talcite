#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "tal/document.hpp"
#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/key.hpp"
#include "tal/node.hpp"
#include "tal/scene.hpp"
#include "tal/style.hpp"
#include "tal/surface.hpp"
#include "tal/text.hpp"

#include "tal/testing.hpp"

// The scene: a document driven by hand on a manual clock, as the trials of programs drive their
// windows, and read back from its last frame.

namespace {

using namespace std::chrono_literals;

using tal::Area;
using tal::Arranged;
using tal::Axis;
using tal::MouseButton;
using tal::Canvas;
using tal::Caret;
using tal::Code;
using tal::Color;
using tal::Colors;
using tal::Document;
using tal::Effect;
using tal::Hooks;
using tal::Key;
using tal::KeyEvent;
using tal::Look;
using tal::MouseEvent;
using tal::Node;
using tal::PasteEvent;
using tal::Press;
using tal::Property;
using tal::Size;
using tal::Spot;
using tal::State;
using tal::Timer;
using tal::testing::Scene;

// A node that writes its text at (0, 0) in its look, and takes the focus when asked to.
class Worded : public Node {
public:
    explicit Worded(bool takes = false) { focusable(takes); }

    Property<std::string> text{*this, Effect::paint, ""};

    Property<Look> look{*this, Effect::paint, Look{}};

    // Whether the pointer is over it.
    [[nodiscard]] bool hovered() const { return states().has(State::hover); }

protected:
    // Puts the text at the start.
    void paint(Canvas& onto) const override { (void)onto.put({0, 0}, *text, *look); }

    // At the end of the text.
    std::optional<Caret> caret() const override { return Caret{{tal::text::width(*text), 0}, Caret::Form::bar, false}; }
};

// A node of the class "knob" that writes "x" in its style's look, and knows the glyph that marks a
// menu with more in it.
class Styled : public Node {
public:
    Styled() { classes({"knob"}); }

    // The glyph for a menu with more in it, for the document's terminal.
    [[nodiscard]] std::string_view more() const { return glyph("menu.more"); }

protected:
    // Puts "x" at the start in the sheet's look.
    void paint(Canvas& onto) const override { (void)onto.put({0, 0}, "x", look()); }
};

// A node that places its one child at (3, 1), four cells wide and two high, and any further one
// at its corner without width.
class Corner : public Node {
protected:
    // The first child in its place, the rest without room.
    void arrange(Area inside) override {
        (void)inside;

        bool first = true;

        for (Node& one : children()) {
            one.placed(first ? Area{{3, 1}, {4, 2}} : Area{{0, 0}, {0, 2}});
            first = false;
        }
    }
};

// A node that places its children one under another, a row each.
class Stack : public Node {
protected:
    // Child after child, one row high.
    void arrange(Area inside) override {
        int y = inside.at.y;

        for (Node& one : children()) one.placed(Area{{inside.at.x, y++}, {inside.size.wide, 1}});
    }
};

// What reached the document, in order: keys as their letters or names, mouse presses with where
// they were and their buttons, pastes, and the sizes it was laid out in.
struct Heard {
    std::vector<std::string> said;
    std::vector<Size> sizes;
    Hooks hooks;

    // Listens to `one`.
    explicit Heard(Document& one) {
        hooks.add(one.on<KeyEvent>([this](KeyEvent& what) {
            std::string made;

            if (what.key.code() == Code::enter) made = "enter";
            else tal::text::encoded(made, what.key.letter());

            said.push_back("key " + made);
        }));

        hooks.add(one.on<MouseEvent>([this](MouseEvent& what) {
            static const char* const names[] = {"down", "up", "move", "wheel", "click", "double"};

            std::string made = std::string{names[static_cast<int>(what.press)]} + " " + std::to_string(what.screen.x) + "," +
                               std::to_string(what.screen.y);

            if (what.press == Press::wheel)
                made += " " + std::to_string(what.steps) + (what.axis == Axis::horizontal ? " across" : " along");

            static const char* const buttons[] = {"", " left", " middle", " right", " back", " forward"};

            made += buttons[static_cast<int>(what.button)];

            said.push_back(made);
        }));

        hooks.add(one.on<PasteEvent>([this](PasteEvent& what) { said.push_back("paste " + what.text); }));

        hooks.add(one.on<Arranged>([this, &one](Arranged&) { sizes.push_back(one.area().size); }));
    }
};

// Whether `what` throws an exception of type E.
template <typename E, typename F>
bool throws(F what) {
    try {
        what();
    } catch (const E&) {
        return true;
    }

    return false;
}

TRIAL("Scene: show makes the first frame on the scene's screen, read as rows and as text") {
    Scene scene{Size{6, 2}};
    Document document{scene.time()};
    Worded& worded = document.children().add<Worded>();

    worded.text = "ab\xe6\x97\xa5" "c";

    scene.show(document);

    OUGHT(scene.screen().size() == (Size{6, 2}));
    SAME(scene.row(0), std::string{"ab\xe6\x97\xa5" "c "});
    SAME(scene.row(1), std::string{"      "});
    SAME(scene.text(), std::string{"ab\xe6\x97\xa5" "c \n      \n"});
    SAME(scene.at({2, 0}).cluster, std::string{"\xe6\x97\xa5"});
    SAME(scene.at({3, 0}).cluster, std::string{});
    SAME(document.framed(), std::uint64_t{1});
    OUGHT(throws<std::out_of_range>([&scene] { (void)scene.row(2); }));
    OUGHT(throws<std::out_of_range>([&scene] { (void)scene.row(-1); }));
    OUGHT(throws<std::out_of_range>([&scene] { (void)scene.at({6, 0}); }));

    Scene narrow{Size{0, 2}};
    Document empty{narrow.time()};

    narrow.show(empty);

    SAME(narrow.row(1), std::string{});
    OUGHT(throws<std::out_of_range>([&narrow] { (void)narrow.row(-1); }));
    OUGHT(throws<std::out_of_range>([&narrow] { (void)narrow.row(2); }));
}

TRIAL("Scene: before show every step and every reading throws") {
    Scene scene;

    OUGHT(throws<std::logic_error>([&scene] { (void)scene.screen(); }));
    OUGHT(throws<std::logic_error>([&scene] { (void)scene.text(); }));
    OUGHT(throws<std::logic_error>([&scene] { (void)scene.caret(); }));
    OUGHT(throws<std::logic_error>([&scene] { scene.press(Key{U'a'}); }));
    OUGHT(throws<std::logic_error>([&scene] { scene.wait(1ms); }));
    OUGHT(throws<std::logic_error>([&scene] { scene.resize(Size{10, 2}); }));
    OUGHT(throws<std::logic_error>([&scene] { scene.point({0, 0}); }));

    const auto now = scene.clock().now();

    scene.clock().advance(5ms);

    OUGHT(scene.clock().now() - now == 5ms);
}

// The scene hands the document its colors, Unicode and rendering: the look comes at the color
// level asked for, and an emoji sequence takes as many cells as that rendering counts.
TRIAL("Scene: the document gets the scene's colors, Unicode and rendering") {
    Scene scene{Size{8, 2}, Colors::palette, false, tal::text::Rendering{false, false}};
    Document document{scene.time()};

    document.sheet(tal::StyleSheet::parsed(
        R"({"rules": [{"select": "knob", "style": {"256": {"ink": "color196"}, "rgb": {"ink": "#00ff00"}}}], "glyphs": {}})"));

    Node& stack = document.children().add<Corner>();
    Worded& worded = document.children().add<Worded>();
    Styled& styled = stack.children().add<Styled>();

    worded.text = "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb|";

    scene.show(document);

    SAME(scene.at({4, 0}).cluster, std::string{"|"});
    OUGHT(scene.at({3, 1}).look.ink == (std::optional<Color>{Color{255, 0, 0}}));
    SAME(styled.more(), std::string_view{">"});
}

// Every input ends as an application's turn does: a frame when anything changed, none otherwise.
TRIAL("Scene: keys, text and paste reach the document, and the frame follows the change") {
    Scene scene{Size{10, 1}};
    Document document{scene.time()};
    Heard heard{document};
    Worded& worded = document.children().add<Worded>();

    scene.show(document);

    scene.press(Key{Code::enter});
    scene.type("a\xc3\xa9" "e\xcc\x81");
    scene.paste("x y");

    OUGHT(heard.said == (std::vector<std::string>{"key enter", "key a", "key \xc3\xa9", "key e", "key \xcc\x81", "paste x y"}));
    SAME(document.framed(), std::uint64_t{1});

    heard.hooks.add(document.on<KeyEvent>([&worded](KeyEvent& what) { worded.text = *worded.text + "!"; (void)what; }));

    scene.type("zz");

    SAME(scene.row(0), std::string{"!!        "});
    SAME(document.framed(), std::uint64_t{3});
    OUGHT(throws<std::invalid_argument>([&scene] { scene.type("a\xff"); }));
}

// A click is a press and a release in one cell, which the document makes a click; two of them with
// no time between are a double click; the pointer alone hovers; the wheel goes along its axis.
TRIAL("Scene: clicks, a double click, pointing and the wheel reach the document") {
    Scene scene{Size{10, 3}};
    Document document{scene.time()};
    Heard heard{document};
    Worded& worded = document.children().add<Worded>();

    scene.show(document);

    scene.click({1, 2});
    scene.click({4, 0}, MouseButton::right);
    scene.twice({2, 1});
    scene.point({5, 1});
    scene.wheel({3, 2}, -2);
    scene.wheel({3, 2}, 1, Axis::horizontal);

    OUGHT(heard.said == (std::vector<std::string>{"down 1,2 left", "up 1,2 left", "click 1,2 left", "down 4,0 right", "up 4,0 right",
                                                  "click 4,0 right", "down 2,1 left", "up 2,1 left", "click 2,1 left", "down 2,1 left",
                                                  "up 2,1 left", "double 2,1 left", "move 5,1", "wheel 3,2 -2 along", "wheel 3,2 1 across"}));
    OUGHT(worded.hovered());
}

// A click on a node goes to the middle of where it stands on the screen, its parents' places added
// up. A node not on that screen throws: outside the document shown, or without room.
TRIAL("Scene: a click on a node goes to its middle, and a node off the screen throws") {
    Scene scene{Size{10, 4}};
    Document document{scene.time()};
    Heard heard{document};
    Stack& frame = document.children().add<Stack>();

    (void)frame.children().add<Node>();

    Corner& corner = frame.children().add<Corner>();
    Worded& worded = corner.children().add<Worded>();
    Worded& hidden = corner.children().add<Worded>();
    Document other{scene.time()};
    Worded& elsewhere = other.children().add<Worded>();
    Worded loose;

    other.display(Size{10, 4}, Colors::rgb, true);

    (void)other.frame();

    scene.show(document);

    scene.click(worded);

    OUGHT(heard.said == (std::vector<std::string>{"down 5,3 left", "up 5,3 left", "click 5,3 left"}));
    OUGHT(throws<std::logic_error>([&scene, &hidden] { scene.click(hidden); }));
    OUGHT(throws<std::logic_error>([&scene, &elsewhere] { scene.click(elsewhere); }));
    OUGHT(throws<std::logic_error>([&scene, &loose] { scene.click(loose); }));

    scene.resize(Size{5, 4});

    OUGHT(throws<std::logic_error>([&scene, &worded] { scene.click(worded); }));
}

// Waiting moves the scene's clock: the document's timers due by then ring, and what they changed is
// on the screen; one not yet due waits.
TRIAL("Scene: waiting rings the timers that fall due and draws what they changed") {
    Scene scene{Size{6, 1}};
    Document document{scene.time()};
    Worded& worded = document.children().add<Worded>();

    scene.show(document);

    const auto began = scene.clock().now();
    const Timer soon = document.after(30ms, [&worded](Timer&) { worded.text = "soon"; });
    const Timer late = document.after(100ms, [&worded](Timer&) { worded.text = "late"; });

    scene.wait(29ms);

    SAME(scene.row(0), std::string{"      "});

    scene.wait(1ms);

    SAME(scene.row(0), std::string{"soon  "});
    OUGHT(late.waiting());
    OUGHT(scene.clock().now() - began == 30ms);

    scene.wait(70ms);

    SAME(scene.row(0), std::string{"late  "});
}

// A frame that leaves the document due is followed by the next one in the same step, as an
// application follows it at once; a document that never settles throws after sixteen more frames.
TRIAL("Scene: frames follow while the document is due, and one never settling throws") {
    Scene scene{Size{6, 1}};
    Document document{scene.time()};
    Worded& worded = document.children().add<Worded>();
    int heard = 0;
    Hooks hooks;

    hooks.add(document.on<Arranged>([&worded, &heard](Arranged&) {
        if (++heard == 3) return;

        worded.text = std::string(static_cast<std::size_t>(heard), 'x');
        worded.share(tal::Share{heard, 0});
    }));

    scene.show(document);

    SAME(heard, 3);
    SAME(scene.row(0), std::string{"xx    "});
    OUGHT(!document.due());

    Scene other{Size{6, 1}};
    Document restless{other.time()};
    Worded& again = restless.children().add<Worded>();
    Hooks more;

    more.add(restless.on<Arranged>([&again](Arranged&) { again.share(tal::Share{again.share().weight == 1 ? 2 : 1, 0}); }));

    OUGHT(throws<std::logic_error>([&other, &restless] { other.show(restless); }));
    SAME(restless.framed(), std::uint64_t{17});
}

// A new size lays the document out again and the screen takes the new size.
TRIAL("Scene: resizing lays the document out again on a screen of the new size") {
    Scene scene{Size{6, 2}};
    Document document{scene.time()};
    Heard heard{document};

    document.children().add<Worded>().text = "abc";

    scene.show(document);
    scene.resize(Size{4, 3});

    OUGHT(heard.sizes == (std::vector<Size>{{6, 2}, {4, 3}}));
    OUGHT(scene.screen().size() == (Size{4, 3}));
    SAME(scene.text(), std::string{"abc \n    \n    \n"});
}

// The first start of the text, in rows from the top and in each from the left, counted in cells; a
// match starting inside a cell's cluster does not count, and neither does empty text.
TRIAL("Scene: where finds the cell the text starts in") {
    Scene scene{Size{14, 3}};
    Document document{scene.time()};
    Stack& stack = document.children().add<Stack>();
    Worded& top = stack.children().add<Worded>();
    Worded& low = stack.children().add<Worded>();

    top.text = "\xe6\x97\xa5" "e\xcc\x81" " ok \xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb \xf0\x9f\x92\xbb";
    low.text = "zz";

    scene.show(document);

    OUGHT(scene.where("ok") == (Spot{4, 0}));
    OUGHT(scene.where("e") == (Spot{2, 0}));
    OUGHT(scene.where("\xe6\x97\xa5" "e") == (Spot{0, 0}));
    OUGHT(scene.where("\xcc\x81") == std::nullopt);
    OUGHT(scene.where("\xf0\x9f\x92\xbb") == (Spot{10, 0}));
    OUGHT(scene.where("zz") == (Spot{0, 1}));
    OUGHT(scene.where("x") == std::nullopt);
    OUGHT(scene.where("") == std::nullopt);
}

TRIAL("Scene: the caret is the document's, on the screen") {
    Scene scene{Size{10, 2}};
    Document document{scene.time()};
    Worded& worded = document.children().add<Worded>(true);

    worded.text = "abc";

    scene.show(document);

    OUGHT(!scene.caret().has_value());

    worded.focus();
    scene.wait(0ms);

    OUGHT(scene.caret() == (Caret{{3, 0}, Caret::Form::bar, false}));
}

// Showing another document starts over: its first frame is made though nothing in it waits.
TRIAL("Scene: showing another document makes its first frame") {
    Scene scene{Size{4, 1}};
    Document one{scene.time()};
    Document two{scene.time()};

    one.children().add<Worded>().text = "one";
    two.children().add<Worded>().text = "two";

    scene.show(one);
    scene.show(two);

    SAME(scene.row(0), std::string{"two "});

    (void)two.frame();

    scene.show(two);

    SAME(scene.row(0), std::string{"two "});
    SAME(two.framed(), std::uint64_t{3});

    scene.show(one);

    SAME(scene.row(0), std::string{"one "});
}

}
