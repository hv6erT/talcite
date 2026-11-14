#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/clipboard.hpp"
#include "tal/controls.hpp"
#include "tal/document.hpp"
#include "tal/editing.hpp"
#include "tal/event.hpp"
#include "tal/executor.hpp"
#include "tal/geometry.hpp"
#include "tal/key.hpp"
#include "tal/layout.hpp"
#include "tal/node.hpp"
#include "tal/pages.hpp"
#include "tal/port.hpp"
#include "tal/scene.hpp"
#include "tal/style.hpp"
#include "tal/task.hpp"
#include "tal/undo.hpp"

#include "quieting.hpp"
#include "tal/testing.hpp"

// Editors on a scene: typing, erasing and the moves, the selection by keys and by the mouse, the
// history, the block, the clipboard, finding and replacing, and two editors of one text.

namespace {

using namespace std::chrono_literals;

using tal::Code;
using tal::Comb;
using tal::Document;
using tal::Editor;
using tal::Key;
using tal::Mods;
using tal::MouseButton;
using tal::Place;
using tal::Press;
using tal::Range;
using tal::ScriptedClipboard;
using tal::Sift;
using tal::Size;
using tal::Spot;
using tal::Text;
using tal::Undo;
using tal::testing::Scene;

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

// Shift, Ctrl, and both.
constexpr Mods shift{false, false, true};
constexpr Mods ctrl{true, false, false};
constexpr Mods both{true, false, true};

// A column that counts the keys and the pastes nothing under it took, as the nodes above an editor.
class Catcher : public tal::Column {
public:
    // Counts what is left to it.
    Catcher() {
        hooks_.add(otherwise<tal::KeyEvent>([this](tal::KeyEvent& what) {
            (void)what;

            ++heard;
        }));
        hooks_.add(otherwise<tal::PasteEvent>([this](tal::PasteEvent& what) {
            (void)what;

            ++pasted;
        }));
    }

    int heard = 0;
    int pasted = 0;

private:
    tal::Hooks hooks_;
};

// An editor filling a scene of `size` under a catcher, focused, with a scripted clipboard; its loop
// is the program's.
struct Written {
    tal::Loop loop;
    Scene scene;
    Document document{scene.time()};
    std::shared_ptr<Text> text;
    std::shared_ptr<ScriptedClipboard> board = std::make_shared<ScriptedClipboard>();
    Catcher* catcher = nullptr;
    Editor* editor = nullptr;

    // An editor of the text `said`, with the history `undo`.
    Written(const std::string& said, Size size, std::shared_ptr<Undo> undo = std::make_shared<Undo>())
        : Written(std::make_shared<Text>(said), size, std::move(undo)) {}

    // An editor of `given`, with the history `undo`.
    Written(std::shared_ptr<Text> given, Size size, std::shared_ptr<Undo> undo) : scene(size), text(std::move(given)) {
        catcher = &document.children().add<Catcher>();
        editor = &catcher->children().add<Editor>(text, std::move(undo), tal::Port<tal::Clipboard>{board});
        editor->share(tal::Share{1, 0});
        scene.show(document);
        editor->focus();
        scene.wait(0ms);
    }

    // Presses `key` `times` times.
    void press(Key key, int times = 1) {
        for (int one = 0; one < times; ++one) scene.press(key);
    }

    // What the mouse did at `at`, with the left button.
    void mouse(Press press, Spot at, Mods mods = {}) {
        document.mouse(press, MouseButton::left, at, mods);
        scene.wait(0ms);
    }

    // The rows of the screen joined by |.
    std::string rows() {
        std::string made;

        for (int y = 0; y < scene.screen().size().high; ++y) made += (y > 0 ? "|" : "") + scene.row(y);

        return made;
    }

    // The row `y` as # for a lit cell and . for another.
    std::string lit(int y) {
        std::string made;

        for (int x = 0; x < scene.screen().size().wide; ++x) made += scene.at({x, y}).look.reverse ? '#' : '.';

        return made;
    }

    // The cell of the caret.
    std::optional<Spot> caret() {
        const std::optional<tal::Caret> got = scene.caret();

        if (!got) return std::nullopt;

        return got->at;
    }

    // Runs `work` on the loop.
    bool ran(tal::Task<bool> work) {
        const bool made = tal::wait(loop, std::move(work));

        scene.wait(0ms);

        return made;
    }
};

}

// Letters type at the cursor, Enter breaks the line, Backspace and Delete erase a character or a
// line end; an empty last line has its row and the cursor stands on it. The editor is a page too.
TRIAL("Editor: typing, line ends and erasing") {
    Written shown{"", {6, 3}};
    Editor& editor = *shown.editor;

    OUGHT(editor.has_class("page"));
    OUGHT(editor.has_class("editor"));
    OUGHT(!*editor.wrap);
    OUGHT(shown.caret() == (std::optional<Spot>{Spot{0, 0}}));

    shown.scene.type("ab");
    shown.press(Key{Code::enter});

    SAME(shown.text->whole(), std::string{"ab\n"});
    OUGHT(*editor.cursor == (Place{1, 0}));
    OUGHT(shown.caret() == (std::optional<Spot>{Spot{0, 1}}));
    SAME(editor.lines(), std::optional<std::size_t>{2});

    shown.scene.type("c");

    SAME(shown.rows(), std::string{"ab    |c     |      "});

    shown.press(Key{Code::backspace}, 2);

    SAME(shown.text->whole(), std::string{"ab"});
    OUGHT(*editor.cursor == (Place{0, 2}));

    shown.press(Key{Code::left});
    shown.press(Key{Code::remove});

    SAME(shown.text->whole(), std::string{"a"});
    OUGHT(*editor.cursor == (Place{0, 1}));

    shown.press(Key{Code::remove});
    shown.press(Key{Code::home});
    shown.press(Key{Code::backspace});

    SAME(shown.text->whole(), std::string{"a"});

    shown.scene.type("\xc4\x85");
    shown.press(Key{Code::backspace});

    SAME(shown.text->whole(), std::string{"a"});

    shown.press(Key{Code::remove, shift});
    shown.press(Key{U'x', Mods{false, true, false}});
    shown.press(Key{U'x', ctrl});
    shown.press(Key{Code::up, ctrl});
    shown.press(Key{});
    shown.press(Key{Code::tab});

    SAME(shown.text->whole(), std::string{"a"});
    SAME(shown.catcher->heard, 6);
    OUGHT(throws<std::invalid_argument>([] { Editor none{std::shared_ptr<Text>{}}; }));
    OUGHT(throws<std::invalid_argument>([] { Editor none{std::make_shared<Text>(), std::shared_ptr<Undo>{}}; }));
}

