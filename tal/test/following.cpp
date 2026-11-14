#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tal/controls.hpp"
#include "tal/document.hpp"
#include "tal/editing.hpp"
#include "tal/event.hpp"
#include "tal/executor.hpp"
#include "tal/layout.hpp"
#include "tal/model.hpp"
#include "tal/pages.hpp"
#include "tal/process.hpp"
#include "tal/scene.hpp"
#include "tal/style.hpp"
#include "tal/task.hpp"
#include "tal/trouble.hpp"
#include "tal/views.hpp"
#include "tal/window.hpp"

#include "quieting.hpp"
#include "tal/testing.hpp"

// Following addresses: a click on a mark with an address in a label, a list, a table and a tree,
// Enter and a click in a page, Ctrl with a click in a terminal tell `Followed`; when nobody takes
// it the document opens the address through its port of processes, and a failure (no loop running
// among them) tells `Unfollowed`. A new sheet on a live document restyles every window; the tree is
// described as text, and in a debug build Ctrl+F12 shows it in a window.

namespace {

using namespace std::chrono_literals;

using tal::Code;
using tal::Command;
using tal::Document;
using tal::Entry;
using tal::Followed;
using tal::Hooks;
using tal::Key;
using tal::Mark;
using tal::Mods;
using tal::MouseButton;
using tal::Press;
using tal::Process;
using tal::Size;
using tal::Spot;
using tal::Task;
using tal::Unfollowed;
using tal::testing::Scene;

// A port of processes that refuses every start.
class Refusing : public tal::Processes {
public:
    // The adapter's name.
    [[nodiscard]] std::string_view name() const override { return "refusing"; }

    // Refuses.
    Task<Process> start(Command) override { throw tal::Trouble{tal::Kind::denied, "Opening is not allowed."}; }
};

// Rows of one column, each with an address over its first three bytes.
class Rows : public tal::Model {
public:
    // How many rows: three.
    [[nodiscard]] std::size_t rows() const override { return 3; }

    // The row `row`: its text, and the address over its start.
    [[nodiscard]] Entry entry(std::size_t row, std::size_t) const override {
        return Entry{"row" + std::to_string(row) + " text", {}, {Mark{0, 4, {}, "https://x.pl/" + std::to_string(row)}}};
    }
};

// The same rows, as many as `left` says.
class Shrinking : public Rows {
public:
    std::size_t left = 3;

    // How many rows: `left`.
    [[nodiscard]] std::size_t rows() const override { return left; }
};

// The same rows as leaves of a tree.
class Leaves : public tal::TreeModel {
public:
    // How many rows: three.
    [[nodiscard]] std::size_t rows() const override { return 3; }

    // The row `row`, as `Rows` gives it.
    [[nodiscard]] Entry entry(std::size_t row, std::size_t column) const override { return Rows{}.entry(row, column); }

    // Every row stands at the top.
    [[nodiscard]] int depth(std::size_t) const override { return 0; }

    // Every row is a leaf.
    [[nodiscard]] tal::Branch branch(std::size_t) const override { return tal::Branch::leaf; }

    // A leaf neither opens nor closes.
    void open(std::size_t, bool) override {}
};

// A list whose click on a row empties its model first, as a program may do when it hears it.
class Emptying : public tal::List {
public:
    // A list of `rows`, which it empties on a click.
    explicit Emptying(std::shared_ptr<Shrinking> rows) : tal::List(rows), rows_(std::move(rows)) {}

protected:
    // Empties the rows, then makes the row current as a list does.
    void clicked(std::size_t row, int x) override {
        rows_->left = 0;
        tal::List::clicked(row, x);
    }

private:
    std::shared_ptr<Shrinking> rows_;
};

// A column that counts the clicks nobody below took.
class Catcher : public tal::Column {
public:
    int caught = 0;

    // Hears the clicks last.
    Catcher() {
        hooks_.add(otherwise<tal::MouseEvent>([this](tal::MouseEvent& what) {
            if (what.press == Press::click) ++caught;
        }));
    }

private:
    Hooks hooks_;
};

// A document on a scene and a loop, its processes scripted (or refusing), with what was followed
// and what failed.
struct Shown {
    Scene scene;
    tal::Loop loop{scene.time()};
    std::shared_ptr<tal::Scripted> scripted = std::make_shared<tal::Scripted>(loop);
    Document document;
    std::vector<std::string> followed;
    std::vector<std::string> failed;
    std::vector<tal::Kind> kinds;
    bool taking = false;
    Hooks hooks;

