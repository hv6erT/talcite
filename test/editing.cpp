#include "tal/testing.hpp"
#include "waiting.hpp"

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <random>
#include <source_location>
#include <stdexcept>
#include <string>
#include <vector>

#include "faking.hpp"
#include "talcite/bytes.hpp"
#include "talcite/editor.hpp"
#include "talcite/encoding.hpp"
#include "talcite/event.hpp"
#include "talcite/feed.hpp"
#include "talcite/loop.hpp"
#include "talcite/promise.hpp"
#include "talcite/root.hpp"

using namespace talcite;

namespace {

class Sheet : public ui::Root {};


// Reads one row of a view as text, with trailing spaces removed.
std::string said(const Snapshot& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter == 0) continue;

        made += spelled(letter);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}


// A document of the given number of lines, "l0" to "l<n-1>", without a final newline.
std::string many(int lines) {
    std::string made;

    for (int one = 0; one < lines; ++one) made += "l" + std::to_string(one) + "\n";

    made.pop_back();

    return made;
}

// An editor on a fake clipboard, so that no trial reaches the system one; on Windows that would
// overwrite what the user has copied. It holds the given text and stands in the sheet.
std::shared_ptr<ui::Editor> shown(Sheet& sheet, const std::string& what,
                                  const std::shared_ptr<faking::Clipboard>& kept = faking::clipboard()) {
    const auto one = std::make_shared<ui::Editor>(Seam<Clipboard>{kept});

    one->hold(bytes(what));

    sheet.children().add(one);

    return one;
}

}

// --- What the editor knows at the start ----------------------------------------------------------

TRIAL("Editor: an empty one holds nothing") {
    const ui::Editor one;

    OUGHT(!one.held());
    OUGHT(!one.dirty());
    OUGHT(!one.readonly());
    SAME(one.cursor(), std::size_t{0});
}

TRIAL("Editor: once given text, it knows the lines and stands at the start") {
    Sheet sheet;

    const auto one = shown(sheet, "a\nbb\nccc");

    OUGHT(one->held());
    OUGHT(!one->dirty());
    SAME(*one->lines(), std::size_t{3});
    SAME(*one->whole(), std::size_t{8});
    SAME(one->cursor(), std::size_t{0});
    SAME(one->text(), std::string{"a\nbb\nccc"});
}

// --- Changes -------------------------------------------------------------------------------------

// The wheel under the mouse moves the view of the editor three rows a notch, as in the viewer, and
// leaves the cursor where it stood.
TRIAL("Editor: the wheel moves the view and not the cursor") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    ui::WheelEvent down{{1, 1}, -2};

    OUGHT(sheet.dispatch(down));
    SAME(*one->first(), std::size_t{6});
    SAME(said(sheet.render({{0, 0}, {40, 10}}), 0), std::string{"l6"});
    SAME(one->cursor(), std::size_t{0});

    ui::WheelEvent up{{1, 1}, 1};

    OUGHT(sheet.dispatch(up));
    SAME(*one->first(), std::size_t{3});
}

TRIAL("Editor: typing a character moves the cursor and makes the document dirty") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->cursor(1);
    one->insert("X");

    SAME(one->text(), std::string{"aXbc"});
    SAME(one->cursor(), std::size_t{2});
    OUGHT(one->dirty());
}

// The editor's own keys, typing among them, move the cursor by hand; a call from the program does
// not, typing included.
TRIAL("Editor: a move says whether it came by hand") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->focus();

    (void)sheet.render({{0, 0}, {40, 10}});

    std::vector<bool> heard;

    const ui::Hook heard_scroll = sheet.on<ui::ScrollEvent>([&heard](ui::ScrollEvent& what) {
        heard.push_back(what.by_hand());

        return false;
    });

    ui::KeyEvent right{ui::Code::right};
    ui::KeyEvent letter{ui::Key{U'x'}};
    ui::KeyEvent back{ui::Code::backspace};

    OUGHT(sheet.dispatch(right));
    OUGHT(sheet.dispatch(letter));
    OUGHT(sheet.dispatch(back));

    SAME(heard, (std::vector<bool>{true, true, true}));

    heard.clear();

    one->cursor(5);
    one->insert("y");
    one->back_erase(1);

    SAME(heard, (std::vector<bool>{false, false, false}));
}

// Enter splits a line, so the number of lines grows, and that is the adjustment the spool gets from
// `changed` instead of counting the document again.
TRIAL("Editor: enter adds a line") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    SAME(*one->lines(), std::size_t{1});

    one->cursor(3);
    one->insert("\n");

    SAME(*one->lines(), std::size_t{2});
    SAME(one->text(), std::string{"abc\ndef"});
}

TRIAL("Editor: deleting backward removes the character before the cursor") {
    Sheet sheet;

    const auto one = shown(sheet, "abcd");

    one->cursor(3);
    one->back_erase(1);

    SAME(one->text(), std::string{"abd"});
    SAME(one->cursor(), std::size_t{2});
}

TRIAL("Editor: deleting forward removes the character after the cursor") {
    Sheet sheet;

    const auto one = shown(sheet, "abcd");

    one->cursor(1);
    one->front_erase(1);

    SAME(one->text(), std::string{"acd"});
    SAME(one->cursor(), std::size_t{1});
}

TRIAL("Editor: deleting a newline joins the lines") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    SAME(*one->lines(), std::size_t{2});

    one->cursor(4);
    one->back_erase(1);

    SAME(*one->lines(), std::size_t{1});
    SAME(one->text(), std::string{"abcdef"});
}

TRIAL("Editor: read-only accepts no changes") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->readonly(true);
    one->insert("X");
    one->back_erase(1);

    SAME(one->text(), std::string{"abc"});
    OUGHT(!one->dirty());
}

// --- Selection -----------------------------------------------------------------------------------

TRIAL("Editor: the selection returns its text") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    OUGHT(one->selection().empty());

    one->selection().set(2, 5);

    OUGHT(!one->selection().empty());
    SAME(one->selection().text(), std::string{"cde"});
    SAME(one->selection().anchor(), std::size_t{2});
    SAME(one->cursor(), std::size_t{5});
}

TRIAL("Editor: typing over a selection replaces it") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->selection().set(2, 5);
    one->insert("X");

    SAME(one->text(), std::string{"abXf"});
    OUGHT(one->selection().empty());
}

TRIAL("Editor: deleting on a selection removes all of it") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->selection().set(1, 4);
    one->back_erase(1);

    SAME(one->text(), std::string{"aef"});
    SAME(one->cursor(), std::size_t{1});
}

// The selection is a subobject: the anchor and the cursor under one roof. The parameters of `set`
// are named for what they are, so a backward selection is simply an anchor after the cursor, while
// `from` and `upto` return them in order anyway.
TRIAL("Selection: backward, extending and clearing") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->selection().set(5, 2);

    SAME(one->selection().anchor(), std::size_t{5});
    SAME(one->cursor(), std::size_t{2});
    SAME(one->selection().from(), std::size_t{2});
    SAME(one->selection().upto(), std::size_t{5});
    SAME(one->selection().text(), std::string{"cde"});

    one->selection().extend(0);

    SAME(one->selection().anchor(), std::size_t{5});
    SAME(one->cursor(), std::size_t{0});
    SAME(one->selection().text(), std::string{"abcde"});

    one->selection().clear();

    OUGHT(one->selection().empty());
    SAME(one->cursor(), std::size_t{0});
    SAME(one->selection().anchor(), std::size_t{0});
    SAME(one->selection().text(), std::string{});

    // Outside the document it clamps, and a cursor set directly clears the selection.
    one->selection().set(99, 1);

    SAME(one->selection().anchor(), std::size_t{6});

    one->cursor(3);

    OUGHT(one->selection().empty());
}

TRIAL("Selection: erasing removes the selection in one change, and an empty one does nothing") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    OUGHT(!one->selection().erase());
    OUGHT(!one->undoable());

    one->selection().set(4, 1);

    OUGHT(one->selection().erase());
    SAME(one->text(), std::string{"aef"});
    SAME(one->cursor(), std::size_t{1});
    OUGHT(one->selection().empty());

    one->undo();

    SAME(one->text(), std::string{"abcdef"});

    one->readonly(true);
    one->selection().set(0, 2);

    OUGHT(!one->selection().erase());
    SAME(one->text(), std::string{"abcdef"});
}

// Delete with a selection deletes the selection, not the characters after the cursor, just like
// Backspace.
TRIAL("Editor: deleting forward on a selection removes all of it") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->selection().set(1, 4);
    one->front_erase(1);

    SAME(one->text(), std::string{"aef"});
    SAME(one->cursor(), std::size_t{1});
}

// The whole document directly, without touching the cursor and the selection.
TRIAL("Editor: text returns the whole document and touches nothing") {
    Sheet sheet;

    const auto empty = std::make_shared<ui::Editor>();

    SAME(empty->text(), std::string{});

    const auto one = shown(sheet, "ab\ncd");

    one->selection().set(1, 3);

    SAME(one->text(), std::string{"ab\ncd"});
    SAME(one->selection().anchor(), std::size_t{1});
    SAME(one->cursor(), std::size_t{3});
}

// The selection is drawn with marks, one per line, but only for what is visible.
TRIAL("Editor: the selection is visible on the screen") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->selection().set(2, 5);

    const Snapshot made = sheet.render({{0, 0}, {20, 2}});

    OUGHT(!made.at({1, 0}).style.reverse);
    OUGHT(made.at({2, 0}).style.reverse);
    OUGHT(made.at({4, 0}).style.reverse);
    OUGHT(!made.at({5, 0}).style.reverse);
}

TRIAL("Editor: a selection across two lines gives a mark on each") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->selection().set(1, 6);

    const Snapshot made = sheet.render({{0, 0}, {20, 3}});

    OUGHT(!made.at({0, 0}).style.reverse);
    OUGHT(made.at({1, 0}).style.reverse);
    OUGHT(made.at({2, 0}).style.reverse);
    OUGHT(made.at({0, 1}).style.reverse);
    OUGHT(made.at({1, 1}).style.reverse);
    OUGHT(!made.at({2, 1}).style.reverse);
}

// --- Going to a line -----------------------------------------------------------------------------

// `line(n)` puts the cursor at the start of line n (counted from zero) and clears the selection;
// past the end, on the last line; the view follows the cursor. It pairs with `line()`, so what one
// returns the other accepts.
TRIAL("Editor: line(n) jumps to the start of a line, past the end to the last, and clears the selection") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {20, 5}});

    one->selection().set(1, 4);
    one->line(42);

    SAME(one->line(), std::size_t{42});
    SAME(one->column(), std::size_t{0});
    OUGHT(one->selection().empty());

    const Snapshot made = sheet.render({{0, 0}, {20, 5}});

    bool seen = false;

    for (int y = 0; y < 5; ++y)
        if (said(made, y) == "l42") seen = true;

    OUGHT(seen);

    one->line(9999);

    SAME(one->line(), std::size_t{99});
    SAME(one->column(), std::size_t{0});

    one->line(0);

    SAME(one->cursor(), std::size_t{0});

    // An empty document: zero and nothing more.
    const auto empty = std::make_shared<ui::Editor>();

    empty->hold(bytes(""));
    empty->line(5);

    SAME(empty->cursor(), std::size_t{0});
}