// The arrows go by characters, with Ctrl to the sides by words and across line ends; up and down
// keep the column they started in, past short lines and a tab; Home and End go to the ends of the
// line and the page keys a page. Up on the first row and down on the last are left to the nodes
// above.
TRIAL("Editor: moving by characters, words, lines and pages") {
    Written shown{"one two\nthree\n\tx\n", {10, 3}};
    Editor& editor = *shown.editor;
    const int& above = shown.catcher->heard;

    shown.press(Key{Code::right});
    shown.press(Key{Code::right, ctrl});

    OUGHT(*editor.cursor == (Place{0, 3}));

    shown.press(Key{Code::right, ctrl});

    OUGHT(*editor.cursor == (Place{0, 7}));

    shown.press(Key{Code::right, ctrl});

    OUGHT(*editor.cursor == (Place{1, 0}));

    shown.press(Key{Code::left, ctrl});

    OUGHT(*editor.cursor == (Place{0, 7}));

    shown.press(Key{Code::left, ctrl});

    OUGHT(*editor.cursor == (Place{0, 4}));

    shown.press(Key{Code::left, ctrl});

    OUGHT(*editor.cursor == (Place{0, 0}));

    editor.cursor = Place{0, 7};
    shown.press(Key{Code::end});
    shown.press(Key{Code::down});

    OUGHT(*editor.cursor == (Place{1, 5}));

    shown.press(Key{Code::down});

    OUGHT(*editor.cursor == (Place{2, 0}));

    shown.press(Key{Code::down});

    OUGHT(*editor.cursor == (Place{3, 0}));
    SAME(above, 0);

    shown.press(Key{Code::down});

    OUGHT(*editor.cursor == (Place{3, 0}));
    SAME(above, 1);

    shown.press(Key{Code::up}, 3);

    OUGHT(*editor.cursor == (Place{0, 7}));

    shown.press(Key{Code::up});

    SAME(above, 2);

    editor.cursor = Place{2, 1};

    SAME(editor.column(), 8);

    shown.press(Key{Code::home});

    OUGHT(*editor.cursor == (Place{2, 0}));

    shown.press(Key{Code::pageup});

    OUGHT(*editor.cursor == (Place{0, 0}));

    shown.press(Key{Code::pagedown});

    OUGHT(*editor.cursor == (Place{3, 0}));
    SAME(editor.first(), std::size_t{1});

    shown.press(Key{Code::pagedown});

    SAME(above, 3);
    SAME(shown.text->whole(), std::string{"one two\nthree\n\tx\n"});

    Written columns{"abcdef\nabcdef\nabcdef\nabcdef", {8, 4}};
    Editor& one = *columns.editor;

    one.cursor = Place{0, 5};
    columns.press(Key{Code::down});
    one.cursor = Place{1, 1};
    columns.press(Key{Code::down});

    OUGHT(*one.cursor == (Place{2, 1}));

    one.cursor = Place{0, 5};
    columns.press(Key{Code::down});

    OUGHT(one.find(Comb{"b", Sift{}}));

    columns.press(Key{Code::down});

    OUGHT(*one.cursor == (Place{3, 2}));

    one.cursor = Place{0, 5};
    columns.press(Key{Code::down});
    columns.scene.twice({0, 2});
    columns.press(Key{Code::down});

    OUGHT(*one.cursor == (Place{3, 6}));
}

// With wrapping the up and down keys go by rows of the screen, keeping the column in the row; the
// cursor at the end of a full row shows at the start of the next one.
TRIAL("Editor: moving by rows of wrapped lines") {
    Written shown{"abcdefghij\nxy", {4, 4}};
    Editor& editor = *shown.editor;

    editor.wrap = true;
    shown.scene.wait(0ms);

    SAME(shown.rows(), std::string{"abcd|efgh|ij  |xy  "});

    shown.press(Key{Code::right});
    shown.press(Key{Code::down});

    OUGHT(*editor.cursor == (Place{0, 5}));

    shown.press(Key{Code::down});

    OUGHT(*editor.cursor == (Place{0, 9}));

    shown.press(Key{Code::down});

    OUGHT(*editor.cursor == (Place{1, 1}));

    shown.press(Key{Code::up}, 2);

    OUGHT(*editor.cursor == (Place{0, 5}));

    shown.press(Key{Code::end});
    shown.press(Key{Code::up});

    OUGHT(*editor.cursor == (Place{0, 6}));

    editor.cursor = Place{0, 4};
    shown.scene.wait(0ms);

    OUGHT(shown.caret() == (std::optional<Spot>{Spot{0, 1}}));

    shown.press(Key{Code::end});

    OUGHT(shown.caret() == (std::optional<Spot>{Spot{2, 2}}));

    editor.cursor = Place{0, 3};
    shown.press(Key{Code::down});

    OUGHT(*editor.cursor == (Place{0, 7}));

    shown.press(Key{Code::down});

    OUGHT(*editor.cursor == (Place{0, 10}));
}

// Moves with Shift select from where the cursor stood, lit in the state select, the end of a line
// too when the selection goes past it; typing puts the letter in place of the selection, and a
// move without Shift selects nothing. Undoing an erased selection puts the cursor back where it
// stood, at its start when it was made backwards.
TRIAL("Editor: selecting by keys and typing over it") {
    Written shown{"hello world", {12, 2}};
    Editor& editor = *shown.editor;

    shown.press(Key{Code::right, shift}, 5);

    OUGHT(editor.selected() == (Range{Place{0, 0}, Place{0, 5}}));
    SAME(shown.lit(0), std::string{"#####......."});

    shown.scene.type("X");

    SAME(shown.text->whole(), std::string{"X world"});
    OUGHT(!editor.selected().has_value());
    SAME(shown.lit(0), std::string{"............"});

    shown.press(Key{Code::end, shift});
    shown.press(Key{Code::left});

    OUGHT(!editor.selected().has_value());
    OUGHT(*editor.cursor == (Place{0, 6}));

    shown.press(Key{Code::left, both});

    OUGHT(editor.selected() == (Range{Place{0, 2}, Place{0, 6}}));
    OUGHT(*editor.anchor == std::optional<Place>{Place{0, 6}});

    shown.press(Key{Code::backspace});

    SAME(shown.text->whole(), std::string{"X d"});
    OUGHT(editor.undo());
    SAME(shown.text->whole(), std::string{"X world"});
    OUGHT(*editor.cursor == (Place{0, 2}));
    OUGHT(!editor.selected().has_value());

    Written lines{"ab\ncd", {4, 2}};

    lines.press(Key{Code::right});
    lines.press(Key{Code::down, shift});

    OUGHT(lines.editor->selected() == (Range{Place{0, 1}, Place{1, 1}}));
    SAME(lines.lit(0), std::string{".##."});
    SAME(lines.lit(1), std::string{"#..."});
}

