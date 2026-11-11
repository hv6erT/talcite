#include <chrono>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tal/action.hpp"
#include "tal/controls.hpp"
#include "tal/document.hpp"
#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/key.hpp"
#include "tal/layout.hpp"
#include "tal/node.hpp"
#include "tal/scene.hpp"
#include "tal/style.hpp"
#include "tal/surface.hpp"

#include "tal/testing.hpp"

// The simple widgets on a scene: labels with their lines, cuts and marks, the status line, rules,
// buttons, check boxes and radio groups driven by actions, and the progress bar.

namespace {

using namespace std::chrono_literals;

using tal::Action;
using tal::Altered;
using tal::Axis;
using tal::Caret;
using tal::Choice;
using tal::Code;
using tal::Color;
using tal::Document;
using tal::Edge;
using tal::Elide;
using tal::Gauge;
using tal::Hooks;
using tal::Key;
using tal::KeyEvent;
using tal::Label;
using tal::Limits;
using tal::Look;
using tal::Mark;
using tal::Mods;
using tal::Phase;
using tal::Rule;
using tal::Run;
using tal::Size;
using tal::Spot;
using tal::Status;
using tal::StyleSheet;
using tal::Tick;
using tal::testing::Scene;

// The sheet of these trials: a color for each part they look for.
StyleSheet sheet() {
    return StyleSheet::parsed(R"({"rules": [
        {"select": "label::link", "style": {"ink": "#0000ff"}},
        {"select": "button::key", "style": {"ink": "#ff0000"}},
        {"select": "tick::key", "style": {"ink": "#ff0000"}},
        {"select": "button:disabled", "style": {"faint": true}},
        {"select": "gauge::done", "style": {"ink": "#00ff00"}},
        {"select": "gauge::text", "style": {"ink": "#808080"}},
        {"select": "rule::title", "style": {"bold": true}}
    ], "glyphs": {}})");
}

// A scene of `screen` cells and a document on its clock with the trials' sheet.
struct Stage {
    Scene scene;
    Document document{scene.time()};

    // Makes the scene and gives the document the sheet.
    explicit Stage(Size screen) : scene(screen) { document.sheet(sheet()); }

    // Shows the document.
    void shown() { scene.show(document); }
};

// The ink of the cell at `where`.
std::optional<Color> ink(const Scene& scene, Spot where) { return scene.at(where).look.ink; }

TRIAL("Label: one row per line of the text, as wide as the widest") {
    Stage stage{Size{6, 3}};
    Label& label = stage.document.children().add<Label>("ab\ncde\n");

    stage.shown();

    SAME(stage.scene.text(), std::string{"ab    \ncde   \n      \n"});
    OUGHT(label.measured(Limits{{0, 0}, {6, 3}}) == (Size{3, 3}));
    OUGHT(label.has_class("label"));
}

// Wrapping makes as many lines of each line of the text as its width needs, and measures against
// the room; an empty line stays.
TRIAL("Label: wrapping to the width") {
    Stage stage{Size{7, 4}};
    Label& label = stage.document.children().add<Label>("one two three\n\nx");

    label.wrap = true;

    stage.shown();

    SAME(stage.scene.text(), std::string{"one two\nthree  \n       \nx      \n"});
    OUGHT(label.measured(Limits{{0, 0}, {7, 10}}) == (Size{7, 4}));
    OUGHT(label.measured(Limits{{0, 0}, {20, 10}}) == (Size{13, 3}));

    // The space a line was broken after shows nothing, so it neither widens nor cuts the line.
    label.elide = Elide::end;
    label.justify = Edge::end;
    stage.scene.wait(0ms);

    SAME(stage.scene.row(0), std::string{"one two"});
    SAME(stage.scene.row(1), std::string{"  three"});
}

TRIAL("Label: lines stand at the start, in the middle or at the end") {
    Stage stage{Size{8, 2}};
    Label& label = stage.document.children().add<Label>("ab\nabc");

    stage.shown();

    SAME(stage.scene.row(0), std::string{"ab      "});

    label.justify = Edge::middle;
    stage.scene.wait(0ms);

    SAME(stage.scene.row(0), std::string{"   ab   "});
    SAME(stage.scene.row(1), std::string{"  abc   "});

    label.justify = Edge::end;
    stage.scene.wait(0ms);

    SAME(stage.scene.row(0), std::string{"      ab"});
}