// --- The mcedit block ----------------------------------------------------------------------------

// F3 starts a block at the cursor, movement extends it, a second F3 ends it, and the block stays
// although the cursor moves on. A third F3 removes the old one and starts a new one. A backward
// block is ordered by min/max; one of zero length disappears.
TRIAL("Editor: mark starts, ends and starts again; the block stays after the cursor moves") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");
    ui::Editor::Block& block = one->block();

    OUGHT(block.empty());
    OUGHT(!block.pending());
    SAME(block.text(), std::string{});

    one->cursor(2);
    block.mark();

    OUGHT(!block.empty());
    OUGHT(block.pending());
    SAME(block.from(), std::size_t{2});
    SAME(block.upto(), std::size_t{2});

    // Extending: the end is the cursor, also with a key, without Shift.
    one->focus();
    (void)sheet.render({{0, 0}, {20, 2}});

    ui::KeyEvent right{ui::Code::right};

    OUGHT(sheet.dispatch(right));
    OUGHT(sheet.dispatch(right));
    OUGHT(sheet.dispatch(right));

    SAME(block.upto(), std::size_t{5});
    SAME(block.text(), std::string{"cde"});

    block.mark();

    OUGHT(!block.empty());
    OUGHT(!block.pending());

    one->cursor(0);

    SAME(block.from(), std::size_t{2});
    SAME(block.upto(), std::size_t{5});
    SAME(block.text(), std::string{"cde"});

    // A third F3: a new block at the cursor, the old one disappears.
    block.mark();

    OUGHT(block.pending());
    SAME(block.from(), std::size_t{0});

    // Backward: the cursor before the start, and the text is in order anyway.
    block.clear();
    one->cursor(5);
    block.mark();
    one->cursor(2);

    SAME(block.text(), std::string{"cde"});

    block.mark();

    SAME(block.from(), std::size_t{2});
    SAME(block.upto(), std::size_t{5});

    // Zero length: it disappears.
    block.mark();
    block.mark();

    OUGHT(block.empty());
}

// F5: the block's content goes in at the cursor and the block stays; when it lay after the cursor,
// it moves by its own length. One undo, one redo, and the block comes back together with the text.
TRIAL("Editor: copy inserts the block at the cursor and keeps it, undo goes back in one step") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");
    ui::Editor::Block& block = one->block();

    OUGHT(!block.copy());

    one->cursor(2);
    block.mark();
    one->cursor(5);
    block.mark();

    one->cursor(0);

    OUGHT(block.copy());
    SAME(one->text(), std::string{"cdeabcdef"});
    SAME(one->cursor(), std::size_t{3});
    SAME(block.from(), std::size_t{5});
    SAME(block.upto(), std::size_t{8});
    SAME(block.text(), std::string{"cde"});

    one->undo();

    SAME(one->text(), std::string{"abcdef"});
    SAME(block.from(), std::size_t{2});
    SAME(block.upto(), std::size_t{5});
    OUGHT(!one->undoable());

    one->redo();

    SAME(one->text(), std::string{"cdeabcdef"});
    SAME(block.from(), std::size_t{5});

    // After the block: the block does not move.
    one->undo();
    one->cursor(6);

    OUGHT(block.copy());
    SAME(one->text(), std::string{"abcdefcde"});
    SAME(block.from(), std::size_t{2});
    SAME(block.upto(), std::size_t{5});

    // While extending, F5 first ends the block.
    one->undo();
    one->cursor(0);
    block.mark();
    one->cursor(2);

    OUGHT(block.copy());
    OUGHT(!block.pending());

    // The copy's journal entry is sealed: a character typed right after it is undone separately.
    // Nothing that moves the cursor may stand between the copy and the character, because moving
    // the cursor closes the undo group by itself and the trial would then not ask about the seal.
    // `text()` does not move the cursor.
    one->focus();
    (void)sheet.render({{0, 0}, {20, 2}});

    ui::KeyEvent letter{U'X'};

    OUGHT(sheet.dispatch(letter));
    SAME(one->text(), std::string{"ababXcdef"});

    one->undo();

    SAME(one->text(), std::string{"ababcdef"});
}

// F6: the block goes out and comes in where the cursor stood after the cut; before the block the
// target is the same, after the block it moves back by the length. In the block and at its edges,
// nothing. Two journal entries bound into one.
TRIAL("Editor: move carries the block before and after the cursor, refuses inside the block, undo in one step") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");
    ui::Editor::Block& block = one->block();

    OUGHT(!block.move());

    one->cursor(2);
    block.mark();
    one->cursor(5);
    block.mark();

    // Before the block.
    one->cursor(0);

    OUGHT(block.move());
    SAME(one->text(), std::string{"cdeabf"});
    SAME(block.from(), std::size_t{0});
    SAME(block.upto(), std::size_t{3});
    SAME(one->cursor(), std::size_t{3});

    one->undo();

    SAME(one->text(), std::string{"abcdef"});
    SAME(block.from(), std::size_t{2});
    SAME(block.upto(), std::size_t{5});
    OUGHT(!one->undoable());

    // After the block: to the end.
    one->cursor(6);

    OUGHT(block.move());
    SAME(one->text(), std::string{"abfcde"});
    SAME(block.from(), std::size_t{3});
    SAME(block.upto(), std::size_t{6});
    SAME(one->cursor(), std::size_t{6});

    one->undo();

    SAME(one->text(), std::string{"abcdef"});

    // Redo restores both the text and the block.
    one->redo();

    SAME(one->text(), std::string{"abfcde"});
    SAME(block.from(), std::size_t{3});

    one->undo();

    // In the block and at both edges: nothing.
    for (const std::size_t at : {std::size_t{2}, std::size_t{3}, std::size_t{5}}) {
        one->cursor(at);

        OUGHT(!block.move());
        SAME(one->text(), std::string{"abcdef"});
        SAME(block.from(), std::size_t{2});
    }
}

// F8: the block disappears from the document and is removed itself. A cursor after the block moves
// back, one inside the block stands at its start, one before it stays. Undo restores the text and
// the block.
TRIAL("Editor: erase deletes the block and removes it, the cursor by position, undo restores both") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");
    ui::Editor::Block& block = one->block();

    OUGHT(!block.erase());

    one->cursor(2);
    block.mark();
    one->cursor(5);
    block.mark();

    one->cursor(6);

    OUGHT(block.erase());
    SAME(one->text(), std::string{"abf"});
    SAME(one->cursor(), std::size_t{3});
    OUGHT(block.empty());

    one->undo();

    SAME(one->text(), std::string{"abcdef"});
    OUGHT(!block.empty());
    SAME(block.from(), std::size_t{2});
    SAME(block.upto(), std::size_t{5});

    one->cursor(3);

    OUGHT(block.erase());
    SAME(one->cursor(), std::size_t{2});

    one->undo();
    one->cursor(0);

    OUGHT(block.erase());
    SAME(one->cursor(), std::size_t{0});
    SAME(one->text(), std::string{"abf"});
}

// The block follows the document: a change before it shifts it, one after it does nothing, one
// inside lengthens or trims it, and cutting the whole of it removes it.
TRIAL("Editor: the block shifts and trims with changes to the document") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");
    ui::Editor::Block& block = one->block();

    one->cursor(2);
    block.mark();
    one->cursor(5);
    block.mark();

    // Before: a shift.
    one->cursor(0);
    one->insert("XY");

    SAME(one->text(), std::string{"XYabcdef"});
    SAME(block.from(), std::size_t{4});
    SAME(block.upto(), std::size_t{7});
    SAME(block.text(), std::string{"cde"});

    // After: nothing.
    one->cursor(8);
    one->back_erase(1);

    SAME(one->text(), std::string{"XYabcde"});
    SAME(block.from(), std::size_t{4});
    SAME(block.upto(), std::size_t{7});

    // Exactly at the start: the text goes before the block; exactly at the end: it does not enter
    // the block.
    one->cursor(4);
    one->insert("-");

    SAME(block.text(), std::string{"cde"});
    SAME(block.from(), std::size_t{5});

    one->cursor(8);
    one->insert("+");

    SAME(block.text(), std::string{"cde"});
    SAME(one->text(), std::string{"XYab-cde+"});

    // Inside: it lengthens.
    one->cursor(6);
    one->insert("Z");

    SAME(block.text(), std::string{"cZde"});

    // Overlapping the start: trims to what is left.
    one->selection().set(4, 7);
    one->selection().erase();

    SAME(one->text(), std::string{"XYabde+"});
    SAME(block.text(), std::string{"de"});
    SAME(block.from(), std::size_t{4});
    SAME(block.upto(), std::size_t{6});

    // The whole block cut: it disappears.
    one->selection().set(3, 7);
    one->selection().erase();

    OUGHT(block.empty());

    // Undo restores it.
    one->undo();

    OUGHT(!block.empty());
    SAME(block.text(), std::string{"de"});

    // A replacement overlapping the start puts the start after the inserted text; one overlapping
    // the end trims the end to the place of the change, so the inserted text enters the block from
    // neither side.
    one->hold(bytes("abcdef"));
    one->cursor(2);
    block.mark();
    one->cursor(5);
    block.mark();

    one->selection().set(1, 4);
    one->insert("QQ");

    SAME(one->text(), std::string{"aQQef"});
    SAME(block.text(), std::string{"e"});
    SAME(block.from(), std::size_t{3});

    one->hold(bytes("abcdef"));
    one->cursor(2);
    block.mark();
    one->cursor(5);
    block.mark();

    one->selection().set(4, 6);
    one->insert("QQ");

    SAME(one->text(), std::string{"abcdQQ"});
    SAME(block.text(), std::string{"cd"});
    SAME(block.upto(), std::size_t{4});

    // While extending, the start also follows a change before it.
    one->hold(bytes("abcdef"));
    one->cursor(2);
    block.mark();
    one->cursor(0);
    one->insert("XY");

    OUGHT(block.pending());
    SAME(block.from(), std::size_t{4});
    SAME(block.text(), std::string{"ab"});
}