// A press puts the cursor and takes the focus, a drag selects, Shift with a press stretches the
// selection, a double click selects a word or a run of spaces; below the text stands its end. A
// drag past the bottom moves the view a row, and the wheel moves it as on a page.
TRIAL("Editor: the mouse puts the cursor, selects and moves the view") {
    Written shown{"one two\nthree", {10, 3}};
    Editor& editor = *shown.editor;

    shown.document.focus();
    shown.mouse(Press::down, {5, 0});
    shown.mouse(Press::up, {5, 0});

    OUGHT(editor.focused());
    OUGHT(*editor.cursor == (Place{0, 5}));
    OUGHT(!editor.anchor->has_value());

    shown.scene.point({8, 1});
    shown.document.mouse(Press::down, MouseButton::right, {1, 1});
    shown.scene.wait(0ms);

    OUGHT(*editor.cursor == (Place{0, 5}));
    OUGHT(!editor.selected().has_value());

    shown.mouse(Press::down, {1, 0});
    shown.mouse(Press::move, {3, 1});
    shown.mouse(Press::up, {3, 1});

    OUGHT(editor.selected() == (Range{Place{0, 1}, Place{1, 3}}));

    shown.mouse(Press::down, {1, 1});
    shown.mouse(Press::move, {3, 1});
    shown.mouse(Press::move, {1, 1});
    shown.mouse(Press::up, {1, 1});

    OUGHT(!editor.anchor->has_value());

    editor.anchor = *editor.cursor;

    OUGHT(!editor.selected().has_value());

    editor.anchor = Place{0, 1};
    shown.mouse(Press::down, {7, 1}, shift);
    shown.mouse(Press::up, {7, 1}, shift);

    OUGHT(editor.selected() == (Range{Place{0, 1}, Place{1, 5}}));

    shown.scene.twice({5, 0});

    OUGHT(editor.selected() == (Range{Place{0, 4}, Place{0, 7}}));
    OUGHT(*editor.cursor == (Place{0, 7}));

    shown.scene.twice({3, 0});

    OUGHT(editor.selected() == (Range{Place{0, 3}, Place{0, 4}}));

    shown.scene.twice({9, 0});

    OUGHT(editor.selected() == (Range{Place{0, 4}, Place{0, 7}}));

    shown.scene.click({2, 2});

    OUGHT(*editor.cursor == (Place{1, 5}));

    Written tall{"0\n1\n2\n3\n4\n5\n6", {4, 3}};

    tall.mouse(Press::down, {0, 0});
    tall.mouse(Press::move, {0, 5});

    OUGHT(*tall.editor->cursor == (Place{3, 0}));
    SAME(tall.editor->first(), std::size_t{1});

    tall.mouse(Press::move, {0, 5});
    tall.mouse(Press::up, {0, 5});

    OUGHT(*tall.editor->cursor == (Place{4, 0}));
    OUGHT(tall.editor->selected() == (Range{Place{0, 0}, Place{4, 0}}));

    tall.scene.wheel({0, 0}, -1);

    SAME(tall.editor->first(), std::size_t{4});
    OUGHT(*tall.editor->cursor == (Place{4, 0}));
}

// Typing joins one step up to a space, a tab or a line end, as in a field; Backspace and Delete
// held join their own steps inside a line. Undo puts the cursor where it stood before the step,
// redo after what it did again; a move seals. The saved place tells whether the text differs from
// the save.
TRIAL("Editor: the history of typing and erasing") {
    Written shown{"", {12, 2}};
    Editor& editor = *shown.editor;

    shown.scene.type("ab cd");

    SAME(*editor.history().undoing(), std::string{"Typing"});
    OUGHT(editor.undo());
    SAME(shown.text->whole(), std::string{"ab"});
    OUGHT(*editor.cursor == (Place{0, 2}));
    OUGHT(editor.undo());
    SAME(shown.text->whole(), std::string{});
    OUGHT(!editor.undo());
    OUGHT(editor.redo());
    OUGHT(*editor.cursor == (Place{0, 2}));
    OUGHT(editor.redo());
    OUGHT(!editor.redo());
    SAME(shown.text->whole(), std::string{"ab cd"});

    editor.history().saved();

    OUGHT(!editor.history().changed());

    shown.press(Key{Code::backspace}, 2);

    SAME(shown.text->whole(), std::string{"ab "});
    SAME(*editor.history().undoing(), std::string{"Erasing"});
    OUGHT(editor.history().changed());
    OUGHT(editor.undo());
    SAME(shown.text->whole(), std::string{"ab cd"});
    OUGHT(*editor.cursor == (Place{0, 5}));
    OUGHT(!editor.history().changed());

    shown.press(Key{Code::home});
    shown.press(Key{Code::remove}, 2);

    SAME(shown.text->whole(), std::string{" cd"});
    OUGHT(editor.undo());
    SAME(shown.text->whole(), std::string{"ab cd"});
    OUGHT(*editor.cursor == (Place{0, 0}));

    shown.press(Key{Code::right}, 2);

    shown.scene.type("x");
    shown.press(Key{Code::left});
    shown.press(Key{Code::right});
    shown.scene.type("y");

    SAME(shown.text->whole(), std::string{"abxy cd"});
    OUGHT(editor.undo());
    SAME(shown.text->whole(), std::string{"abx cd"});

    shown.press(Key{Code::end});
    shown.scene.type("e");
    shown.press(Key{Code::enter});
    shown.scene.type("f");

    OUGHT(editor.undo());
    SAME(shown.text->whole(), std::string{"abx cde"});

    shown.press(Key{Code::backspace});
    shown.press(Key{Code::backspace});
    shown.press(Key{Code::left});
    shown.press(Key{Code::backspace});

    OUGHT(editor.undo());
    SAME(shown.text->whole(), std::string{"abx c"});
    OUGHT(editor.undo());
    SAME(shown.text->whole(), std::string{"abx cde"});

    shown.press(Key{Code::end});
    shown.scene.type("gh");
    editor.anchor = Place{0, 0};
    editor.anchor = std::nullopt;
    shown.scene.type("i");

    OUGHT(editor.undo());
    SAME(shown.text->whole(), std::string{"abx cdegh"});

    const std::string before = *editor.history().undoing();

    editor.insert("");
    shown.press(Key{Code::remove});
    SAME(editor.replace_all(Comb{"zz", Sift{}}, "u"), std::size_t{0});

    SAME(*editor.history().undoing(), before);
}