// A line wider than the label is cut where `elide` says, the sheet's glyph standing in the gap;
// without elision only its end falls off.
TRIAL("Label: a long line cut at its end, its start or its middle") {
    Stage stage{Size{6, 1}};
    Label& label = stage.document.children().add<Label>("abcdefghij");

    stage.shown();

    SAME(stage.scene.row(0), std::string{"abcdef"});

    label.elide = Elide::end;
    stage.scene.wait(0ms);

    SAME(stage.scene.row(0), std::string{"abcde\xe2\x80\xa6"});

    label.elide = Elide::start;
    stage.scene.wait(0ms);

    SAME(stage.scene.row(0), std::string{"\xe2\x80\xa6" "fghij"});

    label.elide = Elide::middle;
    stage.scene.wait(0ms);

    SAME(stage.scene.row(0), std::string{"abc\xe2\x80\xa6" "ij"});

    label.text = "abc\xe6\x97\xa5\xe6\x97\xa5";
    stage.scene.wait(0ms);

    SAME(stage.scene.row(0), std::string{"abc\xe2\x80\xa6\xe6\x97\xa5"});

    label.text = "abcdef";
    label.justify = Edge::end;
    stage.scene.wait(0ms);

    SAME(stage.scene.row(0), std::string{"abcdef"});
}

// What lies under a mark is in the look of its part, and a mark with an address is a link; marks
// cut by elision keep their look on what is left, on both sides of the glyph, and nothing written
// past a piece of the text covers the glyph.
TRIAL("Label: marks give parts of the text their look and their links") {
    Stage stage{Size{9, 1}};
    Label& label = stage.document.children().add<Label>("go to x");

    label.marks = std::vector<Mark>{Mark{3, 5, "link", "https://example.org/"}, Mark{6, 7, "", ""}};

    stage.shown();

    const Scene& scene = stage.scene;

    OUGHT(ink(scene, {3, 0}) == (std::optional<Color>{Color{0, 0, 255}}));
    OUGHT(ink(scene, {4, 0}) == (std::optional<Color>{Color{0, 0, 255}}));
    OUGHT(!ink(scene, {2, 0}).has_value());
    OUGHT(!ink(scene, {6, 0}).has_value());
    SAME(scene.screen().address(scene.at({3, 0}).link), std::string_view{"https://example.org/"});
    SAME(scene.at({5, 0}).link, std::uint32_t{0});
    SAME(scene.at({6, 0}).link, std::uint32_t{0});

    label.text = "go to xxxxxxxx";
    label.elide = Elide::end;
    stage.scene.wait(0ms);

    OUGHT(ink(scene, {4, 0}) == (std::optional<Color>{Color{0, 0, 255}}));
    SAME(scene.row(0), std::string{"go to xx\xe2\x80\xa6"});

    label.text = "abcdefghijklm";
    label.elide = Elide::middle;
    label.marks = std::vector<Mark>{Mark{1, 11, "link", {}}};
    stage.scene.wait(0ms);

    SAME(scene.row(0), std::string{"abcd\xe2\x80\xa6" "jklm"});
    OUGHT(ink(scene, {1, 0}) == (std::optional<Color>{Color{0, 0, 255}}));
    OUGHT(ink(scene, {5, 0}) == (std::optional<Color>{Color{0, 0, 255}}));
    OUGHT(!ink(scene, {7, 0}).has_value());
    SAME(scene.at({1, 0}).link, std::uint32_t{0});
    SAME(scene.at({3, 0}).link, std::uint32_t{0});
}

// A status line wants one row over its whole width, and its line is cut at the end.
TRIAL("Status: one row, cut at its end") {
    Stage stage{Size{5, 2}};
    Status& status = stage.document.children().add<tal::Column>().children().add<Status>("abcdefgh\nsecond");

    stage.shown();

    SAME(stage.scene.text(), std::string{"abcd\xe2\x80\xa6\n     \n"});
    OUGHT(status.measured(Limits{{0, 0}, {5, 2}}) == (Size{5, 1}));
    OUGHT(status.has_class("status"));
}

