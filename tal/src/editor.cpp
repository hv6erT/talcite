#include "tal/pages.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "tal/key.hpp"
#include "tal/text.hpp"

#include "spooling.hpp"

// An editor is a page over a text it changes. Its cursor, the anchor of its selection and the ends
// of its block are places in the text, and every change of the text, from this editor or another
// one, comes back as `Edited` and moves them along, as it moves the page's rows; so two editors of
// one text never point past it. A change is one step of the history; the editors of one text share
// one history, since a step knows the text, not the editor.
namespace tal {

namespace {

// What a change is, for joining the steps that come after it: typing, erasing back as Backspace
// does, erasing forth as Delete does, or anything else, which joins nothing.
enum class Doing { typing, back, forth, other };

// The place where `what`, standing at `at`, ends: its lines are broken at \n.
Place ended(Place at, std::string_view what) {
    const std::size_t last = what.rfind('\n');

    if (last == std::string_view::npos) return Place{at.line, at.byte + what.size()};

    return Place{at.line + static_cast<std::size_t>(std::count(what.begin(), what.end(), '\n')), what.size() - last - 1};
}

// `what` with \r\n turned into \n, as a text takes it, so that what goes in is what stands there.
std::string unified(std::string_view what) {
    std::string made;

    for (std::size_t at = 0; at < what.size(); ++at)
        if (what[at] != '\r' || at + 1 >= what.size() || what[at + 1] != '\n') made.push_back(what[at]);

    return made;
}

// Where the place `at` goes after `what`: one before the change stays, one after it moves with the
// text after it, and one inside goes to its start. A place exactly at the start of what came stays
// before it, unless it leans `forward`, as the start of a block does, so that what is put there
// goes in front of the block.
Place shifted(Place at, const Edited& what, bool forward) {
    if (at < what.from || (at == what.from && !forward)) return at;

    if (at < what.upto) return what.from;

    if (at.line == what.upto.line) return Place{what.end.line, what.end.byte + (at.byte - what.upto.byte)};

    return Place{at.line - what.upto.line + what.end.line, at.byte};
}

// Whether the byte `at` of `line` starts a character or ends the line.
bool bordered(std::string_view line, std::size_t at) {
    return at >= line.size() || (static_cast<unsigned char>(line[at]) & 0xC0) != 0x80;
}

// Whether the cluster at the byte `at` of `line` starts with a character of a word.
bool lettered(std::string_view line, std::size_t at) {
    std::size_t next = at;
    const std::optional<char32_t> one = text::decoded(line, next);

    return one && text::in_word(*one);
}

}

// The editor's own state: its text, history and clipboard, the spool its page reads, the block,
// the column the up and down keys keep, and the end of the last change heard, where a change of
// its own or an undo leaves the cursor.
struct Editor::Held {
    Held(std::shared_ptr<Text> given, std::shared_ptr<Undo> steps, Port<Clipboard> board)
        : text(std::move(given)), undo(std::move(steps)), clipboard(std::move(board)) {}

    std::shared_ptr<Text> text;
    std::shared_ptr<Undo> undo;
    Port<Clipboard> clipboard;
    std::shared_ptr<spooling::Lines> lines;
    Hook told;
    std::optional<Range> block;
    bool marking = false;
    std::optional<std::size_t> column;
    std::optional<Place> landing;
    bool inner = false;
    bool dragging = false;
};

// One change of a text by an editor, as a step of the history: pieces done one after another, each
// putting `came` where `gone` stood at `from`; undoing takes them back the other way round. A step
// done right after it joins it while typing or erasing goes on: typing without a space, a tab or a
// line end where the last typing ended, as in a field, or erasing the character before or at the
// place of the last erasing, inside a line. A piece that no longer finds its text there, since the
// text was changed past the history, does nothing and stops the rest.
struct Editor::Change : Step {
    struct Piece {
        Place from;
        std::string gone;
        std::string came;
    };

    Change(std::shared_ptr<Text> given, Doing made, std::string named) : text(std::move(given)), kind(made), name(std::move(named)) {}

    // Takes the pieces back, the last first.
    void undo() override {
        for (auto one = pieces.rbegin(); one != pieces.rend(); ++one)
            if (!swapped(one->from, one->came, one->gone)) return;
    }