// A read-only editor changes nothing: typing, erasing, the history and the block do nothing, while
// moving, selecting and finding go on.
TRIAL("Editor: read-only") {
    Written shown{"one two", {10, 2}};
    Editor& editor = *shown.editor;

    shown.scene.type("x");
    shown.press(Key{Code::left});
    shown.press(Key{Code::right});
    shown.scene.type("w");

    OUGHT(editor.undo());

    editor.readonly = true;

    shown.scene.type("y");
    shown.press(Key{Code::enter});
    shown.press(Key{Code::backspace});
    shown.press(Key{Code::remove});
    editor.insert("z");

    SAME(shown.text->whole(), std::string{"xone two"});
    OUGHT(!editor.undo());
    OUGHT(!editor.redo());

    shown.press(Key{Code::end, shift});

    OUGHT(editor.selected() == (Range{Place{0, 1}, Place{0, 8}}));
    OUGHT(shown.ran(editor.copy()));
    OUGHT(!shown.ran(editor.cut()));
    SAME(shown.board->copied.size(), std::size_t{1});
    OUGHT(!shown.ran(editor.paste()));

    editor.mark();
    editor.mark();

    OUGHT(!editor.copy_block());
    OUGHT(!editor.move_block());
    OUGHT(!editor.erase_block());
    editor.line(0);

    OUGHT(editor.find(Comb{"two", Sift{}}));
    OUGHT(!editor.replace(Comb{"two", Sift{}}, "2"));
    SAME(editor.replace_all(Comb{"o", Sift{}}, "0"), std::size_t{0});
    SAME(shown.text->whole(), std::string{"xone two"});

    editor.readonly = false;

    OUGHT(editor.undo());
    SAME(shown.text->whole(), std::string{"one two"});
}

// The block is marked from the cursor to where it goes, lit, and ended by the next mark; it is
// copied to the cursor, moved there or erased, each as one step, and follows the changes before it.
TRIAL("Editor: the block") {
    Written shown{"one two three", {20, 2}};
    Editor& editor = *shown.editor;

    OUGHT(!editor.copy_block());

    editor.cursor = Place{0, 4};
    editor.mark();
    shown.press(Key{Code::right}, 3);

    OUGHT(editor.block() == (Range{Place{0, 4}, Place{0, 7}}));
    SAME(shown.lit(0), std::string{"....###............."});

    editor.mark();
    shown.press(Key{Code::end});

    OUGHT(editor.block() == (Range{Place{0, 4}, Place{0, 7}}));
    OUGHT(editor.copy_block());
    SAME(shown.text->whole(), std::string{"one two threetwo"});
    OUGHT(*editor.cursor == (Place{0, 16}));
    OUGHT(editor.block() == (Range{Place{0, 4}, Place{0, 7}}));
    OUGHT(editor.undo());

    shown.press(Key{Code::home});

    OUGHT(editor.move_block());
    SAME(shown.text->whole(), std::string{"twoone  three"});
    OUGHT(editor.block() == (Range{Place{0, 0}, Place{0, 3}}));
    OUGHT(*editor.cursor == (Place{0, 3}));
    OUGHT(editor.undo());
    SAME(shown.text->whole(), std::string{"one two three"});

    editor.cursor = Place{0, 4};
    editor.mark();
    editor.cursor = Place{0, 7};
    editor.cursor = Place{0, 5};

    OUGHT(!editor.move_block());
    OUGHT(editor.block() == (Range{Place{0, 4}, Place{0, 5}}));

    editor.unblock();

    OUGHT(!editor.block().has_value());

    editor.cursor = Place{0, 4};
    editor.mark();
    editor.cursor = Place{0, 7};
    editor.mark();
    editor.cursor = Place{0, 4};
    shown.scene.type("Z");

    OUGHT(editor.block() == (Range{Place{0, 5}, Place{0, 8}}));

    shown.press(Key{Code::home});
    shown.scene.type("XX");

    OUGHT(editor.block() == (Range{Place{0, 7}, Place{0, 10}}));

    shown.press(Key{Code::end});

    OUGHT(editor.erase_block());
    SAME(shown.text->whole(), std::string{"XXone Z three"});
    OUGHT(*editor.cursor == (Place{0, 13}));
    OUGHT(!editor.block().has_value());

    editor.mark();
    editor.mark();

    OUGHT(!editor.block().has_value());
    OUGHT(!editor.erase_block());

    editor.cursor = Place{0, 9};
    editor.mark();
    shown.press(Key{Code::left}, 3);

    OUGHT(editor.block() == (Range{Place{0, 6}, Place{0, 9}}));

    editor.unblock();
    editor.mark();
    shown.press(Key{Code::left});

    OUGHT(editor.block() == (Range{Place{0, 5}, Place{0, 6}}));

    shown.scene.type("Q");

    OUGHT(editor.block() == (Range{Place{0, 6}, Place{0, 7}}));

    shown.press(Key{Code::right}, 2);

    OUGHT(editor.copy_block());
    SAME(shown.text->whole(), std::string{"XXoneQ ZZ three"});
    OUGHT(editor.block() == (Range{Place{0, 7}, Place{0, 8}}));
    OUGHT(*editor.cursor == (Place{0, 9}));

    editor.cursor = Place{0, 8};

    OUGHT(!editor.move_block());

    editor.cursor = Place{0, 7};

    OUGHT(!editor.move_block());
}

// The selection is copied and cut through the clipboard, and the clipboard's text pasted in place
// of the selection; nothing is cut when the clipboard refuses it. A pasted text from the terminal
// goes in as one step, its \r\n as \n.
TRIAL("Editor: copying, cutting and pasting") {
    Written shown{"one two", {10, 2}};
    Editor& editor = *shown.editor;

    OUGHT(!shown.ran(editor.copy()));

    editor.anchor = Place{0, 4};
    editor.cursor = Place{0, 7};

    OUGHT(shown.ran(editor.copy()));
    OUGHT(shown.board->copied == (std::vector<std::string>{"two"}));
    OUGHT(shown.ran(editor.cut()));
    SAME(shown.text->whole(), std::string{"one "});

    shown.press(Key{Code::home});

    OUGHT(shown.ran(editor.paste()));
    SAME(shown.text->whole(), std::string{"twoone "});
    OUGHT(*editor.cursor == (Place{0, 3}));

    shown.press(Key{Code::right, shift});
    shown.board->held = "X";

    OUGHT(shown.ran(editor.paste()));
    SAME(shown.text->whole(), std::string{"twoXne "});

    shown.press(Key{Code::home, shift});
    shown.board->takes = false;

    OUGHT(!shown.ran(editor.cut()));
    SAME(shown.text->whole(), std::string{"twoXne "});

    shown.board->held.reset();

    OUGHT(!shown.ran(editor.paste()));

    shown.board->held = "";

    OUGHT(!shown.ran(editor.paste()));

    shown.scene.paste("p\r\nq");

    SAME(shown.text->whole(), std::string{"p\nqne "});
    SAME(shown.catcher->pasted, 0);
    OUGHT(editor.undo());
    SAME(shown.text->whole(), std::string{"twoXne "});
}