    // A document of `size` whose addresses open through the scripted port, or through a refusing
    // one.
    explicit Shown(Size size = {30, 6}, bool refusing = false)
        : scene(size),
          document(scene.time(), refusing ? tal::Port<tal::Processes>{std::make_shared<Refusing>()} : tal::Port<tal::Processes>{scripted}) {
        hooks.add(document.on<Unfollowed>([this](Unfollowed& one) {
            failed.push_back(one.address + ": " + one.why.what());
            kinds.push_back(one.why.kind());
        }));
    }

    // Hears `Followed` from `one`, and takes it while `taking`.
    void listen(tal::Node& one) {
        hooks.add(one.on<Followed>([this](Followed& told) {
            followed.push_back(told.address);

            if (taking) told.take();
        }));
    }

    // Shows the document and lets everything settle.
    void show() {
        scene.show(document);
        settled();
    }

    // Lets the loop and the frames go until both are quiet.
    void settled() {
        for (int turn = 0; turn < 3; ++turn) {
            QUIETED(loop);
            scene.wait(0ms);
        }
    }

    // The addresses opened through the port so far.
    std::vector<std::string> opened() const {
        std::vector<std::string> made;

        for (const Command& one : scripted->started())
            if (one.mode == tal::Mode::opened) made.push_back(one.arguments.at(0));

        return made;
    }
};

}

TRIAL("Following: a click on an address in a label tells Followed, and the document opens what nobody took") {
    {
        Shown shown;
        Catcher& catcher = shown.document.children().add<Catcher>();
        tal::Label& label = catcher.children().add<tal::Label>("see https://x.pl here");

        label.share(tal::Share{1, 0});
        label.marks = std::vector<Mark>{Mark{4, 16, {}, "https://x.pl"}};
        shown.listen(label);
        shown.show();
        shown.scene.click({1, 0});
        shown.settled();

        OUGHT(shown.followed.empty());
        SAME(catcher.caught, 1);

        shown.scene.click({6, 0});
        shown.settled();

        SAME(catcher.caught, 1);
        OUGHT(shown.followed == (std::vector<std::string>{"https://x.pl"}));
        OUGHT(shown.opened() == (std::vector<std::string>{"https://x.pl"}));
        OUGHT(shown.failed.empty());

        shown.scene.click({6, 3});
        shown.settled();

        SAME(shown.followed.size(), std::size_t{1});

        shown.taking = true;
        shown.scene.wait(1s);
        shown.scene.click({6, 0});
        shown.settled();

        SAME(shown.followed.size(), std::size_t{2});
        SAME(shown.opened().size(), std::size_t{1});

        shown.taking = false;
        shown.scene.wait(1s);
        shown.document.mouse(Press::down, MouseButton::left, {6, 0}, Mods{.ctrl = true});
        shown.document.mouse(Press::up, MouseButton::left, {6, 0}, Mods{.ctrl = true});
        shown.settled();

        SAME(shown.followed.size(), std::size_t{2});

        shown.document.follow("");
        shown.settled();

        SAME(shown.opened().size(), std::size_t{1});
    }

    {
        Shown shown;
        tal::Label& label = shown.document.children().add<tal::Label>("see https://x.pl here");

        label.marks = std::vector<Mark>{Mark{4, 16, {}, "https://x.pl"}};
        shown.listen(label);
        shown.document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "label", "style": {"padding": [1, 0, 0, 2]}}], "glyphs": {}})"));
        shown.show();
        shown.scene.click({4, 1});
        shown.scene.click({17, 0});
        shown.settled();

        OUGHT(shown.followed.empty());

        shown.scene.click({17, 1});
        shown.settled();

        OUGHT(shown.followed == (std::vector<std::string>{"https://x.pl"}));
    }

    {
        Scene scene{{30, 2}};
        Document alone{scene.time(), tal::Port<tal::Processes>{std::make_shared<Refusing>()}};
        tal::Label& label = alone.children().add<tal::Label>("https://x.pl");
        std::vector<std::string> failed;
        Hooks hooks;

        label.marks = std::vector<Mark>{Mark{0, 12, {}, "https://x.pl"}};
        hooks.add(alone.on<Unfollowed>([&failed](Unfollowed& one) {
            OUGHT(one.why.kind() == tal::Kind::missing);
            failed.push_back(one.address + ": " + one.why.what());
        }));
        scene.show(alone);
        scene.click({2, 0});

        OUGHT(failed == (std::vector<std::string>{"https://x.pl: No loop is running."}));
    }

    Shown refused{{30, 6}, true};
    tal::Label& label = refused.document.children().add<tal::Label>("https://x.pl");

    label.marks = std::vector<Mark>{Mark{0, 12, {}, "https://x.pl"}};
    refused.show();
    refused.scene.click({2, 0});
    refused.settled();

    OUGHT(refused.failed == (std::vector<std::string>{"https://x.pl: Opening is not allowed."}));
    OUGHT(refused.kinds == (std::vector<tal::Kind>{tal::Kind::denied}));
}