    // Does the pieces, the first first.
    void redo() override {
        for (const Piece& one : pieces)
            if (!swapped(one.from, one.gone, one.came)) return;
    }

    // What the change was, for a menu.
    [[nodiscard]] std::string said() const override { return name; }

    // Takes in `later` while typing or erasing goes on; a change of either holds one piece.
    bool merged(const Step& later) override {
        const auto* next = dynamic_cast<const Change*>(&later);

        if (next == nullptr || next->kind != kind || next->text != text) return false;

        Piece& last = pieces.front();
        const Piece& one = next->pieces.front();

        switch (kind) {
            case Doing::typing:
                if (!one.gone.empty() || one.came.find_first_of(" \t\n") != std::string::npos || one.from != ended(last.from, last.came)) return false;

                last.came += one.came;

                return true;
            case Doing::back:
                if ((one.gone + last.gone).find('\n') != std::string::npos || ended(one.from, one.gone) != last.from) return false;

                last.from = one.from;
                last.gone = one.gone + last.gone;

                return true;
            case Doing::forth:
                if ((last.gone + one.gone).find('\n') != std::string::npos || one.from != last.from) return false;

                last.gone += one.gone;

                return true;
            case Doing::other: return false;
        }

        return false;
    }

    // Puts `now` where `was` stands at `from`; false, changing nothing, when `was` does not stand
    // there. `now` goes in first and `was` goes after it, so that a place at the end of `was` ends
    // up at the end of `now`, and one at its start stays in front.
    bool swapped(Place from, const std::string& was, const std::string& now) {
        try {
            if (text->between(from, ended(from, was)) != was) return false;
        } catch (const std::logic_error&) {
            return false;
        }

        const Place after = now.empty() ? from : text->insert(from, now);

        text->erase(after, ended(after, was));

        return true;
    }

    std::shared_ptr<Text> text;
    Doing kind;
    std::string name;
    std::vector<Piece> pieces;
};

// An editor of `text`, in the class "editor" too, with the history `undo` and the clipboard
// `clipboard`; the cursor at the start, nothing selected, without wrapping. It takes typing, Enter,
// Backspace, Delete (not with Shift, Midnight Commander's cut) and the moves: the arrows, Ctrl with
// the arrows to the sides by words, the page keys, Home and End; with Shift a move selects. Tab and
// every key with Alt or with Ctrl otherwise are left to the program, which binds the history, the
// block and the clipboard to keys of its own. A press puts the cursor and takes the focus, a drag
// selects (going past the edge moves the view a row a step), Shift with a press stretches the
// selection and a double click selects a word. A text or a history that is none throws
// std::invalid_argument.
Editor::Editor(std::shared_ptr<Text> text, std::shared_ptr<Undo> undo, Port<Clipboard> clipboard)
    : held_(std::make_unique<Held>(std::move(text), std::move(undo), std::move(clipboard))) {
    Held& held = *held_;

    if (held.text == nullptr) throw std::invalid_argument{"tal::Editor: no text"};

    if (held.undo == nullptr) throw std::invalid_argument{"tal::Editor: no history"};

    add_class("editor");

    held.lines = std::make_shared<spooling::Lines>(held.text);

    hold(held.lines);

    held.told = held.text->on<Edited>([this](Edited& what) { heard(what); });

    hooks_.add(on<Changed>([this](Changed& what) {
        Held& kept = *held_;

        if (what.of(wrap) || what.of(tab)) kept.column.reset();

        if (kept.inner || (!what.of(cursor) && !what.of(anchor))) return;

        stood(fitted(*cursor), *anchor ? std::optional<Place>{fitted(**anchor)} : std::nullopt);

        kept.column.reset();
        kept.undo->seal();

        settled();
    }));

    hooks_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        Held& kept = *held_;

        if (what.press == Press::up) {
            if (!kept.dragging) return;

            kept.dragging = false;

            if (*anchor == std::optional<Place>{*cursor}) stood(*cursor, std::nullopt);

            return;
        }

        if (what.press == Press::move) {
            if (!kept.dragging) return;

            what.take();
            dragged(what.at);

            return;
        }

        if (what.button != MouseButton::left || (what.press != Press::down && what.press != Press::double_click)) return;

        what.take();

        const Place place = aimed(what.at);

        if (what.press == Press::double_click) {
            worded(place);

            return;
        }

        focus();
        capture();

        kept.dragging = true;

        if (what.mods.shift) {
            went(place, true);

            return;
        }

        went(place, false);
    }));

    hooks_.add(otherwise<PasteEvent>([this](PasteEvent& what) {
        what.take();
        insert(what.text);
    }));
}