// A clipboard that answers when let through.
class Slow : public tal::Clipboard {
public:
    std::vector<std::string> copied;
    std::optional<std::string> held;
    std::vector<tal::Pending<bool>> gates;

    // Its name.
    [[nodiscard]] std::string_view name() const override { return "slow"; }

    // Takes `what` once let through.
    [[nodiscard]] tal::Task<bool> copy(std::string what) override {
        copied.push_back(std::move(what));

        co_return co_await waiting();
    }

    // Gives what it holds once let through.
    [[nodiscard]] tal::Task<std::optional<std::string>> paste() override {
        (void)co_await waiting();

        co_return held;
    }

    // Lets the waiting answers through.
    void release() {
        for (tal::Pending<bool>& one : gates) one.settle(true);

        gates.clear();
    }

private:
    // An answer that waits to be let through.
    tal::Task<bool> waiting() {
        tal::Pending<bool> gate;

        gates.push_back(gate);

        return gate.task();
    }
};

// Runs `work` into `into`, for a scope.
tal::Task<void> into(tal::Task<bool> work, std::optional<bool>& made) { made = co_await std::move(work); }

// A clipboard that answers later: a cut takes nothing when the selection changed meanwhile, and
// neither a cut nor a paste touches the text when its editor went meanwhile.
TRIAL("Editor: the clipboard answers later") {
    tal::Loop loop;
    Scene scene{{10, 2}};
    Document document{scene.time()};
    const auto text = std::make_shared<Text>("one two");
    const auto slow = std::make_shared<Slow>();
    Editor* editor = &document.children().add<Editor>(text, std::make_shared<Undo>(), tal::Port<tal::Clipboard>{slow});
    tal::Scope scope{loop};
    std::optional<bool> made;

    scene.show(document);
    editor->focus();
    editor->anchor = Place{0, 4};
    editor->cursor = Place{0, 7};
    scope.run(into(editor->cut(), made));
    QUIETED(loop);
    scene.type("X");
    slow->release();
    QUIETED(loop);

    SAME(made, std::optional<bool>{false});
    SAME(text->whole(), std::string{"one X"});
    OUGHT(slow->copied == (std::vector<std::string>{"two"}));

    editor->anchor = Place{0, 0};
    made.reset();
    scope.run(into(editor->cut(), made));
    QUIETED(loop);
    (void)document.children().remove(*editor);
    slow->release();
    QUIETED(loop);

    SAME(made, std::optional<bool>{false});
    SAME(text->whole(), std::string{"one X"});

    editor = &document.children().add<Editor>(text, std::make_shared<Undo>(), tal::Port<tal::Clipboard>{slow});
    slow->held = "new";
    made.reset();
    scope.run(into(editor->paste(), made));
    QUIETED(loop);
    (void)document.children().remove(*editor);
    slow->release();
    QUIETED(loop);

    SAME(made, std::optional<bool>{false});
    SAME(text->whole(), std::string{"one X"});

    editor = &document.children().add<Editor>(text, std::make_shared<Undo>(), tal::Port<tal::Clipboard>{slow});
    editor->anchor = Place{0, 0};
    editor->cursor = Place{0, 3};
    made.reset();
    scope.run(into(editor->cut(), made));
    QUIETED(loop);
    editor->anchor = Place{0, 4};
    editor->cursor = Place{0, 1};
    slow->release();
    QUIETED(loop);

    SAME(made, std::optional<bool>{false});
    SAME(text->whole(), std::string{"one X"});

    editor->anchor = Place{0, 0};
    editor->cursor = Place{0, 3};
    made.reset();
    scope.run(into(editor->cut(), made));
    QUIETED(loop);
    editor->readonly = true;
    slow->release();
    QUIETED(loop);

    SAME(made, std::optional<bool>{false});
    SAME(text->whole(), std::string{"one X"});

    made.reset();
    scope.run(into(editor->paste(), made));
    QUIETED(loop);

    SAME(made, std::optional<bool>{false});
    OUGHT(slow->gates.empty());

    editor->readonly = false;
    editor->anchor = std::nullopt;
    editor->cursor = Place{0, 0};
    made.reset();
    scope.run(into(editor->paste(), made));
    QUIETED(loop);
    editor->readonly = true;
    slow->release();
    QUIETED(loop);

    SAME(made, std::optional<bool>{false});
    SAME(text->whole(), std::string{"one X"});

    editor->readonly = false;
    made.reset();
    scope.run(into(editor->paste(), made));
    QUIETED(loop);
    slow->release();
    QUIETED(loop);

    SAME(made, std::optional<bool>{true});
    SAME(text->whole(), std::string{"newone X"});
}

// Finding goes on from the selection forth or back, selecting the hit, and does not go round the
// ends; replacing puts the text in place of a selected hit and finds the next; replacing all is
// one step.
TRIAL("Editor: finding and replacing") {
    Written shown{"cat Cat\ncat", {10, 3}};
    Editor& editor = *shown.editor;
    const Comb cat{"cat", Sift{}};

    OUGHT(editor.find(cat));
    OUGHT(editor.selected() == (Range{Place{0, 0}, Place{0, 3}}));
    OUGHT(editor.find(cat));
    OUGHT(editor.selected() == (Range{Place{0, 4}, Place{0, 7}}));
    OUGHT(editor.find(cat));
    OUGHT(editor.selected() == (Range{Place{1, 0}, Place{1, 3}}));
    OUGHT(!editor.find(cat));
    OUGHT(editor.find(cat, true));
    OUGHT(editor.selected() == (Range{Place{0, 4}, Place{0, 7}}));
    OUGHT(*editor.cursor == (Place{0, 4}));
    OUGHT(editor.find(cat, true));
    OUGHT(!editor.find(cat, true));
    OUGHT(!editor.find(Comb{"", Sift{}}));

    editor.line(0);

    OUGHT(!editor.replace(cat, "dog"));
    OUGHT(editor.selected() == (Range{Place{0, 0}, Place{0, 3}}));
    OUGHT(editor.replace(cat, "dog"));
    SAME(shown.text->whole(), std::string{"dog Cat\ncat"});
    OUGHT(editor.selected() == (Range{Place{0, 4}, Place{0, 7}}));
    SAME(editor.replace_all(cat, "x\ny"), std::size_t{2});
    SAME(shown.text->whole(), std::string{"dog x\ny\nx\ny"});
    SAME(*editor.history().undoing(), std::string{"Replace all"});
    OUGHT(editor.undo());
    SAME(shown.text->whole(), std::string{"dog Cat\ncat"});
    SAME(editor.replace_all(Comb{"c(a)t", Sift{true, true, false}}, "u"), std::size_t{1});
    SAME(shown.text->whole(), std::string{"dog Cat\nu"});
    SAME(editor.replace_all(Comb{"zz", Sift{}}, "u"), std::size_t{0});

    editor.anchor = Place{0, 2};
    editor.cursor = Place{0, 7};

    OUGHT(!editor.replace(Comb{"cat", Sift{}}, "z"));

    editor.anchor = Place{0, 0};
    editor.cursor = Place{1, 1};

    OUGHT(!editor.replace(Comb{"d", Sift{}}, "z"));
    SAME(shown.text->whole(), std::string{"dog Cat\nu"});
}