TRIAL("Following: a click on an address in a list, a table and a tree") {
    {
        Shown shown;
        tal::List& list = shown.document.children().add<tal::List>(std::make_shared<Rows>());

        shown.listen(list);
        shown.show();
        shown.scene.click({1, 1});
        shown.settled();

        SAME(*list.current, std::size_t{1});
        OUGHT(shown.followed == (std::vector<std::string>{"https://x.pl/1"}));
        OUGHT(shown.opened() == (std::vector<std::string>{"https://x.pl/1"}));

        shown.scene.wait(1s);
        shown.scene.click({6, 2});
        shown.settled();

        SAME(*list.current, std::size_t{2});
        SAME(shown.opened().size(), std::size_t{1});
    }

    {
        Shown shown;
        tal::List& list = shown.document.children().add<tal::List>(std::make_shared<Rows>());

        shown.listen(list);
        shown.document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "list", "style": {"padding": [0, 0, 0, 2]}}], "glyphs": {}})"));
        shown.show();
        shown.scene.click({1, 0});
        shown.scene.click({5, 1});
        shown.settled();

        OUGHT(shown.followed == (std::vector<std::string>{"https://x.pl/1"}));
    }

    {
        Shown shown;
        tal::List& list = shown.document.children().add<Emptying>(std::make_shared<Shrinking>());

        shown.listen(list);
        shown.show();
        shown.scene.click({1, 1});
        shown.settled();

        OUGHT(shown.followed.empty());
    }

    {
        Shown shown;

        (void)shown.document.children().add<tal::Table>(std::make_shared<Rows>(), std::vector<tal::Heading>{tal::Heading{"Name", tal::Share{1, 0}}});
        shown.show();
        shown.scene.click({1, 3});
        shown.settled();

        OUGHT(shown.opened() == (std::vector<std::string>{"https://x.pl/2"}));
    }

    Shown shown;

    (void)shown.document.children().add<tal::Tree>(std::make_shared<Leaves>());
    shown.show();
    shown.scene.click({3, 0});
    shown.settled();

    SAME(shown.opened().size(), std::size_t{1});
    OUGHT(shown.opened().at(0) == "https://x.pl/0");
}

TRIAL("Following: Enter or a click on an address in a page opens it") {
    Shown shown;
    tal::Pager& pager = shown.document.children().add<tal::Pager>(std::make_shared<tal::Text>("one\nhttps://x.pl two\n"));

    pager.marks({tal::Stretch{tal::Place{1, 0}, tal::Place{1, 12}, "link", "https://x.pl"}});
    shown.show();
    pager.focus();
    shown.scene.click({3, 1});
    shown.settled();

    OUGHT(shown.opened() == (std::vector<std::string>{"https://x.pl"}));
}

