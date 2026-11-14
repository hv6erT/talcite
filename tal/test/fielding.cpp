#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tal/controls.hpp"
#include "tal/document.hpp"
#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/key.hpp"
#include "tal/layout.hpp"
#include "tal/node.hpp"
#include "tal/scene.hpp"
#include "tal/style.hpp"
#include "tal/window.hpp"

#include "tal/testing.hpp"

// Fields on a scene: one line of text typed, moved through, erased, undone; letters refused,
// guesses taken, a hidden text, a text longer than the field; and a field with a history walked and
// opened.

namespace {

using namespace std::chrono_literals;

using tal::Caret;
using tal::Code;
using tal::Document;
using tal::Edge;
using tal::Field;
using tal::Guessing;
using tal::HistoryField;
using tal::Hooks;
using tal::Key;
using tal::Mods;
using tal::Size;
using tal::Spot;
using tal::Typing;
using tal::testing::Scene;

constexpr Mods ctrl{true, false, false};

// A column that hears the keys nothing under it took, as the nodes above a field.
class Catcher : public tal::Column {
public:
    Catcher() {
        hooks_.add(otherwise<tal::KeyEvent>([this](tal::KeyEvent& what) { heard.push_back(tal::to_string(what.key)); }));
    }

    std::vector<std::string> heard;

private:
    Hooks hooks_;
};

// A field of `wide` columns at the top of a scene, focused, under a catcher.
struct Typed {
    Scene scene;
    Document document{scene.time()};
    Catcher* catcher = nullptr;
    Field* field = nullptr;
    Hooks hooks;

    // Builds it with `said` in the field.
    explicit Typed(std::string said = {}, int wide = 10) : scene(Size{wide, 3}) {
        catcher = &document.children().add<Catcher>();
        field = &catcher->children().add<Field>(std::move(said));
        scene.show(document);
        field->focus();
        scene.wait(0ms);
    }

    // Presses each key of `keys`.
    void pressed(const std::vector<Key>& keys) {
        for (const Key& one : keys) scene.press(one);
    }