// Two editors of one text and one history: a change in one moves the other's cursor, selection and
// block along without moving its view, and the other undoes it. An editor with a history of its
// own whose steps the text no longer holds undoes nothing.
TRIAL("Editor: two editors of one text") {
    const auto undo = std::make_shared<Undo>();
    Written shown{"abc\ndef", {6, 2}, undo};
    Editor& one = *shown.editor;
    const auto board = std::make_shared<ScriptedClipboard>();
    Editor two{shown.text, undo, tal::Port<tal::Clipboard>{board}};

    two.anchor = Place{0, 1};
    two.cursor = Place{0, 3};
    two.mark();
    two.cursor = Place{1, 2};
    two.mark();
    two.cursor = Place{0, 3};
    shown.scene.type("XY");

    OUGHT(*two.cursor == (Place{0, 5}));
    OUGHT(*two.anchor == std::optional<Place>{Place{0, 3}});
    OUGHT(two.block() == (Range{Place{0, 5}, Place{1, 2}}));

    shown.press(Key{Code::down});
    shown.press(Key{Code::end});
    shown.scene.type("Z");

    OUGHT(two.block() == (Range{Place{0, 5}, Place{1, 2}}));
    OUGHT(two.undo());
    SAME(shown.text->whole(), std::string{"XYabc\ndef"});
    OUGHT(*two.cursor == (Place{1, 3}));
    OUGHT(two.undo());
    SAME(shown.text->whole(), std::string{"abc\ndef"});
    OUGHT(*two.cursor == (Place{0, 0}));
    OUGHT(*one.cursor == (Place{1, 3}));

    Written tall{"0\n1\n2\n3\n4\n5", {4, 2}, undo};
    Editor other{tall.text, std::make_shared<Undo>(), tal::Port<tal::Clipboard>{board}};

    tall.editor->line(5);
    tall.scene.wait(0ms);

    SAME(tall.editor->first(), std::size_t{4});

    other.insert("new\n");
    tall.scene.wait(0ms);

    SAME(tall.editor->first(), std::size_t{4});
    OUGHT(*tall.editor->cursor == (Place{6, 0}));

    tall.editor->line(0);
    tall.press(Key{Code::down, shift});
    OUGHT(tall.editor->selected() == (Range{Place{0, 0}, Place{1, 0}}));
    tall.press(Key{Code::backspace});

    SAME(tall.text->whole(), std::string{"0\n1\n2\n3\n4\n5"});
    OUGHT(other.undo());
    SAME(tall.text->whole(), std::string{"0\n1\n2\n3\n4\n5"});
    OUGHT(*other.cursor == (Place{0, 0}));
}

// The program puts the cursor and the anchor, fitted to the text; a jump goes to a line, the last
// one past it; an insertion takes \r\n as \n; the column counts cells, a tab to its stop and a wide
// character two. A cursor off the view shows no caret.
TRIAL("Editor: the program's cursor, jumps and insertions") {
    Written shown{"\t\xe6\x97\xa5x\nab", {12, 1}};
    Editor& editor = *shown.editor;

    editor.cursor = Place{0, 4};

    SAME(editor.column(), 10);

    editor.tab = 0;

    SAME(editor.column(), 3);

    editor.tab = 8;
    (void)shown.text->insert(Place{0, 0}, "a");
    editor.cursor = Place{0, 2};

    SAME(editor.column(), 8);

    (void)shown.text->erase(Place{0, 0}, Place{0, 1});
    editor.cursor = Place{0, 2};

    OUGHT(*editor.cursor == (Place{0, 1}));

    editor.cursor = Place{9, 9};

    OUGHT(*editor.cursor == (Place{1, 2}));

    editor.anchor = Place{1, 0};

    OUGHT(editor.selected() == (Range{Place{1, 0}, Place{1, 2}}));

    editor.anchor = Place{0, 3};

    OUGHT(*editor.anchor == std::optional<Place>{Place{0, 1}});

    editor.line(0);

    OUGHT(*editor.cursor == (Place{0, 0}));
    OUGHT(!editor.selected().has_value());

    editor.line(9);

    OUGHT(*editor.cursor == (Place{1, 0}));
    OUGHT(shown.caret() == (std::optional<Spot>{Spot{0, 0}}));

    editor.insert("a\r\nb");

    SAME(shown.text->whole(), std::string{"\t\xe6\x97\xa5x\na\nbab"});
    OUGHT(*editor.cursor == (Place{2, 1}));
    SAME(*editor.history().undoing(), std::string{"Insert"});
    OUGHT(shown.caret() == (std::optional<Spot>{Spot{1, 0}}));

    shown.scene.wheel({0, 0}, 1);

    SAME(editor.first(), std::size_t{0});
    OUGHT(!shown.caret().has_value());
}

// A drag going out of the editor stays the editor's: past its top the view goes a row up, and to
// the left of it the cursor goes to the start of the row.
TRIAL("Editor: dragging out of it") {
    tal::Loop loop;
    Scene scene{{10, 5}};
    Document document{scene.time()};
    tal::Column& column = document.children().add<tal::Column>();
    (void)column.children().add<tal::Label>("top");
    tal::Row& row = column.children().add<tal::Row>();
    (void)row.children().add<tal::Label>("ab");
    const auto text = std::make_shared<Text>("0\n1\n2\n3\n4\n5\n6\n7\n8\n9");
    Editor& editor = row.children().add<Editor>(text);

    row.share(tal::Share{1, 0});
    editor.share(tal::Share{1, 0});
    scene.show(document);
    editor.first(3);
    scene.wait(0ms);

    document.mouse(Press::down, MouseButton::left, {3, 2});
    scene.wait(0ms);

    OUGHT(*editor.cursor == (Place{4, 1}));

    document.mouse(Press::move, MouseButton::left, {0, 0});
    scene.wait(0ms);

    OUGHT(*editor.cursor == (Place{2, 0}));
    SAME(editor.first(), std::size_t{2});

    document.mouse(Press::up, MouseButton::left, {0, 0});
    scene.wait(0ms);

    OUGHT(editor.selected() == (Range{Place{2, 0}, Place{4, 1}}));
}