TRIAL("Following: a new sheet on a live document restyles every window, its frames, padding and colors") {
    Shown shown{{30, 10}};
    tal::Layers& layers = shown.document.children().add<tal::Layers>();
    auto lower = std::make_shared<tal::Window>("dolne");
    auto upper = std::make_shared<tal::Window>("górne");

    lower->children().add<tal::Label>("pod spodem");
    upper->children().add<tal::Label>("na wierzchu");
    lower->seat(tal::Area{{0, 0}, {20, 6}});
    upper->seat(tal::Area{{10, 4}, {20, 6}});
    (void)layers.open(lower);
    (void)layers.open(upper);
    shown.show();

    const std::optional<Spot> before = shown.scene.where("pod spodem");

    OUGHT(before.has_value());

    shown.document.sheet(tal::StyleSheet::parsed(R"({"rules": [
        {"select": "window", "style": {"border": "twin", "padding": [0, 2, 0, 2], "paper": "#000080"}},
        {"select": "label", "style": {"ink": "#ffff00"}}], "glyphs": {}})"));
    shown.settled();

    const std::optional<Spot> after = shown.scene.where("pod spodem");

    OUGHT(after.has_value() && after->x == before->x + 2);
    OUGHT(shown.scene.at(*after).look.ink == (tal::Color{255, 255, 0}));
    SAME(shown.scene.at({0, 0}).cluster, std::string{"╔"});
    OUGHT(shown.scene.at({0, 1}).look.paper == (tal::Color{0, 0, 128}));
    OUGHT(shown.scene.where("na wierzchu").has_value());
    SAME(shown.scene.at({10, 4}).cluster, std::string{"╔"});
}

TRIAL("Following: a node describes its tree, and in a debug build Ctrl+F12 shows it") {
    Shown shown{{60, 12}};
    tal::Column& column = shown.document.children().add<tal::Column>();
    tal::Label& label = column.children().add<tal::Label>("x");
    tal::Pager& reader = column.children().add<tal::Pager>(std::make_shared<tal::Text>("y\n"));
    std::vector<tal::Code> fell;

    column.id("box");
    label.id("name");
    label.add_class("big");
    reader.id("reader");
    reader.share(tal::Share{1, 0});
    shown.hooks.add(shown.document.on<tal::KeyEvent>([&fell](tal::KeyEvent& one) { fell.push_back(one.key.code()); }));
    shown.document.sheet(tal::StyleSheet::parsed(R"({"rules": [
        {"select": "label.big", "style": {"ink": "#ffff00", "paper": "#000080", "bold": true}}], "glyphs": {}})"));
    shown.show();
    reader.focus();
    shown.settled();

    const std::string said = column.described();

    OUGHT(said.starts_with("box 0,0 60x"));
    OUGHT(said.find("\n  name.label.big 0,0 ") != std::string::npos);
    OUGHT(said.find(" ink #ffff00 paper #000080 bold border none padding 0 0 0 0\n") != std::string::npos);
    OUGHT(said.find(" ink default paper default border none padding 0 0 0 0\n") != std::string::npos);
    OUGHT(said.find("\n  reader.page.pager [focus] ") != std::string::npos);
    OUGHT(shown.document.described().starts_with("- 0,0 60x12 "));

    shown.scene.press(Key{Code::f12, Mods{.ctrl = true}});
    shown.settled();

    OUGHT(shown.document.find<tal::Window>("inspector") == nullptr);

    tal::Layers& lower = shown.document.children().add<tal::Layers>();
    tal::Layers& upper = shown.document.children().add<tal::Layers>();

    shown.settled();
    reader.focus();
    shown.scene.press(Key{Code::f12});
    shown.scene.press(Key{Code::f12, Mods{.ctrl = true, .shift = true}});
    shown.scene.press(Key{Code::f12, Mods{.ctrl = true, .alt = true}});
    shown.settled();

    OUGHT(shown.document.find<tal::Window>("inspector") == nullptr);

    fell.clear();
    shown.scene.press(Key{Code::f12, Mods{.ctrl = true}});
    shown.settled();

#ifndef NDEBUG
    const tal::Window* inspector = shown.document.find<tal::Window>("inspector");
    const std::optional<Spot> focused = shown.scene.where("reader.page.pager [focus]");
    const std::optional<Spot> whole = shown.scene.where("box 0,0");

    OUGHT(inspector != nullptr && inspector->parent() == &upper);
    OUGHT(fell.empty());
    OUGHT(focused.has_value() && whole.has_value() && focused->y < whole->y);
    OUGHT(shown.scene.where("name.label.big").has_value());

    shown.scene.press(Key{Code::escape});
    shown.settled();

    OUGHT(shown.document.find<tal::Window>("inspector") == nullptr);
#else
    OUGHT(shown.document.find<tal::Window>("inspector") == nullptr);
    OUGHT(fell == (std::vector<tal::Code>{Code::f12}));
#endif

    (void)lower;
    (void)upper;
}