// An editor goes with its hook on the text.
Editor::~Editor() = default;

// The selection, from its start to its end whichever way it was made; none when the anchor is none
// or stands at the cursor.
std::optional<Range> Editor::selected() const {
    if (!*anchor || **anchor == *cursor) return std::nullopt;

    return Range{std::min(**anchor, *cursor), std::max(**anchor, *cursor)};
}

// The column of the cursor in its line, in cells as drawn from the line's start: a tab to the next
// multiple of `tab`, a wide character two.
int Editor::column() const {
    const std::string_view line = held_->text->line(cursor->line);
    const text::Rendering how = rendering();
    const int step = std::max(*tab, 1);
    int cells = 0;

    for (std::size_t at = 0; at < cursor->byte;) {
        const std::size_t next = text::after(line, at, how);
        const std::string_view sign = line.substr(at, next - at);

        cells = sign == "\t" ? (cells / step + 1) * step : cells + text::width(sign, how);
        at = next;
    }

    return cells;
}

// Puts the cursor at the start of the line `which`, the last line past it, with nothing selected.
void Editor::line(std::size_t which) { went(Place{std::min(which, held_->text->lines() - 1), 0}, false); }

// Puts `what` where the selection is, or at the cursor, as one step of the history that joins no
// other; the cursor goes after it. Nothing while read-only, and nothing put in place of nothing is
// no step.
void Editor::insert(std::string_view what) {
    Held& held = *held_;

    if (!editable()) return;

    const std::string cleaned = unified(what);
    const Range where = selected().value_or(Range{*cursor, *cursor});
    auto one = std::make_unique<Change>(held.text, Doing::other, "Insert");

    one->pieces.push_back(Change::Piece{where.from, held.text->between(where.from, where.upto), cleaned});

    (void)made(std::move(one), true);
}

// Undoes the last step of the history; the cursor goes after what came back, with nothing selected.
// False while read-only or without a step.
bool Editor::undo() {
    Held& held = *held_;

    if (!editable()) return false;

    held.landing.reset();

    if (!held.undo->undo()) return false;

    if (held.landing) stood(*held.landing, std::nullopt);

    settled();

    return true;
}

// Does again the step undone last; the cursor goes after it. False while read-only or without one.
bool Editor::redo() {
    Held& held = *held_;

    if (!editable()) return false;

    held.landing.reset();

    if (!held.undo->redo()) return false;

    if (held.landing) stood(*held.landing, std::nullopt);

    settled();

    return true;
}

// The history, for its menu texts and its saved place.
Undo& Editor::history() const { return *held_->undo; }

// Copies the selection to the clipboard; false without one or when the clipboard refused it.
Task<bool> Editor::copy() {
    const std::optional<Range> range = selected();

    if (!range) co_return false;

    const Port<Clipboard> board = held_->clipboard;

    co_return co_await board->copy(held_->text->between(range->from, range->upto));
}

// Copies the selection to the clipboard and takes it away as one step; nothing is taken when the
// clipboard refused it, or when the selection no longer holds what was copied by the time it took
// it, nor by an editor that went meanwhile. False while read-only too.
Task<bool> Editor::cut() {
    const std::optional<Range> range = selected();

    if (!range || !editable()) co_return false;

    const std::string what = held_->text->between(range->from, range->upto);
    const Port<Clipboard> board = held_->clipboard;
    const std::weak_ptr<Node> alive = weak_from_this();
    const bool owned = !alive.expired();

    if (!co_await board->copy(what) || (owned && alive.expired())) co_return false;

    const std::optional<Range> now = selected();

    if (!now || !editable() || held_->text->between(now->from, now->upto) != what) co_return false;

    auto one = std::make_unique<Change>(held_->text, Doing::other, "Cut");

    one->pieces.push_back(Change::Piece{now->from, what, {}});

    co_return made(std::move(one), true);
}