// A frame takes its cells from the editor: a page is the rows inside it, and a press on the frame
// goes to the nearest cell inside.
TRIAL("Editor: framed") {
    Written shown{"0\n1\n2\n3\n4\n5\n6\n7\n8\n9", {6, 5}};
    Editor& editor = *shown.editor;

    shown.document.sheet(tal::StyleSheet::parsed(R"({"rules": [{"select": "editor", "style": {"border": "thin"}}], "glyphs": {}})"));
    shown.scene.wait(0ms);
    shown.press(Key{Code::pagedown});

    OUGHT(*editor.cursor == (Place{3, 0}));
    OUGHT(shown.caret() == (std::optional<Spot>{Spot{1, 3}}));

    shown.mouse(Press::down, {0, 0});
    shown.mouse(Press::up, {0, 0});

    OUGHT(*editor.cursor == (Place{editor.first(), 0}));

    shown.mouse(Press::down, {5, 4});
    shown.mouse(Press::up, {5, 4});

    OUGHT(*editor.cursor == (Place{editor.first() + 2, 1}));
}

// Changes from another editor: the view keeps its top line, the last when the text got shorter,
// and goes no further down than the last row at the bottom; a press before the next frame finds
// the text as it is; a column kept for the up and down keys is taken anew.
TRIAL("Editor: changes from elsewhere") {
    Written shown{"0\n1\n2\n3\n4\n5", {4, 2}};
    Editor& editor = *shown.editor;
    Editor other{shown.text, std::make_shared<Undo>(), tal::Port<tal::Clipboard>{shown.board}};

    editor.line(5);
    shown.scene.wait(0ms);

    SAME(editor.first(), std::size_t{4});

    other.anchor = Place{2, 1};
    other.cursor = Place{5, 1};
    other.insert("");
    shown.scene.wait(0ms);

    SAME(shown.text->whole(), std::string{"0\n1\n2"});
    SAME(editor.first(), std::size_t{1});
    OUGHT(*editor.cursor == (Place{2, 1}));

    editor.line(0);
    shown.scene.wait(0ms);
    other.cursor = Place{0, 0};
    other.insert("\n");
    shown.document.mouse(Press::down, MouseButton::left, {1, 0});
    shown.document.mouse(Press::up, MouseButton::left, {1, 0});
    shown.scene.wait(0ms);

    OUGHT(*editor.cursor == (Place{0, 0}));

    Written wrapped{"abcdefghijklmnop\n0123456789abcdef", {4, 2}};
    Editor gone{wrapped.text, std::make_shared<Undo>(), tal::Port<tal::Clipboard>{wrapped.board}};

    wrapped.editor->wrap = true;
    wrapped.editor->cursor = Place{1, 8};
    wrapped.scene.wait(0ms);

    SAME(wrapped.rows(), std::string{"4567|89ab"});

    gone.anchor = Place{0, 16};
    gone.cursor = Place{1, 16};
    gone.insert("");
    wrapped.scene.wait(0ms);

    SAME(wrapped.rows(), std::string{"abcd|efgh"});

    Written lines{"abc\nabcdef\nabcdef", {8, 3}};
    Editor elsewhere{lines.text, std::make_shared<Undo>(), tal::Port<tal::Clipboard>{lines.board}};

    lines.press(Key{Code::right}, 2);
    lines.press(Key{Code::down});

    OUGHT(*lines.editor->cursor == (Place{1, 2}));

    elsewhere.cursor = Place{1, 0};
    elsewhere.insert("XX");
    lines.press(Key{Code::down});

    OUGHT(*lines.editor->cursor == (Place{2, 4}));
}

// One history of two texts: undoing a step of the other text leaves the cursor where it is.
TRIAL("Editor: one history of two texts") {
    const auto undo = std::make_shared<Undo>();
    Written one{"", {6, 2}, undo};
    Written two{"", {6, 2}, undo};

    one.scene.type("a");
    one.press(Key{Code::home});
    two.scene.type("b");

    OUGHT(one.editor->undo());
    SAME(two.text->whole(), std::string{});
    SAME(one.text->whole(), std::string{"a"});
    OUGHT(*one.editor->cursor == (Place{0, 0}));
}

// With wrapping toggled or the tab changed the column kept for the up and down keys is taken anew.
TRIAL("Editor: the column kept, taken anew") {
    Written shown{"abcdefghij\nxy\nabcdefgh", {4, 6}};
    Editor& editor = *shown.editor;

    editor.cursor = Place{0, 7};
    shown.press(Key{Code::down});

    OUGHT(*editor.cursor == (Place{1, 2}));

    editor.wrap = true;
    shown.scene.wait(0ms);
    shown.press(Key{Code::down});

    OUGHT(*editor.cursor == (Place{2, 2}));

    Written tabbed{"\tx\n\ty", {12, 3}};

    tabbed.press(Key{Code::right});
    tabbed.press(Key{Code::down});

    OUGHT(*tabbed.editor->cursor == (Place{1, 1}));

    tabbed.editor->tab = 2;
    tabbed.scene.wait(0ms);
    tabbed.press(Key{Code::up});

    OUGHT(*tabbed.editor->cursor == (Place{0, 1}));
}

// Editing a long line lays it out as anew: with wrapping the rows remembered in it go, and the
// view keeps the row it showed at the top, found again; without wrapping the columns remembered
// go. A wide character put at the start moves every row and column after it.
TRIAL("Editor: a long line edited") {
    const std::string digits = [] {
        std::string made;

        for (int one = 0; one < 2000; ++one) made += static_cast<char>('0' + one % 10);

        return made;
    }();

    for (const bool wrapping : {true, false}) {
        Written shown{digits + "\nend", {10, 4}};
        Written fresh{"\xe6\x97\xa5" + digits + "\nend", {10, 4}};

        shown.editor->wrap = wrapping;
        fresh.editor->wrap = wrapping;
        shown.scene.wait(0ms);
        fresh.scene.wait(0ms);
        shown.press(Key{Code::end});
        shown.press(Key{Code::home});
        shown.scene.type("\xe6\x97\xa5");
        shown.press(Key{Code::end});
        fresh.press(Key{Code::end});

        OUGHT(*shown.editor->cursor == *fresh.editor->cursor);
        SAME(shown.editor->first(), fresh.editor->first());
        SAME(*shown.editor->shift, *fresh.editor->shift);
        SAME(shown.rows(), fresh.rows());
        OUGHT(shown.caret() == fresh.caret());
    }

    Written top{digits, {4, 4}};
    Editor other{top.text, std::make_shared<Undo>(), tal::Port<tal::Clipboard>{top.board}};

    top.editor->wrap = true;
    top.scene.wait(0ms);
    top.scene.wheel({0, 0}, -1);
    top.mouse(Press::down, {0, 0});
    top.mouse(Press::up, {0, 0});

    OUGHT(*top.editor->cursor == (Place{0, 12}));

    top.scene.type("x");

    SAME(top.scene.row(0), std::string{"x234"});
    OUGHT(top.caret() == (std::optional<Spot>{Spot{1, 0}}));

    other.insert("\xe6\x97\xa5");
    top.scene.wait(0ms);

    SAME(top.scene.row(0), std::string{"6789"});
}