TRIAL("Editor: the block is visible on the screen, also while extending; readonly and hold") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");
    ui::Editor::Block& block = one->block();

    one->cursor(2);
    block.mark();
    one->cursor(5);
    block.mark();
    one->cursor(0);

    Snapshot made = sheet.render({{0, 0}, {20, 2}});

    OUGHT(!made.at({1, 0}).style.reverse);
    OUGHT(made.at({2, 0}).style.reverse);
    OUGHT(made.at({4, 0}).style.reverse);
    OUGHT(!made.at({5, 0}).style.reverse);

    // While extending: it grows with the cursor.
    block.clear();

    made = sheet.render({{0, 0}, {20, 2}});

    OUGHT(!made.at({2, 0}).style.reverse);

    one->cursor(1);
    block.mark();
    one->cursor(4);

    made = sheet.render({{0, 0}, {20, 2}});

    OUGHT(!made.at({0, 0}).style.reverse);
    OUGHT(made.at({1, 0}).style.reverse);
    OUGHT(made.at({3, 0}).style.reverse);
    OUGHT(!made.at({4, 0}).style.reverse);

    block.mark();

    // Read-only: the actions refuse, but a block can be set.
    one->readonly(true);

    OUGHT(!block.copy());
    OUGHT(!block.move());
    OUGHT(!block.erase());
    OUGHT(!block.empty());
    SAME(one->text(), std::string{"abcdef"});

    one->readonly(false);

    // A new document removes the block.
    one->hold(bytes("xyz"));

    OUGHT(block.empty());
    OUGHT(!block.pending());
}

// --- The cursor on the screen --------------------------------------------------------------------

TRIAL("Editor: the cursor stands where the byte points") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->cursor(5);

    (void)sheet.render({{0, 0}, {20, 3}});

    const std::optional<ui::Spot> at = one->caret();

    OUGHT(at.has_value());
    SAME(at->x, 1);
    SAME(at->y, 1);
}

TRIAL("Editor: a cursor after the last character of a line also has a place") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->cursor(3);

    (void)sheet.render({{0, 0}, {20, 3}});

    const std::optional<ui::Spot> at = one->caret();

    OUGHT(at.has_value());
    SAME(at->x, 3);
    SAME(at->y, 0);
}

// --- Keys ----------------------------------------------------------------------------------------

TRIAL("Editor: a letter goes into the document") {
    Sheet sheet;

    const auto one = shown(sheet, "ab");

    one->focus();

    ui::KeyEvent what{U'X'};

    OUGHT(sheet.dispatch(what));
    SAME(one->text(), std::string{"Xab"});
}

TRIAL("Editor: the arrows move by characters and by lines") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->focus();

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::KeyEvent right{ui::Code::right};

    OUGHT(sheet.dispatch(right));
    SAME(one->cursor(), std::size_t{1});

    ui::KeyEvent down{ui::Code::down};

    OUGHT(sheet.dispatch(down));
    SAME(one->cursor(), std::size_t{5});

    ui::KeyEvent left{ui::Code::left};

    OUGHT(sheet.dispatch(left));
    SAME(one->cursor(), std::size_t{4});
}

// A multibyte character is deleted as a whole; otherwise Backspace would cut it in half and leave
// garbage that can no longer be typed back.
TRIAL("Editor: deleting removes a whole character, not one byte") {
    Sheet sheet;

    const auto one = shown(sheet, "a\xc4\x85"
                                  "b");

    one->focus();

    one->cursor(3);

    ui::KeyEvent back{ui::Code::backspace};

    OUGHT(sheet.dispatch(back));
    SAME(one->text(), std::string{"ab"});
    SAME(one->cursor(), std::size_t{1});
}

// --- Undo ----------------------------------------------------------------------------------------

TRIAL("Editor: a fresh document has nothing to undo") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    OUGHT(!one->undoable());
    OUGHT(!one->redoable());
    OUGHT(!one->dirty());

    one->undo();
    one->redo();

    SAME(one->text(), std::string{"abc"});
}

TRIAL("Editor: undo returns the document from before the change, redo restores it") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->cursor(1);
    one->insert("X");

    SAME(one->text(), std::string{"aXbc"});
    OUGHT(one->undoable());
    OUGHT(!one->redoable());

    one->undo();

    SAME(one->text(), std::string{"abc"});
    OUGHT(!one->undoable());
    OUGHT(one->redoable());

    one->redo();

    SAME(one->text(), std::string{"aXbc"});
    OUGHT(one->undoable());
    OUGHT(!one->redoable());
}

// A typed word is gathered into one entry; otherwise undo would go letter by letter, and five
// thousand characters would make five thousand steps.
TRIAL("Editor: a typed word is undone as a whole") {
    Sheet sheet;

    const auto one = shown(sheet, "");

    one->insert("a");
    one->insert("b");
    one->insert("c");

    SAME(one->text(), std::string{"abc"});

    one->undo();

    SAME(one->text(), std::string{});
    OUGHT(!one->undoable());
}

TRIAL("Editor: a space ends a word, so it is undone separately") {
    Sheet sheet;

    const auto one = shown(sheet, "");

    for (const char sign : std::string{"ala "}) one->insert(std::string{sign});
    for (const char sign : std::string{"ma"}) one->insert(std::string{sign});

    SAME(one->text(), std::string{"ala ma"});

    one->undo();

    SAME(one->text(), std::string{"ala "});

    one->undo();

    SAME(one->text(), std::string{});
}

TRIAL("Editor: moving the cursor closes the word") {
    Sheet sheet;

    const auto one = shown(sheet, "xy");

    one->cursor(2);
    one->insert("a");
    one->insert("b");

    one->cursor(0);
    one->insert("Z");

    SAME(one->text(), std::string{"Zxyab"});

    one->undo();

    SAME(one->text(), std::string{"xyab"});

    one->undo();

    SAME(one->text(), std::string{"xy"});
}

TRIAL("Editor: enter is a separate step") {
    Sheet sheet;

    const auto one = shown(sheet, "");

    one->insert("a");
    one->insert("\n");
    one->insert("b");

    SAME(*one->lines(), std::size_t{2});

    one->undo();

    SAME(one->text(), std::string{"a\n"});

    one->undo();

    SAME(one->text(), std::string{"a"});
    SAME(*one->lines(), std::size_t{1});
}

// Typing over a selection is one change. If it were a deletion followed by typing, one undo would
// return the document without the selected piece instead of with what stood in it.
TRIAL("Editor: typing over a selection is undone in one step") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->selection().set(2, 5);
    one->insert("X");

    SAME(one->text(), std::string{"abXf"});

    one->undo();

    SAME(one->text(), std::string{"abcdef"});
    OUGHT(!one->undoable());
}

TRIAL("Editor: a held backspace is gathered into one step") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(6);
    one->back_erase(1);
    one->back_erase(1);
    one->back_erase(1);

    SAME(one->text(), std::string{"abc"});

    one->undo();

    SAME(one->text(), std::string{"abcdef"});
    SAME(one->cursor(), std::size_t{6});
}

// A long held Backspace. The deleted characters are collected from the back; putting each new one
// before the previous ones would rewrite the entry in full on every character, which is quadratic
// for a large file. Multibyte characters come back whole on undo.
TRIAL("Editor: a long held backspace is undone in full in one step") {
    Sheet sheet;

    std::string was;

    for (int at = 0; at < 300; ++at) was += "zażółć gęślą ";

    const auto one = shown(sheet, was);

    one->cursor(was.size());

    while (one->cursor() > 0) one->back_erase(1);

    SAME(one->text(), std::string{});

    one->undo();

    SAME(one->text(), was);
    OUGHT(!one->undoable());

    one->redo();

    SAME(one->text(), std::string{});
}

// A held Backspace a few bytes at a time: every piece comes back in its own order.
TRIAL("Editor: a backspace of several bytes comes back in order") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdefghi");

    one->cursor(9);
    one->back_erase(3);
    one->back_erase(3);
    one->back_erase(3);

    SAME(one->text(), std::string{});

    one->undo();

    SAME(one->text(), std::string{"abcdefghi"});
}

// A held Backspace followed by a held Delete at the same place is one entry, and it comes back in
// the right order: first what was before the cursor, then what was after it.
TRIAL("Editor: backspace and delete at the same place come back in order") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(3);
    one->back_erase(1);
    one->back_erase(1);
    one->front_erase(1);
    one->front_erase(1);

    SAME(one->text(), std::string{"af"});

    one->undo();

    SAME(one->text(), std::string{"abcdef"});
    OUGHT(!one->undoable());
}

TRIAL("Editor: a held delete is gathered into one step") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(1);
    one->front_erase(1);
    one->front_erase(1);

    SAME(one->text(), std::string{"adef"});

    one->undo();

    SAME(one->text(), std::string{"abcdef"});
}

// A new change after an undo cuts off the redo branch: there is nothing left to redo.
TRIAL("Editor: a change after an undo cuts off redo") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->cursor(3);
    one->insert("X");
    one->undo();

    OUGHT(one->redoable());

    one->cursor(0);
    one->insert("Y");

    OUGHT(!one->redoable());
    SAME(one->text(), std::string{"Yabc"});
}

// Dirtiness is computed from the journal, not from the quilt: undoing everything returns to the
// same place in the journal, so the document is clean again.
TRIAL("Editor: undoing everything makes the document clean") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->cursor(1);
    one->insert("X");

    OUGHT(one->dirty());

    one->undo();

    OUGHT(!one->dirty());

    one->redo();

    OUGHT(one->dirty());

    one->clean();

    OUGHT(!one->dirty());

    one->undo();

    OUGHT(one->dirty());
}

TRIAL("Editor: undo puts the cursor where it stood before the change") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(4);
    one->insert("XY");

    SAME(one->cursor(), std::size_t{6});

    one->undo();

    SAME(one->cursor(), std::size_t{4});
}

TRIAL("Editor: undo restores the number of lines") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef\nghi");

    SAME(*one->lines(), std::size_t{3});

    one->cursor(3);
    one->back_erase(1);

    SAME(*one->lines(), std::size_t{3});
    SAME(one->text(), std::string{"ab\ndef\nghi"});

    one->cursor(2);
    one->front_erase(1);

    SAME(*one->lines(), std::size_t{2});

    one->undo();

    SAME(*one->lines(), std::size_t{3});
    SAME(one->text(), std::string{"ab\ndef\nghi"});
}

// A line does not come from a newline but from whether something follows it: "abc" and "abc\n" are
// one line, and an empty document has none. An adjustment counted from newlines alone would go
// wrong at both of these ends.
TRIAL("Editor: enter at the end of the document adds no line") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    SAME(*one->lines(), std::size_t{1});

    one->cursor(3);
    one->insert("\n");

    SAME(*one->lines(), std::size_t{1});

    one->insert("d");

    SAME(*one->lines(), std::size_t{2});

    one->undo();
    one->undo();

    SAME(*one->lines(), std::size_t{1});
    SAME(one->text(), std::string{"abc"});
}

TRIAL("Editor: an empty document has no line, and one character already has one") {
    Sheet sheet;

    const auto one = shown(sheet, "");

    SAME(*one->lines(), std::size_t{0});

    one->insert("a");

    SAME(*one->lines(), std::size_t{1});

    one->back_erase(1);

    SAME(*one->lines(), std::size_t{0});
}

TRIAL("Editor: read-only does not undo") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->cursor(1);
    one->insert("X");
    one->readonly(true);

    OUGHT(!one->undoable());

    one->undo();

    SAME(one->text(), std::string{"aXbc"});
}