// Puts the clipboard's text where the selection is, or at the cursor, as `insert` does; false while
// read-only, with nothing in the clipboard, or for an editor that went meanwhile.
Task<bool> Editor::paste() {
    if (!editable()) co_return false;

    const Port<Clipboard> board = held_->clipboard;
    const std::weak_ptr<Node> alive = weak_from_this();
    const bool owned = !alive.expired();
    const std::optional<std::string> got = co_await board->paste();

    if ((owned && alive.expired()) || !got || got->empty() || !editable()) co_return false;

    insert(*got);

    co_return true;
}

// Marks the block as F3 does in Midnight Commander: without a block it starts one at the cursor,
// which stretches it as it moves; the next mark ends it there (a block of nothing goes); after that
// a mark starts another one.
void Editor::mark() {
    Held& held = *held_;

    if (held.marking) {
        blocked();
    } else {
        held.block = Range{*cursor, *cursor};
        held.marking = true;
    }

    settled();
}

// The block, its ends in order: while it is being marked, from its start to the cursor.
std::optional<Range> Editor::block() const {
    const Held& held = *held_;

    if (!held.block) return std::nullopt;

    if (!held.marking) return held.block;

    return Range{std::min(held.block->from, *cursor), std::max(held.block->from, *cursor)};
}

// Drops the block, the text staying as it is.
void Editor::unblock() {
    held_->block.reset();
    held_->marking = false;

    settled();
}

// Puts a copy of the block at the cursor as one step, the cursor after it; the block stays and
// moves along when it lies after the cursor. A block being marked ends first. False while read-only
// or without a block.
bool Editor::copy_block() {
    Held& held = *held_;

    if (!editable()) return false;

    blocked();

    if (!held.block) return false;

    auto one = std::make_unique<Change>(held.text, Doing::other, "Copy block");

    one->pieces.push_back(Change::Piece{*cursor, {}, held.text->between(held.block->from, held.block->upto)});

    return made(std::move(one), true);
}

// Moves the block to the cursor as one step; the block then covers it there and the cursor stands
// after it. A block being marked ends first. False while read-only, without a block, or with the
// cursor in the block or at its edge.
bool Editor::move_block() {
    Held& held = *held_;

    if (!editable()) return false;

    blocked();

    if (!held.block || (held.block->from <= *cursor && *cursor <= held.block->upto)) return false;

    const Range was = *held.block;
    const std::string what = held.text->between(was.from, was.upto);
    const Place target = shifted(*cursor, Edited{was.from, was.upto, was.from}, false);
    auto one = std::make_unique<Change>(held.text, Doing::other, "Move block");

    one->pieces.push_back(Change::Piece{was.from, what, {}});
    one->pieces.push_back(Change::Piece{target, {}, what});

    if (!made(std::move(one), true)) return false;

    held.block = Range{target, *cursor};

    settled();

    return true;
}

// Takes the block away as one step: a cursor after it moves back, one inside goes to its start. A
// block being marked ends first. False while read-only or without a block.
bool Editor::erase_block() {
    Held& held = *held_;

    if (!editable()) return false;

    blocked();

    if (!held.block) return false;

    auto one = std::make_unique<Change>(held.text, Doing::other, "Erase block");

    one->pieces.push_back(Change::Piece{held.block->from, held.text->between(held.block->from, held.block->upto), {}});

    return made(std::move(one), false);
}

// Finds `how` from the cursor on, or back before it: forth from the end of the selection, back from
// its start, so that finding again goes on past the hit. The hit is selected, the cursor at its end
// the way it went, and shown. The search does not go round the end. False without a hit or with a
// comb that is not ready.
bool Editor::find(const Comb& how, bool back) {
    if (!how.ready()) return false;

    const std::optional<Range> range = selected();
    const Place from = range ? (back ? range->from : range->upto) : *cursor;
    const std::optional<Range> got = hit(how, from, back);

    if (!got) return false;

    stood(back ? got->from : got->upto, back ? got->upto : got->from);

    held_->column.reset();

    settled();

    return true;
}