// A rule draws its line with the glyphs of its kind and its title in the middle, in the look of the
// part "title"; a vertical one goes down its column.
TRIAL("Rule: the line in the sheet's glyphs, the title in the middle") {
    Stage stage{Size{12, 1}};
    Rule& rule = stage.document.children().add<Rule>(Axis::horizontal, "ab");

    stage.shown();

    SAME(stage.scene.row(0), std::string{"\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80 ab \xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"});
    OUGHT(stage.scene.at({5, 0}).look.bold);
    OUGHT(!stage.scene.at({0, 0}).look.bold);
    OUGHT(rule.measured(Limits{{0, 0}, {12, 1}}) == (Size{6, 1}));

    rule.kind = tal::Border::twin;
    rule.title = "";
    stage.scene.wait(0ms);

    SAME(stage.scene.at({0, 0}).cluster, std::string{"\xe2\x95\x90"});
    SAME(stage.scene.at({5, 0}).cluster, std::string{"\xe2\x95\x90"});
    OUGHT(rule.measured(Limits{{0, 0}, {12, 1}}) == (Size{1, 1}));

    Stage down{Size{3, 2}};
    Rule& upright = down.document.children().add<Rule>(Axis::vertical);

    down.shown();

    SAME(down.scene.text(), std::string{"\xe2\x94\x82  \n\xe2\x94\x82  \n"});
    OUGHT(upright.measured(Limits{{0, 0}, {3, 2}}) == (Size{1, 1}));
}

// A button shows its action's text in brackets with the letter in the part "key", and runs the
// action on Enter, the space bar or a click; other keys, keys with modifiers and other buttons do
// nothing.
TRIAL("Button: its action's text and letter, run by Enter, space and a click") {
    Stage stage{Size{14, 2}};
    auto open = std::make_shared<Action>("Open", U'o');
    int ran = 0;
    Hooks hooks;

    hooks.add(open->on<Run>([&ran](Run&) { ++ran; }));

    tal::Button& button = stage.document.children().add<tal::Button>(open);

    stage.shown();

    SAME(stage.scene.row(0), std::string{"[ Open ]      "});
    OUGHT(ink(stage.scene, {2, 0}) == (std::optional<Color>{Color{255, 0, 0}}));
    OUGHT(!ink(stage.scene, {3, 0}).has_value());

    button.focus();
    stage.scene.press(Key{Code::enter});
    stage.scene.press(Key{U' '});
    stage.scene.press(Key{U'x'});
    stage.scene.press(Key{Code::enter, Mods{true, false, false}});
    stage.scene.click({1, 0});
    stage.scene.click({1, 0}, tal::MouseButton::right);

    SAME(ran, 3);

    button.primary = true;
    stage.scene.wait(0ms);

    SAME(stage.scene.row(0), std::string{"[< Open >]    "});
    OUGHT(button.measured(Limits{{0, 0}, {14, 1}}) == (Size{10, 1}));
}

// The program hears keys first: one taken before the button's default action does not run it.
TRIAL("Button: a key the program takes does not reach the default action") {
    Stage stage{Size{10, 1}};
    auto open = std::make_shared<Action>("Go");
    int ran = 0;
    Hooks hooks;

    hooks.add(open->on<Run>([&ran](Run&) { ++ran; }));

    tal::Button& button = stage.document.children().add<tal::Button>(open);

    hooks.add(button.on<KeyEvent>([](KeyEvent& what) { what.take(); }, Phase::capture));

    stage.shown();
    button.focus();
    stage.scene.press(Key{Code::enter});

    SAME(ran, 0);
}

// The button follows its action: a new text lays it out again, a disabled action disables it (it
// loses the focus and looks disabled), and a button gone hears its action no more.
TRIAL("Button: it follows its action's text and availability") {
    Stage stage{Size{14, 1}};
    auto open = std::make_shared<Action>("Open", U'o');
    tal::Row& row = stage.document.children().add<tal::Row>();
    auto& button = row.children().add<tal::Button>(open);

    (void)row.children().add<Label>("x");

    stage.shown();
    button.focus();

    SAME(stage.scene.row(0), std::string{"[ Open ]x     "});

    open->text("Opening");
    stage.scene.wait(0ms);

    SAME(stage.scene.row(0), std::string{"[ Opening ]x  "});
    OUGHT(button.measured(Limits{{0, 0}, {14, 1}}) == (Size{11, 1}));

    open->enable(false);
    stage.scene.wait(0ms);

    OUGHT(!button.enabled());
    OUGHT(!button.focused());
    OUGHT(stage.scene.at({0, 0}).look.faint);

    open->enable(true);

    OUGHT(button.enabled());

    {
        const std::shared_ptr<tal::Node> gone = row.children().remove(button);
    }

    open->text("Shut");

    SAME(open->text(), std::string{"Shut"});
}