// A random sequence of changes undone to the end must give the original to the byte: it is the only
// way to catch an error in reversing a change that does not show in any single case.
TRIAL("Editor: a random sequence of changes is undone to the original") {
    std::mt19937 gen(11);

    for (int round = 0; round < 60; ++round) {
        Sheet sheet;

        const std::string was = "pierwszy\ndrugi\ntrzeci\nczwarty";

        const auto one = shown(sheet, was);

        std::vector<std::string> steps;

        for (int step = 0; step < 12; ++step) {
            const std::string before = one->text();

            const std::size_t much = one->whole().value_or(0);

            one->cursor(much == 0 ? 0 : gen() % (much + 1));

            if (gen() % 2 || much == 0) {
                std::string what;

                const int size = 1 + static_cast<int>(gen() % 4);

                for (int which = 0; which < size; ++which)
                    what.push_back(gen() % 7 == 0 ? '\n' : static_cast<char>('A' + gen() % 26));

                one->insert(what);
            } else if (gen() % 2) {
                one->back_erase(1 + gen() % 3);
            } else {
                one->front_erase(1 + gen() % 3);
            }

            // A step that changed nothing (deleting at an end) leaves no entry in the journal, so
            // there is nothing to undo for it either.
            if (one->text() != before) steps.push_back(before);
        }

        while (!steps.empty()) {
            one->undo();

            SAME(one->text(), steps.back());

            steps.pop_back();
        }

        OUGHT(!one->undoable());
        OUGHT(!one->dirty());
        SAME(one->text(), was);
    }
}

// Jumping by words follows the same rule as whole-word search and double-click: one definition of a
// word in the whole library.
TRIAL("Editor: ctrl with an arrow moves by words") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->focus();

    ui::KeyEvent right{ui::Key{ui::Code::right, ui::Mods{true, false, false}}};

    OUGHT(sheet.dispatch(right));
    SAME(one->cursor(), std::size_t{4});

    OUGHT(sheet.dispatch(right));
    SAME(one->cursor(), std::size_t{7});

    OUGHT(sheet.dispatch(right));
    SAME(one->cursor(), std::size_t{11});

    ui::KeyEvent left{ui::Key{ui::Code::left, ui::Mods{true, false, false}}};

    OUGHT(sheet.dispatch(left));
    SAME(one->cursor(), std::size_t{7});

    OUGHT(sheet.dispatch(left));
    SAME(one->cursor(), std::size_t{4});

    OUGHT(sheet.dispatch(left));
    SAME(one->cursor(), std::size_t{0});
}

TRIAL("Editor: jumping by words crosses between lines") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->focus();

    one->cursor(3);

    ui::KeyEvent right{ui::Key{ui::Code::right, ui::Mods{true, false, false}}};

    OUGHT(sheet.dispatch(right));
    SAME(one->cursor(), std::size_t{4});

    ui::KeyEvent left{ui::Key{ui::Code::left, ui::Mods{true, false, false}}};

    OUGHT(sheet.dispatch(left));
    SAME(one->cursor(), std::size_t{3});
}

TRIAL("Editor: jumping by words does not split a multibyte character") {
    Sheet sheet;

    const auto one = shown(sheet, "k\xc4\x85t ma");

    one->focus();

    ui::KeyEvent right{ui::Key{ui::Code::right, ui::Mods{true, false, false}}};

    OUGHT(sheet.dispatch(right));
    SAME(one->cursor(), std::size_t{5});
}

TRIAL("Editor: ctrl with an arrow and shift extends the selection") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->focus();

    ui::KeyEvent right{ui::Key{ui::Code::right, ui::Mods{true, false, true}}};

    OUGHT(sheet.dispatch(right));
    OUGHT(sheet.dispatch(right));

    SAME(one->selection().anchor(), std::size_t{0});
    SAME(one->selection().text(), std::string{"ala ma "});
}

TRIAL("Editor: jumping by words at the ends stays in place") {
    Sheet sheet;

    const auto one = shown(sheet, "ala");

    one->focus();

    ui::KeyEvent left{ui::Key{ui::Code::left, ui::Mods{true, false, false}}};

    OUGHT(sheet.dispatch(left));
    SAME(one->cursor(), std::size_t{0});

    one->cursor(3);

    ui::KeyEvent right{ui::Key{ui::Code::right, ui::Mods{true, false, false}}};

    OUGHT(sheet.dispatch(right));
    SAME(one->cursor(), std::size_t{3});
}

// --- Clipboard -----------------------------------------------------------------------------------

TRIAL("Editor: copy and paste go through the clipboard") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->selection().set(0, 3);

    OUGHT(one->copy());

    one->cursor(11);
    one->insert(" ");

    OUGHT(one->paste());

    SAME(one->text(), std::string{"ala ma kota ala"});
}

TRIAL("Editor: copying without a selection touches nothing") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    OUGHT(!one->copy());
}

// Cutting goes through the same deletion of the selection as Backspace, so it is one change in the
// journal and comes back with one undo.
TRIAL("Editor: cutting removes the text and is undone in one step") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->selection().set(4, 6);

    OUGHT(one->cut());

    SAME(one->text(), std::string{"ala  kota"});

    one->undo();

    SAME(one->text(), std::string{"ala ma kota"});
    OUGHT(!one->undoable());
}

TRIAL("Editor: pasting over a selection replaces it") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->selection().set(0, 2);

    OUGHT(one->copy());

    one->selection().set(4, 6);

    OUGHT(one->paste());

    SAME(one->text(), std::string{"abcdab"});
}

TRIAL("Editor: read-only copies, but does not cut or paste") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->selection().set(0, 3);

    OUGHT(one->copy());

    one->readonly(true);

    OUGHT(!one->cut());
    OUGHT(!one->paste());

    SAME(one->text(), std::string{"ala ma kota"});
    OUGHT(!one->dirty());
}

TRIAL("Editor: a copy goes to the given clipboard") {
    Sheet sheet;

    const auto kept = faking::clipboard();
    const auto one = shown(sheet, "ala ma kota", kept);

    one->selection().set(0, 3);

    OUGHT(one->copy());
    SAME(kept->kept.value_or(""), std::string{"ala"});
}

// When the clipboard refuses, cutting does not delete: the document would be left without text that
// has nowhere to be pasted from.
TRIAL("Editor: a clipboard refusal does not cut") {
    Sheet sheet;

    const auto kept = faking::clipboard();

    kept->refusing = true;

    const auto one = shown(sheet, "ala ma kota", kept);

    one->selection().set(0, 3);

    OUGHT(!one->cut());
    SAME(kept->copies, 1);
    SAME(one->text(), std::string{"ala ma kota"});
    OUGHT(!one->dirty());
}

TRIAL("Editor: two editors on one clipboard exchange text") {
    Sheet first;
    Sheet second;

    const auto kept = faking::clipboard();
    const auto one = shown(first, "ala ma kota", kept);
    const auto two = shown(second, "pies", kept);

    one->selection().set(4, 6);

    OUGHT(one->cut());

    two->cursor(4);

    OUGHT(two->paste());
    SAME(two->text(), std::string{"piesma"});
}

// Ctrl with a letter is not the editor's business: the editor binds no clipboard key at all. In raw
// mode Ctrl+C is not an interrupt but an ordinary key, so an editor that took it whenever it had the
// focus would hide it from an application that wants it for quitting, with no way for the
// application to undo that.
TRIAL("Editor: ctrl with a letter goes up and leaves the clipboard alone") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->focus();
    one->selection().set(0, 4);

    ui::KeyEvent copy{ui::Key{U'c', ui::Mods{true, false, false}}};

    OUGHT(!sheet.dispatch(copy));

    one->cursor(11);

    ui::KeyEvent paste{ui::Key{U'v', ui::Mods{true, false, false}}};

    OUGHT(!sheet.dispatch(paste));

    ui::KeyEvent cut{ui::Key{U'x', ui::Mods{true, false, false}}};

    OUGHT(!sheet.dispatch(cut));

    SAME(one->text(), std::string{"ala ma kota"});
}

// The application binds the clipboard keys it wants with a hook, and then they work, because a hook
// sees the key regardless of what `handle` does.
TRIAL("Editor: clipboard on ctrl can be added with a hook") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->focus();
    one->selection().set(0, 4);

    const std::weak_ptr<ui::Editor> weak = one;

    const ui::Hook heard_key = one->on<ui::KeyEvent>([weak](ui::KeyEvent& what) {
        if (!what.mods().ctrl || what.key().letter() != U'c') return false;

        const auto mine = weak.lock();

        return mine && mine->copy();
    });

    ui::KeyEvent copy{ui::Key{U'c', ui::Mods{true, false, false}}};

    OUGHT(sheet.dispatch(copy));

    SAME(one->selection().text(), std::string{"ala "});
}

// The keys of Midnight Commander's clipboard (Ctrl+Ins, Shift+Ins, Shift+Del) go up as well: they
// are the application's to bind, and Shift+Del in particular does not delete a character on its
// way, which would leave the application cutting from a changed text.
TRIAL("Editor: insert and delete with modifiers go up and leave the text alone") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->focus();
    one->selection().set(0, 4);

    ui::KeyEvent copy{ui::Key{ui::Code::insert, ui::Mods{true, false, false}}};

    OUGHT(!sheet.dispatch(copy));

    one->cursor(11);

    ui::KeyEvent paste{ui::Key{ui::Code::insert, ui::Mods{false, false, true}}};

    OUGHT(!sheet.dispatch(paste));

    one->cursor(0);

    ui::KeyEvent cut{ui::Key{ui::Code::remove, ui::Mods{false, false, true}}};

    OUGHT(!sheet.dispatch(cut));

    SAME(one->text(), std::string{"ala ma kota"});
    SAME(one->cursor(), std::size_t{0});
}

// Delete alone, without Shift, still deletes a character rather than cutting.
TRIAL("Editor: delete alone deletes a character, it does not cut") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->focus();
    one->cursor(0);

    ui::KeyEvent remove{ui::Code::remove};

    OUGHT(sheet.dispatch(remove));
    SAME(one->text(), std::string{"bcdef"});
}

// --- Mouse ---------------------------------------------------------------------------------------

TRIAL("Editor: a double click takes a word") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota\ndrugi");

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::DoubleClickEvent what{{5, 0}, ui::Mouse::left};

    OUGHT(sheet.dispatch(what));
    SAME(one->selection().text(), std::string{"ma"});
    SAME(one->selection().anchor(), std::size_t{4});
    SAME(one->cursor(), std::size_t{6});
}

// A letter with a diacritic is a letter by the same rule by which the comb searches for whole
// words; otherwise double-click and search would start to differ.
TRIAL("Editor: a double click does not split a word with a diacritic") {
    Sheet sheet;

    const auto one = shown(sheet, "ma k\xc4\x85t tam");

    (void)sheet.render({{0, 0}, {20, 2}});

    ui::DoubleClickEvent what{{3, 0}, ui::Mouse::left};

    OUGHT(sheet.dispatch(what));
    SAME(one->selection().text(), std::string{"k\xc4\x85t"});
}