// Puts `with` in place of the selection when it is a hit of `how`, as one step, and finds the next
// hit; otherwise only finds it. Gives whether something was put; false while read-only.
bool Editor::replace(const Comb& how, std::string_view with) {
    Held& held = *held_;

    if (!editable() || !how.ready()) return false;

    const std::optional<Range> range = selected();
    bool done = false;

    if (range && range->from.line == range->upto.line) {
        const std::optional<Hit> one = how.forth(held.text->line(range->from.line), range->from.byte);

        if (one && one->from == range->from.byte && one->upto == range->upto.byte) {
            auto change = std::make_unique<Change>(held.text, Doing::other, "Replace");

            change->pieces.push_back(Change::Piece{range->from, held.text->between(range->from, range->upto), unified(with)});

            done = made(std::move(change), true);
        }
    }

    (void)find(how, false);

    return done;
}

// Puts `with` in place of every hit of `how` in the text as one step, the hits sought in the text
// as it was, so that what is put is never sought; gives how many, and no hit is no step. The pieces
// go from the last hit to the first, so that each stands where it was found. Nothing while
// read-only.
std::size_t Editor::replace_all(const Comb& how, std::string_view with) {
    Held& held = *held_;

    if (!editable() || !how.ready()) return 0;

    const std::string put = unified(with);
    auto change = std::make_unique<Change>(held.text, Doing::other, "Replace all");

    for (std::size_t line = held.text->lines(); line-- > 0;) {
        const std::string_view said = held.text->line(line);
        const std::vector<Hit> hits = how.all(said);

        for (auto one = hits.rbegin(); one != hits.rend(); ++one)
            change->pieces.push_back(Change::Piece{Place{line, one->from}, std::string{said.substr(one->from, one->upto - one->from)}, put});
    }

    const std::size_t many = change->pieces.size();

    (void)made(std::move(change), false);

    return many;
}

// The editor's keys, as the constructor tells; keys it does not take go on to the nodes above, the
// up and down keys too when there is no row to go to.
void Editor::keyed(KeyEvent& what) {
    Held& held = *held_;
    const Key key = what.key;
    const Mods mods = key.mods();
    const Code code = key.code();

    if (mods.alt) return;

    if (mods.ctrl) {
        if (code != Code::left && code != Code::right) return;

        went(word(*cursor, code == Code::right), mods.shift);
        what.take();

        return;
    }

    switch (code) {
        case Code::none: {
            if (key.letter() == 0) return;

            std::string one;

            text::encoded(one, key.letter());
            typed(one);

            break;
        }
        case Code::enter: typed("\n"); break;
        case Code::backspace: erased(false); break;
        case Code::remove:
            if (mods.shift) return;

            erased(true);
            break;
        case Code::left:
        case Code::right: went(stepped(*cursor, code == Code::right), mods.shift); break;
        case Code::up:
        case Code::down:
        case Code::pageup:
        case Code::pagedown: {
            const int much = code == Code::up || code == Code::down ? 1 : high();
            const int rows = code == Code::up || code == Code::pageup ? -much : much;
            const std::size_t column = held.column ? *held.column : across(*cursor).value_or(0);
            const std::optional<Place> to = beyond(*cursor, rows, column);

            if (!to) return;

            went(*to, mods.shift);

            held.column = column;

            break;
        }
        case Code::home: went(Place{cursor->line, 0}, mods.shift); break;
        case Code::end: went(Place{cursor->line, held.text->line(cursor->line).size()}, mods.shift); break;
        default: return;
    }

    what.take();
}

// The cursor's cell, where the page shows it.
std::optional<Caret> Editor::caret() const {
    const std::optional<Spot> at = spot(*cursor);

    if (!at) return std::nullopt;

    return Caret{*at};
}

// Follows a change of the text: the spool, the cursor, the anchor and the block go along (the block
// goes when nothing is left of it, unless it is being marked), the page lays the changed lines out
// again, and the end of the change is kept for the cursor of a change of this editor's own.
void Editor::heard(const Edited& what) {
    Held& held = *held_;

    held.lines->edited(what.from.line, what.upto.line, what.end.line);

    stood(shifted(*cursor, what, false), *anchor ? std::optional<Place>{shifted(**anchor, what, false)} : std::nullopt);

    if (held.block) {
        held.block->from = shifted(held.block->from, what, true);

        if (!held.marking) {
            held.block->upto = shifted(held.block->upto, what, false);

            if (held.block->upto <= held.block->from) held.block.reset();
        }
    }

    held.landing = what.end;
    held.column.reset();

    edited(what.from.line);
    lighted();
}