    // The column of the caret.
    int caret() { return scene.caret() ? scene.caret()->at.x : -1; }
};

// Letters go in at the cursor; the arrows, Home and End move it a character at a time, also over a
// character of several bytes; Backspace and Delete take one away. The caret stands at the cursor.
TRIAL("Field: typing, moving and erasing characters") {
    Typed typed{"a\xc5\xbc"};
    Field& field = *typed.field;

    SAME(*field.cursor, std::size_t{3});
    SAME(typed.caret(), 2);
    OUGHT(field.has_class("input"));

    typed.pressed({Key{Code::left}, Key{U'b'}, Key{Code::home}, Key{U'X'}});

    SAME(*field.text, std::string{"Xab\xc5\xbc"});
    SAME(typed.scene.row(0), std::string{"Xab\xc5\xbc      "});
    SAME(typed.caret(), 1);

    typed.pressed({Key{Code::end}, Key{Code::backspace}, Key{Code::home}, Key{Code::remove}, Key{Code::right}});

    SAME(*field.text, std::string{"ab"});
    SAME(*field.cursor, std::size_t{1});

    typed.pressed({Key{Code::end}, Key{Code::remove}, Key{Code::right}, Key{Code::home}, Key{Code::backspace}, Key{Code::left}});

    SAME(*field.text, std::string{"ab"});
    SAME(*field.cursor, std::size_t{0});
    OUGHT(typed.scene.at({0, 0}).look.under);

    field.cursor = 99;
    typed.scene.wait(0ms);

    SAME(typed.caret(), 2);
}

// Ctrl with an arrow goes past what is not a word and then past the word.
TRIAL("Field: Ctrl and the arrows go by words") {
    Typed typed{"one, two three", 20};
    Field& field = *typed.field;

    typed.pressed({Key{Code::left, ctrl}});

    SAME(*field.cursor, std::size_t{9});

    typed.pressed({Key{Code::left, ctrl}, Key{Code::left, ctrl}});

    SAME(*field.cursor, std::size_t{0});

    typed.pressed({Key{Code::right, ctrl}});

    SAME(*field.cursor, std::size_t{3});

    typed.pressed({Key{Code::right, ctrl}, Key{Code::right, ctrl}, Key{Code::right, ctrl}});

    SAME(*field.cursor, std::size_t{14});
}

// Before a letter goes in the field tells `Typing`; a handler that takes it refuses the letter,
// also in a paste, which comes in one line, its line ends (\r\n, \n or \r) as spaces.
TRIAL("Field: letters refused, and a paste in one line") {
    Typed typed;
    Field& field = *typed.field;

    typed.hooks.add(field.on<Typing>([](Typing& what) {
        if (what.letter == U'x') what.take();
    }));

    typed.pressed({Key{U'a'}, Key{U'x'}, Key{U'b'}});
    typed.scene.paste("c\r\nxd\re");

    SAME(*field.text, std::string{"abc d e"});

    typed.scene.paste("xx");

    SAME(*field.text, std::string{"abc d e"});
}

// After every change the field asks for a guess; what a handler gives stands after the cursor at
// the end, in the part "guess". The right arrow there takes one letter of it, End all of it. Away
// from the end, or with the text hidden, there is none.
TRIAL("Field: a guess after the cursor, taken by the right arrow and End") {
    Typed typed;
    Field& field = *typed.field;
    const std::vector<std::string> words{"poirot", "pocket"};

    typed.hooks.add(field.on<Guessing>([&words](Guessing& what) {
        for (const std::string& one : words)
            if (!what.text.empty() && one.starts_with(what.text) && one != what.text) what.ahead = one.substr(what.text.size());
    }));

    typed.pressed({Key{U'p'}, Key{U'o'}, Key{U'c'}});

    SAME(field.guessed(), std::string{"ket"});
    SAME(typed.scene.row(0), std::string{"pocket    "});
    OUGHT(typed.scene.at({3, 0}).look.faint);
    OUGHT(!typed.scene.at({2, 0}).look.faint);

    typed.pressed({Key{Code::right}});

    SAME(*field.text, std::string{"pock"});

    typed.pressed({Key{Code::left}});

    SAME(field.guessed(), std::string{});

    typed.pressed({Key{Code::end}, Key{Code::end}});

    SAME(*field.text, std::string{"pocket"});
    SAME(field.guessed(), std::string{});

    field.text = "po";
    field.cursor = 2;
    field.hidden = true;
    typed.scene.wait(0ms);

    SAME(field.guessed(), std::string{});
    SAME(typed.scene.row(0), std::string{"\xe2\x80\xa2\xe2\x80\xa2        "});
    SAME(typed.caret(), 2);
}

// Typing joins in one step until a space, erasing in another; a move of the cursor seals them,
// also one by the program, and Backspace at the start is no step. Ctrl+Z and Ctrl+Y undo and redo
// with the cursor; a text set by the program forgets the history.
TRIAL("Field: undo and redo of typing and erasing") {
    Typed typed;
    Field& field = *typed.field;

    typed.pressed({Key{U'a'}, Key{U'b'}, Key{U' '}, Key{U'c'}, Key{U'd'}, Key{Code::backspace}, Key{Code::backspace}});

    SAME(*field.text, std::string{"ab "});

    typed.pressed({Key{U'z', ctrl}});

    SAME(*field.text, std::string{"ab cd"});
    SAME(*field.cursor, std::size_t{5});

    typed.pressed({Key{U'z', ctrl}});

    SAME(*field.text, std::string{"ab"});

    typed.pressed({Key{U'Y', ctrl}});

    SAME(*field.text, std::string{"ab cd"});

    typed.pressed({Key{Code::home}, Key{U'x'}, Key{Code::end}, Key{U'y'}, Key{U'z', ctrl}});

    SAME(*field.text, std::string{"xab cd"});

    field.text = "new";
    typed.scene.wait(0ms);
    typed.pressed({Key{U'z', ctrl}});

    SAME(*field.text, std::string{"new"});

    typed.pressed({Key{U'1'}});
    field.cursor = 1;
    field.cursor = 4;
    typed.pressed({Key{U'2'}, Key{Code::home}, Key{Code::backspace}, Key{U'z', ctrl}});

    SAME(*field.text, std::string{"new1"});
}

// Backspace and Delete are steps of their own kinds, so they do not join; Delete held joins. Undo
// puts the cursor where it stood, redo after what it did again.
TRIAL("Field: Backspace and Delete stand apart in the history") {
    Typed typed{"abcd"};
    Field& field = *typed.field;

    typed.pressed({Key{Code::home}, Key{Code::right}, Key{Code::right}, Key{Code::backspace}, Key{Code::remove}});

    SAME(*field.text, std::string{"ad"});

    typed.pressed({Key{U'z', ctrl}});

    SAME(*field.text, std::string{"acd"});
    SAME(*field.cursor, std::size_t{1});

    typed.pressed({Key{U'z', ctrl}});

    SAME(*field.text, std::string{"abcd"});
    SAME(*field.cursor, std::size_t{2});

    typed.pressed({Key{U'y', ctrl}});

    SAME(*field.text, std::string{"acd"});
    SAME(*field.cursor, std::size_t{1});

    typed.pressed({Key{Code::remove}, Key{Code::remove}});

    SAME(*field.text, std::string{"a"});

    typed.pressed({Key{U'z', ctrl}});

    SAME(*field.text, std::string{"acd"});
    SAME(*field.cursor, std::size_t{1});
}

// A text longer than the field shifts so that the cursor shows; while it fits it stands as
// `justify` says. A left click puts the cursor before the character it hits.
TRIAL("Field: a long text shifted, a short one justified, a click") {
    Typed typed{"", 5};
    Field& field = *typed.field;

    typed.pressed({Key{U'a'}, Key{U'b'}, Key{U'c'}, Key{U'd'}, Key{U'e'}, Key{U'f'}});

    SAME(typed.scene.row(0), std::string{"cdef "});
    SAME(typed.caret(), 4);

    typed.pressed({Key{Code::home}});

    SAME(typed.scene.row(0), std::string{"abcde"});
    SAME(typed.caret(), 0);

    typed.scene.click({3, 0});

    SAME(*field.cursor, std::size_t{3});

    typed.scene.click({1, 0}, tal::MouseButton::right);

    SAME(*field.cursor, std::size_t{3});

    Typed shortened{"ab", 6};

    shortened.field->justify = Edge::end;
    shortened.scene.wait(0ms);

    SAME(shortened.scene.row(0), std::string{"   ab "});
    SAME(shortened.caret(), 5);

    shortened.scene.click({3, 0});

    SAME(*shortened.field->cursor, std::size_t{0});

    shortened.field->justify = Edge::middle;
    shortened.scene.wait(0ms);

    SAME(shortened.scene.row(0), std::string{" ab   "});
}

// Tab, Enter and keys with Alt or with Ctrl and another letter go on to the nodes above.
TRIAL("Field: what it leaves to the nodes above") {
    Typed typed;

    typed.pressed({Key{Code::tab}, Key{Code::enter}, Key{U'a', Mods{false, true, false}}, Key{U'q', ctrl}, Key{Code::f2}, Key{U'z', Mods{true, true, false}}});

    OUGHT(typed.catcher->heard == (std::vector<std::string>{"Tab", "Enter", "Alt+A", "Ctrl+Q", "F2", "Ctrl+Alt+Z"}));

    typed.pressed({Key{}});

    SAME(typed.catcher->heard.size(), std::size_t{7});
    SAME(*typed.field->text, std::string{});
}

// The text shifts to keep the cursor in view from the start, and as the text, the cursor or the
// size change; a text as wide as the field shifts too, leaving the cursor its cell.
TRIAL("Field: the cursor kept in view") {
    Typed typed{"abcdefgh", 5};
    Field& field = *typed.field;

    SAME(typed.scene.row(0), std::string{"efgh "});

    typed.pressed({Key{Code::backspace}});

    SAME(typed.scene.row(0), std::string{"defg "});

    field.cursor = 0;
    typed.scene.wait(0ms);

    SAME(typed.scene.row(0), std::string{"abcde"});
    SAME(typed.caret(), 0);

    Typed full{"abcde", 5};

    SAME(full.scene.row(0), std::string{"bcde "});
    SAME(full.caret(), 4);
}

// A cursor set inside a character stands before it; a hidden text takes one glyph a character,
// however wide it is; a click on either half of a wide character goes before it; a frame moves
// where a click lands.
TRIAL("Field: a cursor inside a character, a hidden wide text, a framed field") {
    Typed typed{"e\xcc\x81x"};
    Field& field = *typed.field;

    field.cursor = 1;
    typed.scene.wait(0ms);

    SAME(typed.caret(), 0);

    typed.pressed({Key{U'a'}});

    SAME(*field.text, std::string{"ae\xcc\x81x"});

    Typed wide{"\xe6\x97\xa5\xe6\x9c\xac"};

    wide.field->hidden = true;
    wide.scene.wait(0ms);

    SAME(wide.caret(), 2);

    wide.field->hidden = false;
    wide.scene.wait(0ms);
    wide.scene.click({3, 0});

    SAME(*wide.field->cursor, std::size_t{3});

    Typed framed{"abcd"};

    framed.field->add_class("history");
    framed.scene.wait(0ms);
    framed.scene.click({3, 1});

    SAME(*framed.field->cursor, std::size_t{2});
}

// A guess asked again when nothing changed shows when the answer did; a move that changes nothing
// paints nothing.
TRIAL("Field: a guess that changed with the words, and nothing painted for nothing") {
    Typed typed;
    Field& field = *typed.field;
    std::vector<std::string> words;

    typed.hooks.add(field.on<Guessing>([&words](Guessing& what) {
        for (const std::string& one : words)
            if (one.starts_with(what.text) && one != what.text) what.ahead = one.substr(what.text.size());
    }));

    typed.pressed({Key{U'p'}});
    words = {"pq"};
    typed.pressed({Key{Code::end}});

    SAME(typed.scene.row(0), std::string{"pq        "});

    typed.pressed({Key{Code::home}});

    const std::uint64_t frames = typed.document.framed();

    typed.pressed({Key{Code::home}});

    SAME(typed.document.framed(), frames);
}

// A screen of layers holding a full window with a history field on top.
struct Remembering {
    Scene scene;
    Document document{scene.time()};
    tal::Layers* layers = nullptr;
    HistoryField* field = nullptr;