// A click on a place that is not a word takes a run of characters of the same kind, so a double
// click never looks ignored.
TRIAL("Editor: a double click outside a word takes a run of characters of the same kind") {
    Sheet sheet;

    const auto one = shown(sheet, "ala   ma");

    (void)sheet.render({{0, 0}, {20, 2}});

    ui::DoubleClickEvent what{{4, 0}, ui::Mouse::left};

    OUGHT(sheet.dispatch(what));
    SAME(one->selection().text(), std::string{"   "});
}

TRIAL("Editor: a double click after a tab takes the word after it") {
    Sheet sheet;

    const auto one = shown(sheet, "\tala ma");

    (void)sheet.render({{0, 0}, {20, 2}});

    ui::DoubleClickEvent what{{9, 0}, ui::Mouse::left};

    OUGHT(sheet.dispatch(what));
    SAME(one->selection().text(), std::string{"ala"});
}

// A press sets the cursor together with the anchor; movement with the button held extends the
// selection.
TRIAL("Editor: dragging with the mouse extends the selection") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef\nghijkl");

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::MouseEvent down{{2, 0}, ui::Press::down, ui::Mouse::left};

    OUGHT(sheet.dispatch(down));
    SAME(one->cursor(), std::size_t{2});
    OUGHT(one->selection().empty());

    ui::MouseEvent move{{5, 0}, ui::Press::move, ui::Mouse::left};

    OUGHT(sheet.dispatch(move));
    SAME(one->selection().anchor(), std::size_t{2});
    SAME(one->selection().text(), std::string{"cde"});

    ui::MouseEvent far{{3, 1}, ui::Press::move, ui::Mouse::left};

    OUGHT(sheet.dispatch(far));
    SAME(one->selection().text(), std::string{"cdef\nghi"});

    ui::MouseEvent up{{3, 1}, ui::Press::up, ui::Mouse::left};

    OUGHT(sheet.dispatch(up));

    // After release, movement over the editor has no right to extend anything.
    ui::MouseEvent after{{5, 1}, ui::Press::move, ui::Mouse::none};

    OUGHT(sheet.dispatch(after));
    SAME(one->selection().text(), std::string{"cdef\nghi"});
}

// The edge row scrolls by one line, and the speed comes from how much the user really moves the
// mouse: when it stops, scrolling stops with it. No timer is needed for that.
TRIAL("Editor: dragging in the edge row scrolls") {
    Sheet sheet;

    const auto one = shown(sheet, many(20));

    (void)sheet.render({{0, 0}, {20, 3}});

    SAME(*one->first(), std::size_t{0});

    ui::MouseEvent down{{1, 0}, ui::Press::down, ui::Mouse::left};

    OUGHT(sheet.dispatch(down));

    ui::MouseEvent low{{1, 2}, ui::Press::move, ui::Mouse::left};

    OUGHT(sheet.dispatch(low));
    (void)sheet.render({{0, 0}, {20, 3}});
    SAME(*one->first(), std::size_t{1});

    OUGHT(sheet.dispatch(low));
    (void)sheet.render({{0, 0}, {20, 3}});
    SAME(*one->first(), std::size_t{2});

    OUGHT(!one->selection().empty());

    ui::MouseEvent high{{1, 0}, ui::Press::move, ui::Mouse::left};

    OUGHT(sheet.dispatch(high));
    (void)sheet.render({{0, 0}, {20, 3}});
    SAME(*one->first(), std::size_t{1});
}

// A drag goes outside the editor: the press asks for capture, so movement below and above the
// editor still arrives, scrolls by one line and extends the selection to the outermost visible
// line, since outside the area there is no cell to hit.
TRIAL("Editor: dragging outside the editor scrolls and selects to the outermost line") {
    Sheet sheet;

    const auto one = shown(sheet, many(20));

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::MouseEvent down{{1, 0}, ui::Press::down, ui::Mouse::left};

    OUGHT(sheet.dispatch(down));

    ui::MouseEvent below{{1, 10}, ui::Press::move, ui::Mouse::left};

    OUGHT(sheet.dispatch(below));
    SAME(one->selection().text(), std::string{"0\nl1\nl"});

    (void)sheet.render({{0, 0}, {20, 3}});
    SAME(*one->first(), std::size_t{1});

    OUGHT(sheet.dispatch(below));
    SAME(one->selection().text(), std::string{"0\nl1\nl2\nl"});

    (void)sheet.render({{0, 0}, {20, 3}});
    SAME(*one->first(), std::size_t{2});

    ui::MouseEvent above{{1, -5}, ui::Press::move, ui::Mouse::left};

    OUGHT(sheet.dispatch(above));
    SAME(one->selection().text(), std::string{"0\nl1\nl"});

    (void)sheet.render({{0, 0}, {20, 3}});
    SAME(*one->first(), std::size_t{1});
}

// A release outside the editor also comes back to it and ends the drag; otherwise it would go to
// whatever lies under the pointer, and selecting would stay on until the next press.
TRIAL("Editor: releasing outside the editor ends the drag") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef\nghijkl");

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::MouseEvent down{{1, 0}, ui::Press::down, ui::Mouse::left};
    ui::MouseEvent move{{4, 0}, ui::Press::move, ui::Mouse::left};
    ui::MouseEvent up{{4, 10}, ui::Press::up, ui::Mouse::left};

    OUGHT(sheet.dispatch(down));
    OUGHT(sheet.dispatch(move));
    (void)sheet.dispatch(up);

    SAME(one->selection().text(), std::string{"bcd"});

    ui::MouseEvent later{{3, 1}, ui::Press::move, ui::Mouse::left};

    (void)sheet.dispatch(later);

    SAME(one->selection().text(), std::string{"bcd"});
}

// `at()` means in the editor what it means in the viewer: the byte at which the first visible line
// starts.
TRIAL("Editor: at is the byte of the first visible line, not of the cursor's line") {
    Sheet sheet;

    const auto one = shown(sheet, many(20));

    (void)sheet.render({{0, 0}, {20, 3}});

    one->cursor(std::size_t{0});
    one->first(std::size_t{4});

    SAME(one->cursor(), std::size_t{0});
    SAME(*one->at(), std::size_t{12});
    SAME(*one->whole(), many(20).size());
}

// --- Search --------------------------------------------------------------------------------------

TRIAL("Editor: search finds and selects") {
    Sheet sheet;

    const auto one = shown(sheet, "pierwszy\ndrugi\ntrzeci drugi");

    const Comb how{"drugi", {}};

    const std::optional<Hit> got = one->find(how, 0, false);

    OUGHT(got.has_value());
    SAME(got->from, std::size_t{9});
    SAME(got->upto, std::size_t{14});
    SAME(one->selection().text(), std::string{"drugi"});
    SAME(one->line(), std::size_t{1});
}

TRIAL("Editor: search goes on from the given place") {
    Sheet sheet;

    const auto one = shown(sheet, "pierwszy\ndrugi\ntrzeci drugi");

    const Comb how{"drugi", {}};

    SAME(one->find(how, 10, false)->from, std::size_t{22});
    OUGHT(!one->find(how, 23, false).has_value());
}

TRIAL("Editor: backward search takes the previous match") {
    Sheet sheet;

    const auto one = shown(sheet, "ala\nma\nala\nkota\nala");

    const Comb how{"ala", {}};

    SAME(one->find(how, 19, true)->from, std::size_t{16});
    SAME(one->find(how, 16, true)->from, std::size_t{7});
    SAME(one->find(how, 7, true)->from, std::size_t{0});
    OUGHT(!one->find(how, 0, true).has_value());
}

TRIAL("Editor: search does not find what is not there") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    const Comb how{"xyz", {}};

    OUGHT(!one->find(how, 0, false).has_value());
    OUGHT(!one->find(how, 7, true).has_value());
}

// Replacing takes the selection only when the whole of it is a hit. A selection that merely
// contains a hit stays untouched, and replacing goes on to look for the next one.
TRIAL("Editor: replacing does not take a selection that merely contains a hit") {
    Sheet sheet;

    const auto one = shown(sheet, "xala ala");

    const Comb how{"ala", {}};

    one->selection().set(0, 4);

    OUGHT(!one->replace(how, "ola"));
    SAME(one->text(), std::string{"xala ala"});
    SAME(one->selection().from(), std::size_t{5});
    SAME(one->selection().upto(), std::size_t{8});
}

// A bad pattern has no right to do anything: neither find nor throw.
TRIAL("Editor: a comb that is not ready finds nothing") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    const Comb how{"[", Sift{true, false, false}};

    OUGHT(!one->find(how, 0, false).has_value());
}

TRIAL("Editor: a regular expression search goes line by line") {
    Sheet sheet;

    const auto one = shown(sheet, "rok 2026\nbez liczb\nmiesiac 12");

    const Comb how{"[0-9]+", Sift{true, false, false}};

    SAME(one->selection().text(), std::string{});

    OUGHT(one->find(how, 0, false).has_value());
    SAME(one->selection().text(), std::string{"2026"});

    OUGHT(one->find(how, 8, false).has_value());
    SAME(one->selection().text(), std::string{"12"});
}

// A pattern does not cross the end of a line: we search by lines, because the document is served in
// lines.
TRIAL("Editor: a pattern does not cross the end of a line") {
    Sheet sheet;

    const auto one = shown(sheet, "ab\ncd");

    const Comb how{"b\nc", {}};

    OUGHT(!one->find(how, 0, false).has_value());
}

// Searching a document longer than one batch of lines: the batches compute their starting byte
// themselves, so an off-by-one error would show only past a batch boundary.
TRIAL("Editor: search crosses a batch boundary") {
    Sheet sheet;

    std::string big;

    for (int which = 0; which < 300; ++which) big += "wiersz\n";

    big += "igla";

    const auto one = shown(sheet, big);

    const Comb how{"igla", {}};

    const std::optional<Hit> got = one->find(how, 0, false);

    OUGHT(got.has_value());
    SAME(got->from, big.size() - 4);
    SAME(one->line(), std::size_t{300});

    SAME(one->find(how, big.size(), true)->from, big.size() - 4);
}

// --- Replace -------------------------------------------------------------------------------------

TRIAL("Editor: replacing without a selection first shows the match") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma ala");

    const Comb how{"ala", {}};

    OUGHT(!one->replace(how, "ola"));
    SAME(one->selection().text(), std::string{"ala"});
    SAME(one->text(), std::string{"ala ma ala"});
}

TRIAL("Editor: replacing takes the selected match and shows the next one") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma ala");

    const Comb how{"ala", {}};

    (void)one->find(how, 0, false);

    // Replacing leaves the next match selected, so a second call replaces it without searching. A
    // helper `whole` along the way would break that, since it selects the whole document.
    OUGHT(one->replace(how, "ola"));
    SAME(one->selection().text(), std::string{"ala"});

    OUGHT(one->replace(how, "ola"));
    SAME(one->selection().text(), std::string{});

    SAME(one->text(), std::string{"ola ma ola"});
}