// Does the change `one` and, when the text changed, keeps it in the history; with `landing` the
// cursor goes to the end of what it did last, otherwise it stays where the change moved it, and
// nothing is selected. Gives whether the text changed.
bool Editor::made(std::unique_ptr<Change> one, bool landing) {
    Held& held = *held_;

    held.landing.reset();

    one->redo();

    if (!held.landing) return false;

    held.undo->push(std::move(one));

    stood(landing ? *held.landing : *cursor, std::nullopt);

    settled();

    return true;
}

// Types `what` at the cursor, over the selection, as typing that joins the typing before it.
void Editor::typed(std::string_view what) {
    Held& held = *held_;

    if (!editable()) return;

    const Range where = selected().value_or(Range{*cursor, *cursor});
    auto one = std::make_unique<Change>(held.text, Doing::typing, "Typing");

    one->pieces.push_back(Change::Piece{where.from, held.text->between(where.from, where.upto), std::string{what}});

    (void)made(std::move(one), true);
}

// Erases the selection, or else the character after the cursor (`forth`) or before it, a line end
// too, as erasing that joins the erasing before it; nothing past the ends of the text.
void Editor::erased(bool forth) {
    Held& held = *held_;

    if (!editable()) return;

    std::optional<Range> where = selected();
    Doing kind = Doing::other;

    if (!where) {
        const Place other = stepped(*cursor, forth);

        where = Range{std::min(other, *cursor), std::max(other, *cursor)};
        kind = forth ? Doing::forth : Doing::back;
    }

    auto one = std::make_unique<Change>(held.text, kind, "Erasing");

    one->pieces.push_back(Change::Piece{where->from, held.text->between(where->from, where->upto), {}});

    (void)made(std::move(one), true);
}

// Moves the cursor to `to`: `stretching` the selection from where the cursor stood, otherwise
// selecting nothing. Typing after it joins no typing before it.
void Editor::went(Place to, bool stretching) {
    Held& held = *held_;

    stood(to, stretching ? std::optional<Place>{(*anchor).value_or(*cursor)} : std::nullopt);

    held.column.reset();
    held.undo->seal();

    settled();
}

// Sets the cursor and the anchor as the editor's own doing, which its handler of `Changed` lets be.
void Editor::stood(Place to, std::optional<Place> from) {
    Held& held = *held_;

    held.inner = true;
    cursor = to;
    anchor = from;
    held.inner = false;
}

// Lights the selection and the block, and paints again, for the cursor too.
void Editor::lighted() {
    std::vector<Range> all;

    if (const std::optional<Range> one = selected()) all.push_back(*one);

    if (const std::optional<Range> one = block()) all.push_back(*one);

    lit(std::move(all));
    repaint();
}

// Lights the selection and the block and shows the cursor, after the editor moved it or changed the
// text; a change from elsewhere leaves the view where it is.
void Editor::settled() {
    lighted();
    show(*cursor);
}

// The place for the cell `at` of the box: the cell is taken inside the rows first, and under the
// last row stands the end of the text.
Place Editor::aimed(Spot at) {
    const Style& style = this->style();
    const int line = style.border == Border::none ? 0 : 1;
    const int left = line + style.padding.left;
    const int top = line + style.padding.top;
    const int wide = std::max(area().size.wide - 2 * line - style.padding.left - style.padding.right, 1);
    const Spot inside{std::clamp(at.x, left, left + wide - 1), std::clamp(at.y, top, top + high() - 1)};

    if (const std::optional<Place> got = under(inside)) return *got;

    const std::size_t last = held_->text->lines() - 1;

    return Place{last, held_->text->line(last).size()};
}

// Stretches the selection to the cell `at` while the mouse drags; past the top or the bottom the
// view goes a row that way, as far as the mouse goes on moving.
void Editor::dragged(Spot at) {
    const Style& style = this->style();
    const int top = (style.border == Border::none ? 0 : 1) + style.padding.top;
    const int y = at.y - top;
    const Place place = aimed(at);

    if (y >= 0 && y < high()) {
        went(place, true);

        return;
    }

    const std::optional<Place> past = beyond(place, y < 0 ? -1 : 1, across(place).value_or(0));

    went(past.value_or(place), true);
}