// A check box turns over on Enter, the space bar or a click, and then runs its action, which sees
// the new state; it follows the action like a button.
TRIAL("Tick: turned over by Enter, space and a click, then the action runs") {
    Stage stage{Size{12, 1}};
    auto hidden = std::make_shared<Action>("Hidden", U'h');
    std::vector<bool> seen;
    Hooks hooks;

    Tick& tick = stage.document.children().add<Tick>(hidden);

    hooks.add(hidden->on<Run>([&seen, &tick](Run&) { seen.push_back(*tick.ticked); }));

    stage.shown();

    SAME(stage.scene.row(0), std::string{"[ ] Hidden  "});
    OUGHT(ink(stage.scene, {4, 0}) == (std::optional<Color>{Color{255, 0, 0}}));

    tick.focus();
    stage.scene.press(Key{U' '});

    SAME(stage.scene.row(0), std::string{"[x] Hidden  "});

    stage.scene.click({0, 0});
    stage.scene.press(Key{Code::enter});
    stage.scene.press(Key{U'h', Mods{false, true, false}});

    OUGHT(seen == (std::vector<bool>{true, false, true}));

    Stage twice{Size{12, 1}};

    (void)twice.document.children().add<Tick>(std::make_shared<Action>("Hidden", U'd'));

    twice.shown();

    OUGHT(ink(twice.scene, {6, 0}) == (std::optional<Color>{Color{255, 0, 0}}));
    OUGHT(!ink(twice.scene, {7, 0}).has_value());
    OUGHT(tick.measured(Limits{{0, 0}, {12, 1}}) == (Size{10, 1}));

    hidden->enable(false);

    OUGHT(!tick.enabled());

    Stage other{Size{12, 1}};
    Tick& on = other.document.children().add<Tick>(std::make_shared<Action>("On"), true);

    other.shown();

    OUGHT(*on.ticked);
    SAME(other.scene.row(0), std::string{"[x] On      "});
}

// A radio group chooses at once with the arrows, Home and End, and with a click on an option; the
// caret stands in the chosen bracket. Keys with modifiers do nothing.
TRIAL("Choice: arrows, Home, End and a click choose at once") {
    Stage stage{Size{10, 3}};
    Choice& choice = stage.document.children().add<Choice>(std::vector<std::string>{"one", "two", "three"}, 7);

    stage.shown();

    SAME(*choice.chosen, std::size_t{2});
    SAME(stage.scene.text(), std::string{"( ) one   \n( ) two   \n(\xe2\x80\xa2) three \n"});
    OUGHT(choice.measured(Limits{{0, 0}, {10, 3}}) == (Size{9, 3}));

    choice.focus();
    stage.scene.wait(0ms);

    OUGHT(stage.scene.caret() == (Caret{{1, 2}}));

    stage.scene.press(Key{Code::up});
    SAME(*choice.chosen, std::size_t{1});
    stage.scene.press(Key{Code::up});
    stage.scene.press(Key{Code::up});
    SAME(*choice.chosen, std::size_t{0});
    stage.scene.press(Key{Code::end});
    SAME(*choice.chosen, std::size_t{2});
    stage.scene.press(Key{Code::down});
    SAME(*choice.chosen, std::size_t{2});
    stage.scene.press(Key{Code::home});
    SAME(*choice.chosen, std::size_t{0});
    stage.scene.press(Key{Code::down});
    SAME(*choice.chosen, std::size_t{1});
    stage.scene.press(Key{Code::down, Mods{true, false, false}});
    SAME(*choice.chosen, std::size_t{1});

    stage.scene.click({5, 2});

    SAME(*choice.chosen, std::size_t{2});
    SAME(stage.scene.row(2), std::string{"(\xe2\x80\xa2) three "});

    Stage tall{Size{10, 5}};
    Choice& short_one = tall.document.children().add<Choice>(std::vector<std::string>{"a", "b"});

    tall.shown();
    tall.scene.click({1, 4});

    SAME(*short_one.chosen, std::size_t{0});
}