TRIAL("Editor: replacing leaves alone a selection that is not a match") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    const Comb how{"ala", {}};

    one->selection().set(4, 6);

    OUGHT(!one->replace(how, "ola"));
    SAME(one->text(), std::string{"ala ma kota"});
}

TRIAL("Editor: replacing all takes every match") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma ala\nala kot");

    const Comb how{"ala", {}};

    SAME(one->replace_all(how, "ola"), std::size_t{3});
    SAME(one->text(), std::string{"ola ma ola\nola kot"});
}

// A line ends in LF or in CRLF, and a line's text holds neither. The place of a hit is counted from
// where the spool says the line starts, not by adding the lengths of the lines before it, so a
// document from Windows is searched and replaced at the right bytes.
TRIAL("Editor: searching and replacing all hit the right bytes in a CRLF document") {
    Sheet sheet;

    const auto one = shown(sheet, "foo\r\nfoo\r\nx foo\r\n");

    const Comb how{"foo", {}};

    const std::optional<Hit> second = one->find(how, 1, false);

    OUGHT(second.has_value() && second->from == 5 && second->upto == 8);

    const std::optional<Hit> back = one->find(how, 15, true);

    OUGHT(back.has_value() && back->from == 12 && back->upto == 15);

    SAME(one->replace_all(how, "bar"), std::size_t{3});
    SAME(one->text(), std::string{"bar\r\nbar\r\nx bar\r\n"});
}

// All the replacements are tied to the first, so they are undone in one step.
TRIAL("Editor: replacing all is undone in one step") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma ala\nala kot");

    const Comb how{"ala", {}};

    SAME(one->replace_all(how, "ola"), std::size_t{3});

    one->undo();

    SAME(one->text(), std::string{"ala ma ala\nala kot"});
    OUGHT(!one->undoable());

    one->redo();

    SAME(one->text(), std::string{"ola ma ola\nola kot"});
}

// A replacement that contains the pattern has no right to make replacing loop.
TRIAL("Editor: replacing with text containing the pattern does not loop") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ala");

    const Comb how{"ala", {}};

    SAME(one->replace_all(how, "alala"), std::size_t{2});
    SAME(one->text(), std::string{"alala alala"});
}

TRIAL("Editor: replacing with nothing deletes the matches") {
    Sheet sheet;

    const auto one = shown(sheet, "a1b22c333d");

    const Comb how{"[0-9]+", Sift{true, false, false}};

    SAME(one->replace_all(how, ""), std::size_t{3});
    SAME(one->text(), std::string{"abcd"});
}

TRIAL("Editor: replacing whole words") {
    Sheet sheet;

    const auto one = shown(sheet, "ala alama ala");

    const Comb how{"ala", Sift{false, false, true}};

    SAME(one->replace_all(how, "ola"), std::size_t{2});
    SAME(one->text(), std::string{"ola alama ola"});
}

// Replacing all shifts the block together with the text and puts the cursor after the last
// replacement, and undo restores the text, the block and the cursor from before.
TRIAL("Editor: replacing all shifts the block, places the cursor and is undone in full") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma ala i ala");
    ui::Editor::Block& block = one->block();

    one->cursor(4);
    block.mark();
    one->cursor(6);
    block.mark();

    SAME(block.text(), std::string{"ma"});

    one->cursor(3);

    const Comb how{"ala", {}};

    SAME(one->replace_all(how, "kasia"), std::size_t{3});
    SAME(one->text(), std::string{"kasia ma kasia i kasia"});
    SAME(block.from(), std::size_t{6});
    SAME(block.upto(), std::size_t{8});
    SAME(one->cursor(), std::size_t{22});

    one->undo();

    SAME(one->text(), std::string{"ala ma ala i ala"});
    SAME(block.from(), std::size_t{4});
    SAME(block.upto(), std::size_t{6});
    SAME(one->cursor(), std::size_t{3});

    one->redo();

    SAME(one->text(), std::string{"kasia ma kasia i kasia"});
    SAME(one->cursor(), std::size_t{22});
}

// A pattern that also matches nothing does not make replacing loop: an empty hit replaces nothing
// and moves the search on by one character.
TRIAL("Editor: an empty hit does not make replacing all loop") {
    Sheet sheet;

    const auto one = shown(sheet, "axxbx");

    const Comb how{"x*", Sift{true, false, false}};

    SAME(one->replace_all(how, "y"), std::size_t{2});
    SAME(one->text(), std::string{"ayby"});
}

// Twenty thousand hits are replaced and undone at once. Going separately through search, selection
// and insertion for each, with the quilt rebuilt every time, would be quadratic.
TRIAL("Editor: twenty thousand hits are replaced and undone in one step") {
    Sheet sheet;

    const std::string was = many(20000);
    const auto one = shown(sheet, was);

    const Comb how{"l", {}};

    SAME(one->replace_all(how, "wiersz "), std::size_t{20000});
    SAME(one->text().substr(0, 17), std::string{"wiersz 0\nwiersz 1"});
    SAME(one->text().size(), was.size() + 20000 * 6);

    one->undo();

    SAME(one->text(), was);
    OUGHT(!one->undoable());

    one->redo();

    SAME(one->text().size(), was.size() + 20000 * 6);
}

// Replacements with line ends change the number of lines, so going to a line past the old end
// lands.
TRIAL("Editor: replacing all with lines adjusts the number of lines") {
    Sheet sheet;

    const auto one = shown(sheet, "a\na\na");

    one->line(2);

    SAME(one->line(), std::size_t{2});

    const Comb how{"a", {}};

    SAME(one->replace_all(how, "x\ny"), std::size_t{3});

    one->line(5);

    SAME(one->line(), std::size_t{5});
    SAME(one->cursor(), std::size_t{10});
}

// Replacing after undoing past the saved point cuts off the branch it lay on: the document is
// dirty, even if the number of entries is back to what it was at saving.
TRIAL("Editor: replacing all after undoing past the save leaves the document dirty") {
    Sheet sheet;

    const auto one = shown(sheet, "ala");

    one->cursor(3);
    one->insert("!");
    one->clean();
    one->undo();

    const Comb how{"ala", {}};

    SAME(one->replace_all(how, "ola"), std::size_t{1});
    OUGHT(one->dirty());
}

TRIAL("Editor: read-only does not replace") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma ala");

    one->readonly(true);

    const Comb how{"ala", {}};

    SAME(one->replace_all(how, "ola"), std::size_t{0});
    SAME(one->text(), std::string{"ala ma ala"});
}

// --- Saving --------------------------------------------------------------------------------------

namespace {

// A drain that collects everything into a string, with a counter of chunks, because a document
// larger than memory has to flow in chunks, not in one bite. It returns a promise resolved on the
// pool, like `File::write`. An empty chunk is the end: it is counted separately, because it is what
// closes the write.
Drain into(std::string& what, int& much, int* ended = nullptr) {
    return [&what, &much, ended](Bytes got) {
        if (got.empty() && ended != nullptr) ++*ended;

        for (const std::byte one : got) what.push_back(static_cast<char>(one));

        if (!got.empty()) ++much;

        return ready();
    };
}

// Turns the loop until the write's promise settles, at most ten seconds (then the trial fails
// where it asked); returns whether it was fulfilled. The
// continuation goes onto the loop (`on`): the write finishes on a pool thread, and the variable is
// read by this thread. Otherwise a rejected write would store the result from the pool thread, a
// data race that the thread sanitizer reports.
bool saved(Loop& loop, Promise<void> made, std::source_location where = std::source_location::current()) {
    std::optional<bool> went;

    Promise<void> watched = std::move(made)
                                .on(loop)
                                .then([&went] { went = true; })
                                .recover([&went](const std::exception&) { went = false; });

    tal::testing::waited(where.file_name(), static_cast<int>(where.line()), "the write never settled", loop, [&went] { return went.has_value(); });

    return *went;
}

}

TRIAL("Editor: saving gives the whole content, ends with an empty chunk and makes the document clean") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->cursor(0);
    one->insert("X");

    OUGHT(one->dirty());

    std::string got;
    int much = 0;
    int ended = 0;

    Promise<void> made = one->save(into(got, much, &ended));

    // Until fulfilled, the editor is busy and dirty: it becomes clean only after the whole.
    OUGHT(one->busy());
    OUGHT(one->dirty());

    OUGHT(saved(loop, std::move(made)));

    SAME(got, std::string{"Xabc\ndef"});
    SAME(much, 1);
    SAME(ended, 1);
    OUGHT(!one->dirty());
    OUGHT(!one->busy());
}

// An empty document is a single call for the end: the drain has something to close, though nothing
// to write.
TRIAL("Editor: saving an empty document gives only the end") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(sheet, "");

    std::string got;
    int much = 0;
    int ended = 0;

    OUGHT(saved(loop, one->save(into(got, much, &ended))));

    SAME(got, std::string{});
    SAME(much, 0);
    SAME(ended, 1);
}

// "Clean" means "on disk": a drain that refuses only at the end, because the file could not be
// closed, leaves the document dirty, though it accepted all the chunks.
TRIAL("Editor: a refusal at the end leaves the document dirty") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(0);
    one->insert("X");

    std::string got;

    OUGHT(!saved(loop, one->save([&got](Bytes given) {
        if (given.empty())
            return failed<void>(std::make_exception_ptr(std::runtime_error{"rename failed"}));

        for (const std::byte one : given) got.push_back(static_cast<char>(one));

        return ready();
    })));

    SAME(got, std::string{"Xabcdef"});
    OUGHT(one->dirty());
    OUGHT(!one->busy());
}

// An unfinished save is not a save: the document stays dirty, so that the application does not
// consider it stored on disk. A drain without a promise and a second save during the first are
// rejected at once.
TRIAL("Editor: a drain that refuses interrupts the save") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(0);
    one->insert("X");

    OUGHT(!saved(loop, one->save([](Bytes) {
        return failed<void>(std::make_exception_ptr(std::runtime_error{"disk full"}));
    })));
    OUGHT(one->dirty());
    OUGHT(!one->busy());

    OUGHT(!saved(loop, one->save({})));

    std::string got;
    int much = 0;

    Promise<void> first = one->save(into(got, much));

    OUGHT(!saved(loop, one->save(into(got, much))));
    OUGHT(saved(loop, std::move(first)));
    OUGHT(!one->dirty());
}

// A drain that throws before it returns a promise: the save is rejected and the editor does not
// stay busy; otherwise every later save would be rejected as "a second one in progress", and
// leaving the editor would wait forever.
TRIAL("Editor: a drain that throws before its promise does not leave the editor busy") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(0);
    one->insert("X");

    bool sync = false;
    std::optional<Promise<void>> made;

    try {
        made.emplace(one->save([](Bytes) -> Promise<void> { throw std::runtime_error{"no handle"}; }));
    } catch (const std::exception&) {
        sync = true;
    }

    OUGHT(!sync);
    OUGHT(made.has_value() && !saved(loop, std::move(*made)));
    OUGHT(!one->busy());
    OUGHT(one->dirty());

    std::string got;
    int much = 0;

    OUGHT(saved(loop, one->save(into(got, much))));
    SAME(got, std::string{"Xabcdef"});
}