// Which steps join: typing goes on where it ended without a space, a tab or a line end, and not
// over a selection; erasing goes on next to where it was, not across a line end; nothing joins a
// step of another kind or of another text, nor a step at another place. A step whose piece no
// longer finds its text stops there, and the cursor stays where it was, even when the step was
// undone whole once before.
TRIAL("Editor: steps that join and steps that do not") {
    Written shown{"XY", {12, 3}};
    Editor& editor = *shown.editor;

    shown.scene.type("ab");

    OUGHT(editor.find(Comb{"XY", Sift{}}));

    shown.scene.type("c");

    OUGHT(editor.undo());
    SAME(shown.text->whole(), std::string{"abXY"});
    OUGHT(editor.undo());
    SAME(shown.text->whole(), std::string{"XY"});

    shown.scene.type("a\tb");

    OUGHT(editor.undo());
    SAME(shown.text->whole(), std::string{"aXY"});

    shown.scene.type("bc");
    shown.press(Key{Code::backspace});
    shown.scene.type("x");

    OUGHT(editor.undo());
    SAME(shown.text->whole(), std::string{"abXY"});

    Written lines{"ab\ncd", {6, 3}};

    lines.press(Key{Code::down});
    lines.press(Key{Code::backspace}, 2);

    SAME(lines.text->whole(), std::string{"acd"});
    OUGHT(lines.editor->undo());
    SAME(lines.text->whole(), std::string{"abcd"});

    lines.press(Key{Code::enter});
    lines.press(Key{Code::left});
    lines.press(Key{Code::remove}, 2);

    SAME(lines.text->whole(), std::string{"abd"});
    OUGHT(lines.editor->undo());
    SAME(lines.text->whole(), std::string{"abcd"});

    const auto undo = std::make_shared<Undo>();
    const auto text = std::make_shared<Text>("12\n34");
    Written one{text, {6, 3}, undo};
    Written two{text, {6, 3}, undo};

    two.editor->cursor = Place{1, 0};
    one.scene.type("a");
    two.scene.type("b");

    OUGHT(one.editor->undo());
    SAME(text->whole(), std::string{"a12\n34"});
    OUGHT(one.editor->undo());

    one.press(Key{Code::end});
    two.press(Key{Code::end});
    one.press(Key{Code::backspace});
    two.press(Key{Code::backspace});

    OUGHT(one.editor->undo());
    SAME(text->whole(), std::string{"1\n34"});
    OUGHT(one.editor->undo());

    one.press(Key{Code::home});
    two.press(Key{Code::home});
    one.press(Key{Code::remove});
    two.press(Key{Code::remove});

    OUGHT(one.editor->undo());
    SAME(text->whole(), std::string{"2\n34"});

    const auto shared = std::make_shared<Undo>();
    Written left{"", {6, 2}, shared};
    Written right{"q", {6, 2}, shared};

    right.press(Key{Code::end});
    left.scene.type("a");
    right.scene.type("b");

    OUGHT(left.editor->undo());
    SAME(right.text->whole(), std::string{"q"});
    SAME(left.text->whole(), std::string{"a"});

    Written stale{"a a", {8, 2}};
    Editor away{stale.text, std::make_shared<Undo>(), tal::Port<tal::Clipboard>{stale.board}};

    stale.press(Key{Code::end});

    SAME(stale.editor->replace_all(Comb{"a", Sift{}}, "xyz"), std::size_t{2});
    SAME(stale.text->whole(), std::string{"xyz xyz"});
    OUGHT(*stale.editor->cursor == (Place{0, 7}));
    OUGHT(stale.editor->undo());
    SAME(stale.editor->replace_all(Comb{"a", Sift{}}, "b"), std::size_t{2});
    OUGHT(stale.editor->undo());
    OUGHT(stale.editor->redo());

    away.anchor = Place{0, 0};
    away.cursor = Place{0, 1};
    away.insert("Q");
    stale.press(Key{Code::home});

    OUGHT(stale.editor->undo());
    SAME(stale.text->whole(), std::string{"Q b"});
    OUGHT(*stale.editor->cursor == (Place{0, 0}));
}

// A press takes the focus from another node.
TRIAL("Editor: a press takes the focus") {
    tal::Loop loop;
    Scene scene{{10, 3}};
    Document document{scene.time()};
    tal::Column& column = document.children().add<tal::Column>();
    tal::Field& field = column.children().add<tal::Field>("f");
    Editor& editor = column.children().add<Editor>(std::make_shared<Text>("abc"));

    editor.share(tal::Share{1, 0});
    scene.show(document);
    field.focus();
    scene.wait(0ms);

    OUGHT(field.focused());

    document.mouse(Press::down, MouseButton::left, {1, 1});
    scene.wait(0ms);

    OUGHT(editor.focused());
    OUGHT(*editor.cursor == (Place{0, 1}));
}

// A change from elsewhere lights the selection where it went.
TRIAL("Editor: the selection lit after a change from elsewhere") {
    Written shown{"abc", {6, 1}};
    Editor other{shown.text, std::make_shared<Undo>(), tal::Port<tal::Clipboard>{shown.board}};

    shown.press(Key{Code::right});
    shown.press(Key{Code::right, shift});

    SAME(shown.lit(0), std::string{".#...."});

    other.insert("xy");
    shown.scene.wait(0ms);

    SAME(shown.lit(0), std::string{"...#.."});
}

// A wide character that does not fit ends a wrapped row short: the up key lands on its last
// character, not past it, and a selection lights no cell after it.
TRIAL("Editor: a wrapped row cut short") {
    Written shown{"abc\xe6\x97\xa5\nxyzw", {4, 4}};
    Editor& editor = *shown.editor;

    editor.wrap = true;
    shown.scene.wait(0ms);

    SAME(shown.scene.row(1), std::string{"\xe6\x97\xa5  "});

    editor.cursor = Place{1, 3};
    shown.press(Key{Code::up});

    OUGHT(*editor.cursor == (Place{0, 6}));

    shown.press(Key{Code::up});

    OUGHT(*editor.cursor == (Place{0, 2}));

    editor.cursor = Place{0, 0};
    shown.press(Key{Code::end, shift});

    SAME(shown.lit(0), std::string{"###."});
}