// A choice whose chosen option lies past the end shows no dot and has no caret; arrows bring it
// back. A group without options takes no focus.
TRIAL("Choice: a choice past the end, and an empty group") {
    Stage stage{Size{10, 12}};
    Choice& choice = stage.document.children().add<Choice>(std::vector<std::string>{"a", "b"});

    stage.shown();

    choice.chosen = 9;
    choice.focus();
    stage.scene.wait(0ms);

    SAME(stage.scene.row(0), std::string{"( ) a     "});
    SAME(stage.scene.row(1), std::string{"( ) b     "});
    OUGHT(!stage.scene.caret().has_value());

    stage.scene.press(Key{Code::up});

    SAME(*choice.chosen, std::size_t{0});

    Stage empty{Size{4, 1}};
    Choice& none = empty.document.children().add<Choice>(std::vector<std::string>{}, 3);

    empty.shown();
    none.focus();

    OUGHT(!none.focused());
    SAME(*none.chosen, std::size_t{0});
}

// The bar shows its fraction, rounded, after the percentage; the fraction is cut to [0, 1] and NaN
// counts as zero. Without the percentage the bar starts at once.
TRIAL("Gauge: the percentage and the bar, the fraction cut to its range") {
    Stage stage{Size{20, 1}};
    Gauge& gauge = stage.document.children().add<Gauge>();

    gauge.done = 0.5;

    stage.shown();

    SAME(stage.scene.row(0), std::string{" 50% [\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88"
                                         "\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91]"});
    OUGHT(ink(stage.scene, {6, 0}) == (std::optional<Color>{Color{0, 255, 0}}));
    OUGHT(!ink(stage.scene, {13, 0}).has_value());
    OUGHT(ink(stage.scene, {1, 0}) == (std::optional<Color>{Color{128, 128, 128}}));
    OUGHT(gauge.measured(Limits{{0, 0}, {20, 1}}) == (Size{15, 1}));

    gauge.done = std::numeric_limits<double>::quiet_NaN();
    stage.scene.wait(0ms);

    SAME(stage.scene.row(0).substr(0, 6), std::string{"  0% ["});

    gauge.done = 7.0;
    stage.scene.wait(0ms);

    SAME(stage.scene.row(0).substr(0, 6), std::string{"100% ["});
    SAME(stage.scene.at({18, 0}).cluster, std::string{"\xe2\x96\x88"});

    gauge.percent = false;
    gauge.done = -1.0;
    stage.scene.wait(0ms);

    SAME(stage.scene.at({0, 0}).cluster, std::string{"["});
    SAME(stage.scene.at({19, 0}).cluster, std::string{"]"});
    SAME(stage.scene.at({1, 0}).cluster, std::string{"\xe2\x96\x91"});
    OUGHT(gauge.measured(Limits{{0, 0}, {20, 1}}) == (Size{10, 1}));
}

// A busy gauge runs a block to and fro on the document's clock, one cell a tenth of a second, and
// hides its number; it stops when no longer busy. A gauge made busy before it entered a document
// starts once it is laid out there.
TRIAL("Gauge: a busy block runs on the clock and stops") {
    Stage stage{Size{20, 1}};
    auto made = std::make_shared<Gauge>();

    made->busy = true;

    Gauge& gauge = static_cast<Gauge&>(stage.document.children().add(made));

    stage.shown();

    // Where the busy block's first cell stands.
    const auto block = [&stage] {
        for (int x = 6; x < 19; ++x)
            if (stage.scene.at({x, 0}).cluster == "\xe2\x96\x88") return x - 6;

        return -1;
    };

    SAME(stage.scene.row(0).substr(0, 5), std::string{"     "});
    SAME(block(), 0);
    SAME(stage.scene.at({7, 0}).cluster, std::string{"\xe2\x96\x88"});
    SAME(stage.scene.at({8, 0}).cluster, std::string{"\xe2\x96\x91"});

    stage.scene.wait(99ms);
    SAME(block(), 0);
    stage.scene.wait(1ms);
    SAME(block(), 1);

    for (int at = 1; at < 12; ++at) stage.scene.wait(100ms);

    SAME(block(), 10);

    gauge.busy = false;
    gauge.done = 0.0;
    stage.scene.wait(500ms);

    SAME(block(), -1);
    SAME(stage.scene.row(0).substr(0, 5), std::string{"  0% "});

    // Busy again where it stopped, as soon as it is told: the block stood still meanwhile.
    gauge.busy = true;
    stage.scene.wait(0ms);

    SAME(block(), 10);

    stage.scene.wait(50ms);

    // A new layout in the middle of a beat does not start the beat over.
    gauge.percent = false;
    stage.scene.wait(0ms);
    gauge.percent = true;
    stage.scene.wait(50ms);

    SAME(block(), 9);
}

}