// During a save the content stands still: keys that change it bounce off, so does undo, and after
// fulfillment everything comes back.
TRIAL("Editor: the content does not change during a save") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(0);
    one->insert("X");

    std::string got;
    int much = 0;

    Promise<void> made = one->save(into(got, much));

    one->cursor(0);
    one->insert("Y");

    SAME(one->text(), std::string{"Xabcdef"});
    OUGHT(!one->undoable());

    OUGHT(saved(loop, std::move(made)));

    one->cursor(0);
    one->insert("Y");

    SAME(one->text(), std::string{"YXabcdef"});
    OUGHT(one->undoable());
}

TRIAL("Editor: a large document flows in chunks") {
    Loop loop;
    Sheet sheet;

    std::string big;

    for (int which = 0; which < 20000; ++which) big += "wiersz numer " + std::to_string(which) + "\n";

    const auto one = shown(sheet, big);

    std::string got;
    int much = 0;

    OUGHT(saved(loop, one->save(into(got, much))));

    SAME(got, big);
    OUGHT(much > 1);
}

// --- Clean or dirty ------------------------------------------------------------------------------

// Two ways in which a `dirty()` that counted only entry numbers would lie: a word typed on after
// saving would extend the same entry, and a new change after undoing past the saved point would get
// the same number as the saved one. Saving seals the entry, and cutting off a branch invalidates
// the remembered number.
TRIAL("Editor: typing on after saving makes the document dirty") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->cursor(3);
    one->insert("x");

    std::string got;
    int much = 0;

    OUGHT(saved(loop, one->save(into(got, much))));
    OUGHT(!one->dirty());

    one->insert("y");

    OUGHT(one->dirty());
    SAME(one->text(), std::string{"abcxy"});

    // Undoing just the `y` returns to the saved state, because the `y` started its own entry.
    one->undo();

    OUGHT(!one->dirty());
    SAME(one->text(), std::string{"abcx"});
}

TRIAL("Editor: a new change after undoing past the save makes the document dirty for good") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->cursor(3);
    one->insert("x");
    one->cursor(0);
    one->insert("w");

    std::string got;
    int much = 0;

    OUGHT(saved(loop, one->save(into(got, much))));
    OUGHT(!one->dirty());

    one->undo();

    OUGHT(one->dirty());

    one->cursor(0);
    one->insert("q");

    OUGHT(one->dirty());
    SAME(one->text(), std::string{"qabcx"});

    // No undo returns to the saved state any more.
    one->undo();
    one->undo();

    OUGHT(one->dirty());
}

// --- Clicking ------------------------------------------------------------------------------------

TRIAL("Editor: a click puts the cursor in the clicked cell") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::ClickEvent what{{2, 1}, ui::Mouse::left};

    OUGHT(sheet.dispatch(what));
    SAME(one->cursor(), std::size_t{6});
}

// A screen row is not a line number: with wrapping, one line takes several rows. Computed by adding
// the row to the top line, a click on the second half of a wrapped line would land on the next
// line.
TRIAL("Editor: a click on a wrapped line hits the right line") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdefgh\nxy");

    (void)sheet.render({{0, 0}, {4, 3}});

    ui::ClickEvent what{{1, 1}, ui::Mouse::left};

    OUGHT(sheet.dispatch(what));
    SAME(one->cursor(), std::size_t{5});
    SAME(one->line(), std::size_t{0});
}

// A column is not a byte: a tab takes eight cells, and all of them point to the same byte.
TRIAL("Editor: a click after a tab hits the right byte") {
    Sheet sheet;

    const auto one = shown(sheet, "\tab");

    (void)sheet.render({{0, 0}, {20, 2}});

    ui::ClickEvent what{{8, 0}, ui::Mouse::left};

    OUGHT(sheet.dispatch(what));
    SAME(one->cursor(), std::size_t{1});

    ui::ClickEvent far{{3, 0}, ui::Mouse::left};

    OUGHT(sheet.dispatch(far));
    SAME(one->cursor(), std::size_t{0});
}

TRIAL("Editor: a click past the end of a line stops at its end") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::ClickEvent what{{15, 0}, ui::Mouse::left};

    OUGHT(sheet.dispatch(what));
    SAME(one->cursor(), std::size_t{3});
}

// --- Line and column -----------------------------------------------------------------------------

TRIAL("Editor: the cursor's place is counted in lines and cells") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->cursor(5);

    SAME(one->line(), std::size_t{1});
    SAME(one->column(), std::size_t{1});
}

TRIAL("Editor: the column counts a tab as the screen does, not as a byte") {
    Sheet sheet;

    const auto one = shown(sheet, "\tab");

    one->cursor(1);

    SAME(one->column(), std::size_t{8});

    one->cursor(2);

    SAME(one->column(), std::size_t{9});
}

// --- Selecting with keys -------------------------------------------------------------------------

TRIAL("Editor: the cursor clears the selection, and extend drags it") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(2);

    OUGHT(one->selection().empty());

    one->selection().extend(5);

    OUGHT(!one->selection().empty());
    SAME(one->selection().anchor(), std::size_t{2});
    SAME(one->cursor(), std::size_t{5});
    SAME(one->selection().text(), std::string{"cde"});

    one->cursor(4);

    OUGHT(one->selection().empty());
}

TRIAL("Editor: shift with an arrow extends the selection") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->focus();
    one->cursor(2);

    ui::KeyEvent right{ui::Key{ui::Code::right, ui::Mods{false, false, true}}};

    OUGHT(sheet.dispatch(right));
    OUGHT(sheet.dispatch(right));

    SAME(one->selection().anchor(), std::size_t{2});
    SAME(one->cursor(), std::size_t{4});
    SAME(one->selection().text(), std::string{"cd"});
}

TRIAL("Editor: shift with moving down selects across lines") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->focus();

    (void)sheet.render({{0, 0}, {20, 3}});

    one->cursor(1);

    ui::KeyEvent down{ui::Key{ui::Code::down, ui::Mods{false, false, true}}};

    OUGHT(sheet.dispatch(down));
    SAME(one->selection().text(), std::string{"bc\nd"});
}

TRIAL("Editor: shift with Home takes the start of the line") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->focus();
    one->cursor(4);

    ui::KeyEvent home{ui::Key{ui::Code::home, ui::Mods{false, false, true}}};

    OUGHT(sheet.dispatch(home));
    SAME(one->selection().text(), std::string{"abcd"});
    SAME(one->selection().anchor(), std::size_t{4});
}

// --- Paging --------------------------------------------------------------------------------------

TRIAL("Editor: a page key moves the cursor by a screen") {
    Sheet sheet;

    const auto one = shown(sheet, many(10));

    one->focus();

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::KeyEvent down{ui::Code::pagedown};

    OUGHT(sheet.dispatch(down));
    SAME(one->line(), std::size_t{3});

    OUGHT(sheet.dispatch(down));
    SAME(one->line(), std::size_t{6});

    ui::KeyEvent up{ui::Code::pageup};

    OUGHT(sheet.dispatch(up));
    SAME(one->line(), std::size_t{3});
}

// Near the end of the document a page should reach the last line, not refuse to move.
TRIAL("Editor: a page near the end of the document stops on the last line") {
    Sheet sheet;

    const auto one = shown(sheet, many(10));

    one->focus();

    (void)sheet.render({{0, 0}, {20, 3}});

    one->cursor(24);

    SAME(one->line(), std::size_t{8});

    ui::KeyEvent down{ui::Code::pagedown};

    OUGHT(sheet.dispatch(down));
    SAME(one->line(), std::size_t{9});

    OUGHT(!sheet.dispatch(down));
    SAME(one->line(), std::size_t{9});
}

// A key the editor did not use is to go up; eating it blocks the ancestor.
TRIAL("Editor: an arrow with nowhere to go passes the key up") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->focus();

    (void)sheet.render({{0, 0}, {20, 3}});

    one->cursor(1);

    ui::KeyEvent up{ui::Code::up};

    OUGHT(!sheet.dispatch(up));

    one->cursor(5);

    ui::KeyEvent down{ui::Code::down};

    OUGHT(!sheet.dispatch(down));
}

TRIAL("Editor: F keys go up") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->focus();

    ui::KeyEvent f2{ui::Code::f2};

    OUGHT(!sheet.dispatch(f2));
}

// --- After a change ------------------------------------------------------------------------------

// The spool reads bytes through the piece table, so after a change it sees the new content, and its
// anchors after the place of the change go to the bin, so that they do not point to old places.
TRIAL("Editor: the screen shows the content after a change") {
    Sheet sheet;

    const auto one = shown(sheet, "pierwszy\ndrugi");

    (void)sheet.render({{0, 0}, {20, 3}});

    one->cursor(0);
    one->insert("nowy ");

    const Snapshot made = sheet.render({{0, 0}, {20, 3}});

    SAME(said(made, 0), std::string{"nowy pierwszy"});
    SAME(said(made, 1), std::string{"drugi"});
}

TRIAL("Editor: many changes in different places lose no content") {
    Sheet sheet;

    const auto one = shown(sheet, "aaa\nbbb\nccc\nddd");

    one->cursor(4);
    one->insert("X");

    one->cursor(0);
    one->insert("Y");

    one->cursor(*one->whole());
    one->insert("Z");

    SAME(one->text(), std::string{"Yaaa\nXbbb\nccc\ndddZ"});
    SAME(*one->lines(), std::size_t{4});
}

// A held Backspace and Delete across the boundary of typed text. The deleted characters lie partly
// in the additions and partly in the original, so the entry collects pieces from two sources, and
// it is still one entry that comes back in the right order.
TRIAL("Editor: deleting across the boundary of typed text is undone in one step") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(3);
    one->insert("XY");
    one->cursor(5);

    for (int step = 0; step < 4; ++step) one->back_erase(1);

    SAME(one->text(), std::string{"adef"});

    one->undo();

    SAME(one->text(), std::string{"abcXYdef"});

    one->redo();

    SAME(one->text(), std::string{"adef"});

    one->undo();
    one->cursor(2);

    for (int step = 0; step < 4; ++step) one->front_erase(1);

    SAME(one->text(), std::string{"abef"});

    one->undo();

    SAME(one->text(), std::string{"abcXYdef"});

    one->undo();

    SAME(one->text(), std::string{"abcdef"});
    OUGHT(!one->undoable());
}