// Selects the word at `at`, or the run of spaces or of other characters there: the cluster at the
// place, the one before it at the end of the line. The press before a double click took the column
// kept for the up and down keys already.
void Editor::worded(Place at) {
    const std::string_view line = held_->text->line(at.line);
    const text::Rendering how = rendering();
    const std::size_t here = at.byte < line.size() ? at.byte : text::before(line, line.size(), how);
    const bool kind = lettered(line, here);
    std::size_t from = here;
    std::size_t upto = text::after(line, here, how);

    while (from > 0 && lettered(line, text::before(line, from, how)) == kind) from = text::before(line, from, how);

    while (upto < line.size() && lettered(line, upto) == kind) upto = text::after(line, upto, how);

    stood(Place{at.line, upto}, Place{at.line, from});

    settled();
}

// Ends marking the block at the cursor, from the start it was marked from: its ends in order, and a
// block of nothing goes.
void Editor::blocked() {
    Held& held = *held_;

    if (!held.marking) return;

    held.marking = false;

    const Range ends = Range{std::min(held.block->from, *cursor), std::max(held.block->from, *cursor)};

    if (ends.from == ends.upto) held.block.reset();
    else held.block = ends;
}

// The place nearest `at` in the text: the last line past it, the line's end past that, the start of
// the character a byte inside it.
Place Editor::fitted(Place at) const {
    const std::size_t line = std::min(at.line, held_->text->lines() - 1);
    const std::string_view said = held_->text->line(line);
    std::size_t byte = std::min(at.byte, said.size());

    while (!bordered(said, byte)) --byte;

    return Place{line, byte};
}

// The place a character after `at` (`forth`) or before it, across the end of a line; `at` itself at
// the end or the start of the text.
Place Editor::stepped(Place at, bool forth) const {
    const Text& text = *held_->text;
    const std::string_view line = text.line(at.line);

    if (forth) {
        if (at.byte < line.size()) return Place{at.line, text::after(line, at.byte, rendering())};

        return at.line + 1 < text.lines() ? Place{at.line + 1, 0} : at;
    }

    if (at.byte > 0) return Place{at.line, text::before(line, at.byte, rendering())};

    return at.line > 0 ? Place{at.line - 1, text.line(at.line - 1).size()} : at;
}

// The end of the word after `at` (`forth`) or the start of the word before it, past what is not a
// word first, as in a field; at the end or the start of a line, the line after or before it.
Place Editor::word(Place at, bool forth) const {
    const std::string_view line = held_->text->line(at.line);
    const text::Rendering how = rendering();
    std::size_t to = at.byte;

    if (forth) {
        if (to >= line.size()) return stepped(at, true);

        while (to < line.size() && !lettered(line, to)) to = text::after(line, to, how);
        while (to < line.size() && lettered(line, to)) to = text::after(line, to, how);

        return Place{at.line, to};
    }

    if (to == 0) return stepped(at, false);

    while (to > 0 && !lettered(line, text::before(line, to, how))) to = text::before(line, to, how);
    while (to > 0 && lettered(line, text::before(line, to, how))) to = text::before(line, to, how);

    return Place{at.line, to};
}

// The rows of the content, one at least: a page key moves the cursor so many rows.
int Editor::high() const {
    const Style& style = this->style();
    const int line = style.border == Border::none ? 0 : 1;

    return std::max(area().size.high - 2 * line - style.padding.top - style.padding.bottom, 1);
}

// The first hit of `how` from `from` on, or the last one starting before it (`back`), line by line;
// a hit lies inside one line.
std::optional<Range> Editor::hit(const Comb& how, Place from, bool back) const {
    const Text& text = *held_->text;

    if (!back) {
        for (std::size_t line = from.line; line < text.lines(); ++line)
            if (const std::optional<Hit> one = how.forth(text.line(line), line == from.line ? from.byte : 0))
                return Range{Place{line, one->from}, Place{line, one->upto}};

        return std::nullopt;
    }

    for (std::size_t line = from.line + 1; line-- > 0;)
        if (const std::optional<Hit> one = how.back(text.line(line), line == from.line ? from.byte : SIZE_MAX))
            return Range{Place{line, one->from}, Place{line, one->upto}};

    return std::nullopt;
}

// Whether the text may be changed: not while read-only.
bool Editor::editable() const { return !*readonly; }

}