    // Builds it with `earlier` in the history, on a screen `high` rows high.
    explicit Remembering(std::vector<std::string> earlier, int high = 8) : scene(Size{20, high}) {
        layers = &document.children().add<tal::Layers>();

        auto made = std::make_shared<tal::Window>();

        made->add_class("screen");
        made->full = true;

        tal::Window& screen = layers->open(made);
        tal::Column& column = screen.children().add<tal::Column>();
        tal::Row& row = column.children().add<tal::Row>();

        (void)row.children().add<tal::Label>("> ");
        field = &row.children().add<HistoryField>(std::move(earlier));
        field->share(tal::Share{1, 0});
        scene.show(document);
        field->focus();
        scene.wait(0ms);
    }
};

// Up and Down walk the history, the newest first; below the newest the text typed before comes
// back, and typing leaves the walk; with Shift they are not its. An entry remembered goes to the
// front without a twin, the oldest past `keeps` going away; an empty one is not remembered.
TRIAL("HistoryField: walking and remembering") {
    Remembering shown{{"two", "one"}};
    HistoryField& field = *shown.field;

    shown.scene.type("x");
    shown.scene.press(Key{Code::down});

    SAME(*field.text, std::string{"x"});

    shown.scene.press(Key{Code::up});
    shown.scene.press(Key{Code::up});
    shown.scene.press(Key{Code::up});

    SAME(*field.text, std::string{"one"});

    shown.scene.press(Key{Code::down});

    SAME(*field.text, std::string{"two"});

    shown.scene.press(Key{Code::down});

    SAME(*field.text, std::string{"x"});

    shown.scene.press(Key{Code::up});
    shown.scene.press(Key{Code::up});
    shown.scene.type("!");
    shown.scene.press(Key{Code::down});

    SAME(*field.text, std::string{"one!"});

    shown.scene.press(Key{Code::up, Mods{false, false, true}});

    SAME(*field.text, std::string{"one!"});

    field.remember("one");
    field.remember("");
    field.remember("three");

    OUGHT(field.history() == (std::vector<std::string>{"three", "one", "two"}));

    field.keeps = 2;

    OUGHT(field.history() == (std::vector<std::string>{"three", "one"}));

    field.remember("four");

    OUGHT(field.history() == (std::vector<std::string>{"four", "three"}));

    Remembering empty{{}};

    empty.scene.press(Key{Code::up});
    empty.field->open();

    OUGHT(!empty.field->opened());
}

// Each entry walked to is a step of its own to undo, joining neither the erasing before it nor the
// erasing or typing after it. Walking to the text that stands moves the cursor to its end, and
// undoing that puts the cursor back.
TRIAL("HistoryField: entries walked to undone on their own") {
    Remembering shown{{"two", "one"}};
    HistoryField& field = *shown.field;

    shown.scene.type("ab");
    shown.scene.press(Key{Code::backspace});
    shown.scene.press(Key{Code::up});
    shown.scene.press(Key{Code::backspace});
    shown.scene.press(Key{U'z', ctrl});

    SAME(*field.text, std::string{"two"});

    shown.scene.press(Key{U'z', ctrl});

    SAME(*field.text, std::string{"a"});

    shown.scene.press(Key{Code::up});
    shown.scene.type("x");

    SAME(*field.text, std::string{"twox"});

    shown.scene.press(Key{U'z', ctrl});

    SAME(*field.text, std::string{"two"});

    Remembering same{{"one"}};

    same.scene.type("one");
    same.scene.press(Key{Code::home});
    same.scene.press(Key{Code::up});

    SAME(*same.field->text, std::string{"one"});
    SAME(*same.field->cursor, std::size_t{3});

    same.scene.press(Key{U'z', ctrl});

    SAME(*same.field->text, std::string{"one"});
    SAME(*same.field->cursor, std::size_t{0});
}

// The history opens as a framed list under the field; Enter takes the entry, a click takes the one
// under it, Esc and a press outside close it, the focus back in the field. It shows ten entries at
// most.
TRIAL("HistoryField: the list of the history under the field") {
    Remembering shown{{"alpha", "beta", "gamma"}};
    HistoryField& field = *shown.field;

    field.open();
    shown.scene.wait(0ms);

    OUGHT(field.opened());
    OUGHT(shown.scene.row(1).starts_with("\xe2\x94\x8c"));
    OUGHT(shown.scene.row(2).starts_with("\xe2\x94\x82" "alpha "));

    shown.scene.press(Key{Code::down});
    shown.scene.press(Key{Code::enter});

    OUGHT(!field.opened());
    SAME(*field.text, std::string{"beta"});
    OUGHT(field.focused());

    field.open();
    shown.scene.wait(0ms);
    shown.scene.click({4, 4});

    SAME(*field.text, std::string{"gamma"});
    OUGHT(!field.opened());

    field.open();
    shown.scene.wait(0ms);
    field.open();
    shown.scene.press(Key{Code::escape});

    OUGHT(!field.opened());
    OUGHT(field.focused());
    OUGHT(!shown.scene.row(1).starts_with("\xe2\x94\x8c"));

    field.open();
    shown.scene.wait(0ms);
    field.open();
    shown.scene.click({15, 7});

    OUGHT(!field.opened());
    SAME(*field.text, std::string{"gamma"});

    field.open();
    field.close();
    field.close();

    OUGHT(!field.opened());

    HistoryField alone{{"x"}};

    alone.open();

    OUGHT(!alone.opened());

    Remembering many{{"1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12"}, 16};

    many.field->open();
    many.scene.wait(0ms);

    OUGHT(many.scene.row(12).starts_with("\xe2\x94\x94"));
}

}