// Journal entries are scraps of the quilt. Random typing, deleting in both directions and blocks
// (F5, F6, F8), then undoing everything step by step and redoing everything: after each step the
// text is as it was. The scraps of a copied and moved block point to the same pieces as the
// document, so if any change damaged them, it would show on undo.
TRIAL("Editor: random blocks and deletions are undone and redone") {
    std::mt19937 gen(5);

    const auto drawn = [&gen](std::size_t much) { return much == 0 ? 0 : gen() % (much + 1); };

    for (int round = 0; round < 60; ++round) {
        Sheet sheet;

        const std::string was = many(40);

        const auto one = shown(sheet, was);

        ui::Editor::Block& block = one->block();

        std::vector<std::string> states{was};

        for (int step = 0; step < 15; ++step) {
            const std::string before = one->text();
            const std::size_t much = before.size();
            const unsigned what = gen() % 6;

            if (what == 0 || much < 2) {
                std::string more;

                const int size = 1 + static_cast<int>(gen() % 6);

                for (int which = 0; which < size; ++which)
                    more.push_back(gen() % 5 == 0 ? '\n' : static_cast<char>('A' + gen() % 26));

                one->cursor(drawn(much));
                one->insert(more);
            } else if (what == 1) {
                one->cursor(drawn(much));
                one->back_erase(1 + gen() % 20);
            } else if (what == 2) {
                one->cursor(drawn(much));
                one->front_erase(1 + gen() % 20);
            } else {
                const std::size_t from = gen() % (much - 1);
                const std::size_t upto = from + 1 + gen() % (much - from - 1);

                block.clear();
                one->cursor(from);
                block.mark();
                one->cursor(upto);
                block.mark();
                one->cursor(drawn(much));

                if (what == 3) (void)block.copy();
                if (what == 4) (void)block.move();
                if (what == 5) (void)block.erase();
            }

            if (one->text() != before) states.push_back(one->text());
        }

        for (std::size_t at = states.size() - 1; at > 0; --at) {
            one->undo();

            SAME(one->text(), states[at - 1]);
        }

        OUGHT(!one->undoable());

        for (std::size_t at = 1; at < states.size(); ++at) {
            one->redo();

            SAME(one->text(), states[at]);
        }

        OUGHT(!one->redoable());
    }
}

// A large block deleted, moved and copied, then undone and redone many times: the text comes back
// to the byte. Undo must not insert a copy of the block into the quilt's additions, which never
// shrink.
TRIAL("Editor: a large block is undone and redone many times") {
    Sheet sheet;

    const std::string was = many(100000);

    const auto one = shown(sheet, was);

    ui::Editor::Block& block = one->block();

    one->cursor(10);
    block.mark();
    one->cursor(was.size() - 10);
    block.mark();

    one->cursor(was.size());

    OUGHT(block.copy());

    const std::string copied = one->text();

    OUGHT(block.erase());

    const std::string erased = one->text();

    SAME(erased, was.substr(0, 10) + was.substr(was.size() - 10) + was.substr(10, was.size() - 20));

    for (int round = 0; round < 20; ++round) {
        one->undo();

        SAME(one->text().size(), copied.size());

        one->undo();

        SAME(one->text(), was);

        one->redo();
        one->redo();

        SAME(one->text(), erased);
    }
}

// A newline deleted with Backspace or Delete starts a new undo step, just like a typed one. Undo
// then first takes back the joining of the lines, and only then the characters before it.
TRIAL("Editor: a deleted line end is a separate step") {
    Sheet sheet;

    const auto one = shown(sheet, "ab\ncd");

    one->cursor(5);
    one->back_erase(1);
    one->back_erase(1);
    one->back_erase(1);

    SAME(one->text(), std::string{"ab"});

    one->undo();

    SAME(one->text(), std::string{"ab\n"});

    one->undo();

    SAME(one->text(), std::string{"ab\ncd"});

    one->cursor(0);
    one->front_erase(1);
    one->front_erase(1);
    one->front_erase(1);

    SAME(one->text(), std::string{"cd"});

    one->undo();

    SAME(one->text(), std::string{"\ncd"});
}

// --- Long lines and columns ----------------------------------------------------------------------

// Going down keeps the column on the screen, not the byte: a line of two-byte letters is entered at
// the same column, past a shorter line the column comes back, and the cursor never lands in the
// middle of a letter.
TRIAL("Editor: the vertical arrows keep the column, not the byte") {
    Sheet sheet;

    const auto one = shown(sheet, "abcd\n\xc4\x85\xc4\x85xyz\nab\nabcd");

    one->focus();

    (void)sheet.render({{0, 0}, {20, 5}});

    one->cursor(3);

    ui::KeyEvent down{ui::Code::down};

    OUGHT(sheet.dispatch(down));
    SAME(one->cursor(), std::size_t{5 + 5});
    SAME(one->column(), std::size_t{3});

    OUGHT(sheet.dispatch(down));
    SAME(one->cursor(), std::size_t{13 + 2});

    OUGHT(sheet.dispatch(down));
    SAME(one->cursor(), std::size_t{16 + 3});
    SAME(one->column(), std::size_t{3});
}

// The column of the cursor deep in a long line is counted from the remembered columns, and agrees
// with counting from the start of the line.
TRIAL("Editor: the column deep in a long line") {
    Sheet sheet;

    std::string text;

    for (int at = 0; at < 20000; ++at) text += at % 7 == 0 ? "\t" : "\xc4\x85";

    const auto one = shown(sheet, text);

    std::size_t column = 0;
    std::size_t byte = 0;

    for (int at = 0; at < 20000; ++at) {
        if (at % 997 == 0) {
            one->cursor(byte);

            SAME(one->column(), column);
        }

        if (at % 7 == 0) {
            column = (column / 8 + 1) * 8;
            byte += 1;
        } else {
            column += 1;
            byte += 2;
        }
    }
}

// Without wrapping the view follows the cursor sideways: past the right edge the view moves so that
// the cursor stands in its last column, and the caret is on the screen.
TRIAL("Editor: without wrapping the view follows the cursor sideways") {
    Sheet sheet;

    const auto one = shown(sheet, std::string(100, 'a') + "b\nkoniec");

    one->wrapped(false);
    one->focus();

    (void)sheet.render({{0, 0}, {10, 3}});

    one->cursor(100);

    const Snapshot made = sheet.render({{0, 0}, {10, 3}});

    SAME(one->left(), std::size_t{91});
    SAME(said(made, 0), std::string{"aaaaaaaaab"});
    SAME(*one->caret(), (ui::Spot{9, 0}));

    one->cursor(0);

    (void)sheet.render({{0, 0}, {10, 3}});

    SAME(one->left(), std::size_t{0});
}

// A line longer than the search windows: a hit past them is found at its byte, a hit lying across
// the boundary of two windows too, and "$" does not match at the end of a window, only at the end
// of the line.
TRIAL("Editor: searching a line longer than the search windows") {
    Sheet sheet;

    const std::string text = std::string(64 * 1024 - 2, 'x') + "igla" + std::string(300 * 1024, 'x') + "igla" + "b";

    const auto one = shown(sheet, text);

    const Comb plain{"igla", Sift{false, false, false}};

    SAME(one->find(plain, 0, false)->from, std::size_t{64 * 1024 - 2});
    SAME(one->find(plain, 64 * 1024, false)->from, text.size() - 5);
    SAME(one->find(plain, text.size(), true)->from, text.size() - 5);

    const Comb pattern{"ig.a", Sift{true, false, false}};

    SAME(one->find(pattern, 64 * 1024, false)->from, text.size() - 5);

    const Comb end{"x$", Sift{true, false, false}};

    OUGHT(!one->find(end, 0, false).has_value());

    const Comb last{"b$", Sift{true, false, false}};

    SAME(one->find(last, 0, false)->from, text.size() - 1);
}

// "Replace all" in a long line finds every hit exactly once, those across window boundaries
// included.
TRIAL("Editor: replacing all in a long line takes every hit once") {
    Sheet sheet;

    std::string text;

    for (int at = 0; at < 3000; ++at) text += std::string(97, 'x') + "abc";

    const auto one = shown(sheet, text);

    const Comb how{"abc", Sift{false, false, false}};

    SAME(one->replace_all(how, "d"), std::size_t{3000});
    SAME(one->text().size(), std::size_t{3000 * 98});
    OUGHT(one->text().find("abc") == std::string::npos);
}

// With wrapping, the caret of a cursor in the second row of a line stands in that row, where the
// character is drawn, not at the end of the first row.
TRIAL("Editor: the caret in a wrapped line stands in the row of its character") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdefghij");

    one->focus();

    (void)sheet.render({{0, 0}, {4, 3}});

    one->cursor(6);

    (void)sheet.render({{0, 0}, {4, 3}});

    SAME(*one->caret(), (ui::Spot{2, 1}));

    one->cursor(10);

    (void)sheet.render({{0, 0}, {4, 3}});

    SAME(*one->caret(), (ui::Spot{2, 2}));
}

// With wrapping the vertical arrows go by screen rows: a wrapped line is walked row by row at the
// same column, and past the last row of the document the key goes up to an ancestor.
TRIAL("Editor: with wrapping the arrows go by screen rows") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdefghij");

    one->focus();

    (void)sheet.render({{0, 0}, {4, 3}});

    one->cursor(1);

    ui::KeyEvent down{ui::Code::down};
    ui::KeyEvent up{ui::Code::up};

    OUGHT(sheet.dispatch(down));
    SAME(one->cursor(), std::size_t{5});

    OUGHT(sheet.dispatch(down));
    SAME(one->cursor(), std::size_t{9});

    ui::KeyEvent again{ui::Code::down};

    OUGHT(!sheet.dispatch(again));
    SAME(one->cursor(), std::size_t{9});

    OUGHT(sheet.dispatch(up));
    SAME(one->cursor(), std::size_t{5});

    OUGHT(sheet.dispatch(up));
    SAME(one->cursor(), std::size_t{1});
}

// A row that does not end its line ends where the next row starts, so a column past its end lands
// on its last character, not on the byte after it, which would be drawn on the next row. Here a
// double-width character does not fit at the end of the first row of the second line.
TRIAL("Editor: past the end of a row that does not end its line the cursor stays in the row") {
    Sheet sheet;

    const auto one = shown(sheet, "wxyz\nabc\xe6\xbc\xa2"
                                  "d");

    one->focus();

    (void)sheet.render({{0, 0}, {4, 4}});

    one->cursor(3);

    ui::KeyEvent down{ui::Code::down};

    OUGHT(sheet.dispatch(down));
    SAME(one->cursor(), std::size_t{7});

    OUGHT(sheet.dispatch(down));
    SAME(one->cursor(), std::size_t{12});
}

// A page key goes by the height of the screen in rows, also inside one long wrapped line.
TRIAL("Editor: a page goes by screen rows") {
    Sheet sheet;

    const auto one = shown(sheet, std::string(40, 'a'));

    one->focus();

    (void)sheet.render({{0, 0}, {4, 3}});

    ui::KeyEvent down{ui::Code::pagedown};

    OUGHT(sheet.dispatch(down));
    SAME(one->cursor(), std::size_t{12});

    ui::KeyEvent up{ui::Code::pageup};

    OUGHT(sheet.dispatch(up));
    SAME(one->cursor(), std::size_t{0});
}
