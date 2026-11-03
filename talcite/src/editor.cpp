#include "talcite/editor.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "talcite/clipboard.hpp"
#include "talcite/encoding.hpp"
#include "talcite/root.hpp"

#include "quilt.hpp"
#include "spool.hpp"


namespace talcite::ui {

namespace {

// The selection's role. The selection is not a tag (it has an anchor and a head, which a tag does
// not have), but it is drawn with one, because painting a range is exactly the same work.
constexpr const char* selecting = "talcite.editor.select";

// The number of line feeds in the text.
std::size_t counted(std::string_view what) {
    return static_cast<std::size_t>(std::count(what.begin(), what.end(), '\n'));
}


// Ends an undo group. Space, tab and enter close a word, so one undo takes away the whole word,
// not a letter, and does not jump over the end of a line in doing so.
bool sealed(char last) { return last == '\n' || last == ' ' || last == '\t'; }

}

struct Editor::Kept {
    // One cut of "replace all": its place in the document from before the whole change.
    struct Splice {
        std::size_t at = 0;
        std::string gone;
    };

    // One change of the document: what disappeared, what came and where the cursor stood then. The
    // quilt does not undo itself, so the inverse of every change has to be remembered separately,
    // but as quilt scraps, not bytes. A copy of the deleted text would end up in the additions,
    // which never shrink: deleting a gigabyte block from a log would mean a gigabyte in the log of
    // changes and another one with every undo–redo pair. A scrap is a few spans, so memory grows
    // with the number of changes, not with their length, and undo stitches the document from the
    // same spans.
    struct Change {
        std::size_t at = 0;
        Quilt::Scrap gone;
        Quilt::Scrap came;
        std::size_t was = 0;

        // "Replace all" as one entry: the cuts in ascending order, each with what disappeared, and
        // `with` comes in every place. An empty vector means an ordinary change in one place.
        std::vector<Splice> many;
        std::string with;

        // Undone together with the previous entry. That is how "replace all" comes back in one
        // move, although there were a hundred changes, without keeping the whole document from
        // before the replacement in memory.
        bool tied = false;

        // The mcedit block before and after the change: undo puts the block back where it was
        // (also after `erase()`, after which there is no block), and redo where the change left
        // it.
        std::optional<std::pair<std::size_t, std::size_t>> before;
        std::optional<std::pair<std::size_t, std::size_t>> after;
    };

    using Block = std::optional<std::pair<std::size_t, std::size_t>>;

    // A clipboard given from outside, so that trials do not reach the system one and a program
    // with several editors gives them one shared clipboard.
    Seam<Clipboard> clipboard;

    std::shared_ptr<Quilt> quilt;

    std::size_t cursor = 0;
    std::size_t anchor = 0;

    // The mcedit block: a lasting span of the document next to the selection and the clipboard.
    // While it is being stretched (`marking`) the other end is the cursor, so `block->second` means
    // nothing then; once finished, both ends stand and follow only the document's changes
    // (`apply`). `held` is the state from before the current change, captured in `apply` for
    // `note`.
    Block block;
    bool marking = false;
    Block held;

    // The remembered column for vertical arrows: the column in the cursor's screen row, counted as
    // the page draws it (a tab to the next multiple of eight, a double-width character two). Going
    // down past a short row and on keeps the column, and a row of other characters is entered at
    // the same place on the screen, not at the same byte, which could fall in the middle of a
    // letter.
    std::size_t column = 0;

    // The editor itself, so that a change can tell the page from which line on its memories of
    // rows and columns are no longer true.
    Editor* owner = nullptr;

    bool readonly = false;

    // Whether a save is in progress. The content must not change under the pieces that are just
    // flowing to the sink, so until the save ends the editor is read-only, but without touching
    // `readonly`, which belongs to the caller.
    bool saving = false;

    // Whether a mouse drag is in progress. Motion without a pressed button comes only with
    // `?1003`, which is not turned on, but the flag is needed anyway: motion over the editor after
    // the release must not stretch the selection.
    bool dragging = false;

    std::vector<Change> log;

    // How many log entries are done. Everything past that is the redo branch, which the first new
    // change cuts off.
    std::size_t done = 0;

    // How many there were at the last save. Hence `dirty()`: undoing everything back gives a clean
    // document, because it returns to the same place in the log. Two traps of this count: a word
    // typed on after the save would extend the same entry without changing the number, so saving
    // seals the entry (`joining` goes off); and a new change after undoing below the saved place
    // cuts off the branch that place lay on, so the number goes to `SIZE_MAX`, because the saved
    // state is no longer reachable by any undo.
    std::size_t washed = 0;

    // Whether the next character may extend the last entry. It goes off with every cursor move,
    // with undo and with redo; otherwise a word typed after a jump to another place would stick to
    // the word from before the jump.
    bool joining = false;

    // Whether the next entry is to be tied to the previous one.
    bool tying = false;

    [[nodiscard]] std::string text(std::size_t from, std::size_t upto) const;

    void apply(Spool& spool, std::size_t at, const Quilt::Scrap& gone, const Quilt::Scrap& came);

    void splice(Spool& spool, const std::vector<Splice>& many, const std::string& came, bool back);

    [[nodiscard]] static std::size_t ended(const std::vector<Splice>& many, const std::string& came);

    void note(std::size_t at, Quilt::Scrap gone, Quilt::Scrap came, std::size_t was);

    void blocked();
};

// Ends stretching the block: the ends in order, and a block of zero length disappears, since an
// empty block would only be garbage on the screen. Called by the second F3 and by each of the
// three block actions.
void Editor::Kept::blocked() {
    if (!marking) return;

    marking = false;

    if (!block.has_value()) return;

    const std::size_t from = std::min(block->first, cursor);
    const std::size_t upto = std::max(block->first, cursor);

    if (upto <= from) block.reset();
    else block = std::pair{from, upto};
}

// The document's text between two bytes; empty without a document.
std::string Editor::Kept::text(std::size_t from, std::size_t upto) const {
    if (!quilt || upto <= from) return {};

    return talcite::text(quilt->bytes(from, upto - from));
}

namespace {

// A block bound after the change [at, at + gone) → came. The start: a change before it or exactly
// at it moves it (text inserted at the start of the block goes before the block), a change inside
// puts it after the inserted text. The end: a change after it or exactly at it does nothing (text
// inserted at the end of the block does not enter the block), a change before it moves it, one
// inside cuts it to `at`.
[[nodiscard]] std::size_t shifted(std::size_t bound, bool start, std::size_t at, std::size_t gone,
                                  std::size_t came) {
    if (start ? bound < at : bound <= at) return bound;

    if (bound >= at + gone) return bound - gone + came;

    return start ? at + came : at;
}

}

// The only place where the document really changes. Typing, deleting, undo and redo all come down
// to this, so the correction for the spool is written once, and it decides whether the screen
// shows the new content after the change.
void Editor::Kept::apply(Spool& spool, std::size_t at, const Quilt::Scrap& gone, const Quilt::Scrap& came) {
    // The block follows the document: both ends by `shifted`, and when nothing of it is left, it
    // disappears. While it is being stretched only the start moves; the end is the cursor.
    held = block;

    if (block.has_value()) {
        block->first = shifted(block->first, true, at, gone.much, came.much);

        if (!marking) {
            block->second = shifted(block->second, false, at, gone.much, came.much);

            if (block->second <= block->first) block.reset();
        }
    }

    // The last byte of the document decides whether the last line exists at all: "abc" is one
    // line, "abc\n" is also one, and an empty document has none. The number of line feeds alone
    // does not say that, so the end has to be looked at before and after the change.
    const auto ending = [this](std::size_t much) {
        if (much == 0) return '\0';

        const std::string one = talcite::text(quilt->bytes(much - 1, 1));

        return one.empty() ? '\0' : one.front();
    };

    const std::size_t was = quilt->whole();
    const char before = ending(was);

    quilt->erase(at, gone.much);
    quilt->put(at, came);

    const std::size_t now = quilt->whole();
    const char after = ending(now);

    std::ptrdiff_t lines = static_cast<std::ptrdiff_t>(quilt->lines(came)) -
                           static_cast<std::ptrdiff_t>(quilt->lines(gone));

    if (now > 0 && after != '\n') ++lines;

    if (was > 0 && before != '\n') --lines;

    spool.changed(at, gone.much, came.much, lines);

    if (owner != nullptr) owner->changed(spool.found(at));
}

// The end of the last insertion after the whole change: its place from before the change moved
// by all the earlier cuts.
std::size_t Editor::Kept::ended(const std::vector<Splice>& many, const std::string& came) {
    std::ptrdiff_t moved = 0;

    for (std::size_t one = 0; one + 1 < many.size(); ++one)
        moved += static_cast<std::ptrdiff_t>(came.size()) - static_cast<std::ptrdiff_t>(many[one].gone.size());

    return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(many.back().at) + moved) + came.size();
}

// Many places at once: "replace all" and its undo in one pass over the document. Forward: in the
// place of every `gone` stands `came`; backward: in the place of every `came` (already moved by
// the changes before it) its `gone` returns. Forward, the block follows all the cuts in turn, from
// the left, each at a place already moved by the previous ones, as if they went one after
// another; backward the block is not computed, because undo puts it back from the entry anyway.
// The spool gets one notice from the first cut, because further on each one shifts differently.
void Editor::Kept::splice(Spool& spool, const std::vector<Splice>& many, const std::string& came, bool back) {
    if (many.empty()) return;

    held = block;

    std::vector<Quilt::Cut> cuts;
    cuts.reserve(many.size());

    std::ptrdiff_t moved = 0;
    std::ptrdiff_t lines = 0;

    const std::ptrdiff_t newlines = static_cast<std::ptrdiff_t>(counted(came));

    for (const Splice& one : many) {
        const std::size_t now = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(one.at) + (back ? moved : 0));
        const std::size_t gone = back ? came.size() : one.gone.size();
        const std::size_t comes = back ? one.gone.size() : came.size();

        cuts.push_back(Quilt::Cut{now, gone, back ? std::string_view{one.gone} : std::string_view{came}});

        // The cuts in turn from the left, in a document changed as we go: each stands further than
        // before the change by the sum of the earlier differences.
        if (block.has_value() && !back) {
            const std::size_t here = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(one.at) + moved);

            block->first = shifted(block->first, true, here, gone, comes);

            if (!marking) block->second = shifted(block->second, false, here, gone, comes);
        }

        const std::ptrdiff_t step = static_cast<std::ptrdiff_t>(came.size()) - static_cast<std::ptrdiff_t>(one.gone.size());

        moved += step;

        const std::ptrdiff_t away = static_cast<std::ptrdiff_t>(counted(one.gone));

        lines += back ? away - newlines : newlines - away;
    }

    if (block.has_value() && !marking && block->second <= block->first) block.reset();

    const auto ending = [this](std::size_t much) {
        if (much == 0) return '\0';

        const std::string one = talcite::text(quilt->bytes(much - 1, 1));

        return one.empty() ? '\0' : one.front();
    };

    const std::size_t was = quilt->whole();
    const char before = ending(was);

    quilt->spliced(cuts);

    const std::size_t now = quilt->whole();
    const char after = ending(now);

    if (now > 0 && after != '\n') ++lines;

    if (was > 0 && before != '\n') --lines;

    spool.rewritten(cuts.front().at, static_cast<std::ptrdiff_t>(now) - static_cast<std::ptrdiff_t>(was), lines);

    if (owner != nullptr) owner->changed(spool.found(cuts.front().at));
}

// A new change cuts off the redo branch: after undoing and typing something else there is nowhere
// to redo to. Besides, a typed word gathers into one entry: a character added exactly where the
// previous one ended extends the last entry instead of making a new one. Without this, undo would
// go letter by letter, and five thousand characters would make five thousand steps.
void Editor::Kept::note(std::size_t at, Quilt::Scrap gone, Quilt::Scrap came, std::size_t was) {
    if (washed > done) washed = SIZE_MAX;

    log.resize(done);

    if (joining && !log.empty()) {
        Change& last = log.back();

        const bool typing = gone.much == 0 && came.much > 0 && last.gone.much == 0 &&
                            last.at + last.came.much == at && !sealed(quilt->last(last.came)) &&
                            quilt->lines(came) == 0;

        if (typing) {
            Quilt::joined(last.came, came, false);
            last.after = block;

            return;
        }

        const bool erasing = came.much == 0 && gone.much > 0 && last.came.much == 0 && last.gone.much > 0;

        // A held Backspace gathers the same way, only from the back: every next character vanishes
        // before the previous one, so the entry's start moves back and the scrap grows at the
        // front; in the source the character lies just before the previous one, so the span gets
        // longer.
        if (erasing && at + gone.much == last.at && quilt->lines(gone) == 0) {
            Quilt::joined(last.gone, gone, true);
            last.at = at;
            last.after = block;

            return;
        }

        // A held Delete gathers from the front: the deleted byte is still the same one.
        if (erasing && at == last.at && quilt->lines(gone) == 0) {
            Quilt::joined(last.gone, gone, false);
            last.after = block;

            return;
        }
    }

    Change made;
    made.at = at;
    made.gone = std::move(gone);
    made.came = std::move(came);
    made.was = was;
    made.tied = tying;
    made.before = held;
    made.after = block;

    log.push_back(std::move(made));

    done = log.size();
    joining = true;
}

// An editor with the system clipboard.
Editor::Editor() : Editor(clipboard()) {}

// An editor with the given clipboard.
Editor::Editor(Seam<Clipboard> with) : kept_(std::make_unique<Kept>()), selection_(*this), block_(*this) {
    kept_->clipboard = std::move(with);
    kept_->owner = this;

    role("talcite.editor");
}

// An editor with the given scroll shape and clipboard.
Editor::Editor(ScrollShape shape, Seam<Clipboard> with)
    : Page(shape), kept_(std::make_unique<Kept>()), selection_(*this), block_(*this) {
    kept_->clipboard = std::move(with);
    kept_->owner = this;

    role("talcite.editor");
}

Editor::~Editor() = default;

namespace {

// The quilt is in memory and answers at once, so the editor's spool gets its internal seam
// `Spool::Peek`, not `Feed`: the editor reads bytes at every cursor move and cannot wait for a
// window. That is also why the editor takes a document only as bytes.
Spool::Peek fed(const std::shared_ptr<Quilt>& quilt) {
    return [quilt](std::size_t at, std::size_t most) { return quilt->bytes(at, most); };
}

}

// The quilt under the spool. The spool reads bytes through the piece table, so it sees the
// document together with the changes, and nobody reads the original in again.
void Editor::hold(Bytes given) {
    kept_->quilt = std::make_shared<Quilt>(std::move(given));

    kept_->cursor = 0;
    kept_->anchor = 0;
    kept_->column = 0;
    kept_->block.reset();
    kept_->marking = false;

    // A new document is a new log: undoing to the state of the previous file would make no sense,
    // and `dirty()` has to start from clean.
    kept_->log.clear();
    kept_->done = 0;
    kept_->washed = 0;
    kept_->joining = false;
    kept_->tying = false;
    kept_->saving = false;

    Page::hold(std::make_shared<Spool>(fed(kept_->quilt), kept_->quilt->whole()));

    // A document in memory is counted at once; it is a pass over what is held anyway.
    spool()->counting();
}

// Whether the caller made the editor read-only.
bool Editor::readonly() const { return kept_->readonly; }

// Makes the editor read-only or editable again; a save in progress keeps it read-only anyway.
void Editor::readonly(bool given) { kept_->readonly = given; }

// The cursor as a byte of the document.
std::size_t Editor::cursor() const { return kept_->cursor; }

// The cursor is a byte of the document, and the spool works out the line and the offset in it.
// The other way would be worse: changes are addressed by byte, and lines change their numbering
// with every enter.
//
// Placing the cursor takes the anchor with it, which turns the selection off. A move with Shift
// does not touch the anchor and therefore goes through `Selection::extend`; apart from that one
// thing both ways are the same work, so one calls the other.
void Editor::cursor(std::size_t given, Moved how) {
    Kept& kept = *kept_;

    if (!spool()) return;

    kept.anchor = std::min(given, spool()->whole());

    selection_.extend(given, how);
}

// --- Selection -----------------------------------------------------------------------------------
//
// The selection is the span from the anchor to the cursor, and nothing more: the cursor is its
// end, so moving the cursor moves the selection, and placing the cursor (`Editor::cursor`) turns it
// off. The subobject holds nothing; it looks into the editor's `Kept` through its owner, just as
// the block does.
Editor::Selection::Selection(Editor& owner) : owner_(owner) {}

// Where the selection started; equal to the cursor when nothing is selected.
std::size_t Editor::Selection::anchor() const { return owner_.kept_->anchor; }

// The start of the selection, whichever way it was made.
std::size_t Editor::Selection::from() const { return std::min(owner_.kept_->anchor, owner_.kept_->cursor); }

// The end of the selection, whichever way it was made.
std::size_t Editor::Selection::upto() const { return std::max(owner_.kept_->anchor, owner_.kept_->cursor); }

// True when nothing is selected.
bool Editor::Selection::empty() const { return owner_.kept_->cursor == owner_.kept_->anchor; }

// The selected text; empty when nothing is selected.
std::string Editor::Selection::text() const {
    const Kept& kept = *owner_.kept_;

    if (!kept.quilt || empty()) return {};

    return talcite::text(kept.quilt->bytes(from(), upto() - from()));
}

// The parameters are named for what they are: `set(5, 2)` selects backward, the cursor lands on 2,
// and `from()` and `upto()` still return 2 and 5.
void Editor::Selection::set(std::size_t anchor, std::size_t cursor, Moved how) {
    Kept& kept = *owner_.kept_;

    if (!owner_.spool()) return;

    const std::size_t much = owner_.spool()->whole();

    kept.anchor = std::min(anchor, much);
    kept.cursor = std::min(cursor, much);
    kept.joining = false;

    owner_.invalidate();
    (void)owner_.trigger(ScrollEvent{how});
}

// Turns the selection off without moving the cursor.
void Editor::Selection::clear() {
    owner_.kept_->anchor = owner_.kept_->cursor;

    owner_.invalidate();
}

// Deleting the selection as one change in the log, the same for Backspace, Delete, cutting and
// replacing with nothing. False when there is nothing to delete or it is not allowed.
bool Editor::Selection::erase(Moved how) {
    Kept& kept = *owner_.kept_;

    if (kept.readonly || kept.saving || !kept.quilt || empty()) return false;

    const std::size_t at = from();
    const Quilt::Scrap gone = kept.quilt->taken(at, upto() - at);

    kept.apply(*owner_.spool(), at, gone, {});
    kept.note(at, gone, {}, kept.cursor);

    owner_.settled(at, how);

    return true;
}

// The cursor moves and the anchor stays: a move with Shift and a mouse drag.
void Editor::Selection::extend(std::size_t to, Moved how) {
    Editor& owner = owner_;
    Kept& kept = *owner.kept_;

    if (!owner.spool()) return;

    const std::size_t want = std::min(to, owner.spool()->whole());

    kept.cursor = want;

    // A cursor move closes the undo group: a word typed after a jump to another place has nothing
    // in common with the word from before the jump.
    kept.joining = false;

    const std::size_t which = owner.spool()->found(want);
    const std::size_t offset = want - owner.spool()->at(which).value_or(want);

    kept.column = owner.column_in_row(Place{which, offset}).value_or(0);

    (void)owner.reveal(Place{which, offset});

    // The selection is drawn with tags assembled at every paint, but the picture itself has to be
    // assembled anew; otherwise a moved cursor would change nothing on the screen.
    owner.invalidate();
    (void)owner.trigger(ScrollEvent{how});
}

// The line of the cursor, from zero.
std::size_t Editor::line() const {
    if (!spool()) return 0;

    return spool()->found(kept_->cursor);
}

// A jump to a line, the pair of `line()`: the cursor goes to the start of line `which` (from
// zero), the selection goes off and the view shows it. Past the last line it lands on the last
// one, because "go to 9999" in a file of a hundred lines has to reach the end, not refuse. The
// cursor counts in bytes, and only the spool knows the byte where a line starts, hence the method
// here and not in the caller.
void Editor::line(std::size_t which) {
    if (!spool()) return;

    const std::size_t many = spool()->counted().value_or(0);

    which = many == 0 ? 0 : std::min(which, many - 1);

    cursor(spool()->at(which).value_or(spool()->whole()));
}

// The column counted in cells, not in bytes: the status bar shows what is visible, and a tab is
// visible as eight places. `cursor()` returns the byte, so both measures are available, just as
// with the scroll bar, which goes by lines at one time and by bytes at another.
std::size_t Editor::column() {
    Kept& kept = *kept_;

    if (!spool()) return 0;

    const std::size_t which = spool()->found(kept.cursor);
    const std::optional<std::size_t> began = spool()->at(which);

    if (!began.has_value() || kept.cursor < *began) return 0;

    return column_in_line(Place{which, kept.cursor - *began}).value_or(0);
}

// The whole document directly.
std::string Editor::text() const {
    const Kept& kept = *kept_;

    if (!kept.quilt) return {};

    return kept.text(0, kept.quilt->whole());
}

// --- Clipboard -----------------------------------------------------------------------------------

// The clipboard does not belong to the editor. The editor holds no content here: it gets a
// `Clipboard` when built, and the default `clipboard()` is the door to the system clipboard, on
// POSIX to the clipboard of the machine the user sits at, through the terminal.
//
// Copying is allowed in a read-only document too: it changes nothing.
bool Editor::copy() {
    if (selection_.empty()) return false;

    return kept_->clipboard.provider().copy(selection_.text());
}

// Cutting goes through the same deletion of the selection as Backspace, so it is one change in the
// log and comes back with one undo. When copying fails, nothing is deleted: the document would be
// left without text that has nowhere to be pasted from.
bool Editor::cut() {
    if (kept_->readonly || kept_->saving || selection_.empty() || !copy()) return false;

    return selection_.erase();
}

// Inserts the clipboard's text at the cursor, over the selection if there is one. False when the
// editor is read-only or the clipboard is empty.
bool Editor::paste() {
    if (kept_->readonly || kept_->saving) return false;

    const std::optional<std::string> what = kept_->clipboard.provider().paste();

    if (!what.has_value() || what->empty()) return false;

    insert(*what);

    return true;
}

namespace {

// The selection broken into tags, one per line, because a tag lives in one line. They are computed
// only for what is visible: a selection of a whole two-gigabyte file has as many lines as the
// file, and the screen shows a few dozen anyway.
std::vector<Mark> spanned(Spool& spool, std::size_t from, std::size_t upto, std::size_t top,
                           std::size_t many) {
    std::vector<Mark> made;

    if (from >= upto) return made;

    const std::size_t first = spool.found(from);
    const std::size_t last = spool.found(upto > 0 ? upto - 1 : 0);

    for (std::size_t which = std::max(first, top); which <= last && which < top + many; ++which) {
        const std::optional<std::size_t> began = spool.at(which);

        if (!began.has_value()) break;

        const std::optional<std::size_t> next = spool.at(which + 1);
        const std::size_t ends = next.value_or(spool.whole());

        const std::size_t left = std::max(from, *began) - *began;
        const std::size_t right = std::min(upto, ends) - *began;

        if (right > left) made.push_back(Mark{which, left, right, selecting});
    }

    return made;
}

}

// The selection tags are assembled before every assembly of the picture, not when the selection
// changes: the range of visible lines also changes with scrolling and with resizing the window,
// and those do not go through `select`.
void Editor::layout(Size given) {
    Kept& kept = *kept_;

    std::vector<Mark> made;

    if (spool()) {
        const std::size_t many = static_cast<std::size_t>(std::max(given.high, 1));

        // The block with the same role as the selection; while it is being stretched it is seen
        // growing with the cursor.
        if (!block_.empty()) {
            const std::size_t from = std::min(block_.from(), block_.upto());
            const std::size_t upto = std::max(block_.from(), block_.upto());

            made = spanned(*spool(), from, upto, topmost(), many);
        }

        if (kept.cursor != kept.anchor) {
            const std::size_t from = std::min(kept.cursor, kept.anchor);
            const std::size_t upto = std::max(kept.cursor, kept.anchor);

            for (Mark& one : spanned(*spool(), from, upto, topmost(), many)) made.push_back(std::move(one));
        }
    }

    // Only marks that differ from those shown are a change; the same ones laid out again are not,
    // in whatever order they come (the marks keep themselves sorted).
    const std::vector<Mark>& shown = std::as_const(*this).marks().all();

    if (made.size() != shown.size() || !std::is_permutation(made.begin(), made.end(), shown.begin()))
        marks().all(std::move(made));

    Page::layout(given);
}

// The cursor's place on the screen, or nothing without a document.
std::optional<Spot> Editor::caret() const {
    Kept& kept = *kept_;

    if (!spool()) return {};

    const std::size_t which = spool()->found(kept.cursor);
    const std::optional<std::size_t> began = spool()->at(which);

    if (!began.has_value()) return {};

    return spot_of(Place{which, kept.cursor - *began});
}

// The cursor after a change: the spool recomputes the line, and the picture has to be assembled
// anew anyway. Separate, because all four ways of changing do it: typing, both deletions, undo
// and redo. Like every move of the cursor it says how it came (`ScrollEvent::by_hand`): the
// editor's own keys, mouse and paste pass `Moved::by_hand`, a call from the program leaves the
// default.
void Editor::settled(std::size_t where, Moved how) {
    Kept& kept = *kept_;

    kept.cursor = std::min(where, spool()->whole());
    kept.anchor = kept.cursor;

    const std::size_t which = spool()->found(kept.cursor);
    const std::size_t offset = kept.cursor - spool()->at(which).value_or(kept.cursor);

    kept.column = column_in_row(Place{which, offset}).value_or(0);

    (void)reveal(Place{which, offset});

    invalidate();
    (void)trigger(ScrollEvent{how});
}

// Typing over a selection is one change, not a deletion followed by typing; otherwise one undo
// would give back an empty document instead of what was selected.
void Editor::insert(std::string what, Moved how) {
    Kept& kept = *kept_;

    if (kept.readonly || kept.saving || !kept.quilt || what.empty()) return;

    const std::size_t from = std::min(kept.cursor, kept.anchor);
    const std::size_t upto = std::max(kept.cursor, kept.anchor);

    const Quilt::Scrap gone = kept.quilt->taken(from, upto - from);
    const Quilt::Scrap came = kept.quilt->stored(what);
    const std::size_t was = kept.cursor;

    kept.apply(*spool(), from, gone, came);
    kept.note(from, gone, came, was);

    settled(from + what.size(), how);
}

// Deletes `much` bytes before the cursor, or the selection when there is one.
void Editor::back_erase(std::size_t much, Moved how) {
    Kept& kept = *kept_;

    if (kept.readonly || kept.saving || !kept.quilt) return;

    // With a selection Backspace deletes the selection, however much it was told.
    if (selection_.erase(how)) return;

    const std::size_t upto = kept.cursor;
    const std::size_t from = much >= kept.cursor ? 0 : kept.cursor - much;

    if (upto <= from) return;

    const Quilt::Scrap gone = kept.quilt->taken(from, upto - from);

    kept.apply(*spool(), from, gone, {});
    kept.note(from, gone, {}, kept.cursor);

    settled(from, how);
}

// Deletes `much` bytes after the cursor, or the selection when there is one.
void Editor::front_erase(std::size_t much, Moved how) {
    Kept& kept = *kept_;

    if (kept.readonly || kept.saving || !kept.quilt) return;

    if (selection_.erase(how)) return;

    const std::size_t whole = kept.quilt->whole();

    if (kept.cursor >= whole) return;

    const std::size_t took = std::min(much, whole - kept.cursor);

    if (took == 0) return;

    const std::size_t at = kept.cursor;
    const Quilt::Scrap gone = kept.quilt->taken(at, took);

    kept.apply(*spool(), at, gone, {});
    kept.note(at, gone, {}, at);

    settled(at, how);
}

// --- Search --------------------------------------------------------------------------------------

namespace {

// How far one window of a search reaches. A line is searched in windows of twice this many bytes,
// one window every this many bytes, and a window takes only the hits that start in its first half:
// every hit of up to this many bytes is found exactly and in order, whatever the length of the
// line, and a longer one is not found. `std::regex` cannot read a stream, and a line may be a
// gigabyte long.
constexpr std::size_t span = 64 * 1024;

// The bytes [from, upto) of the document, read through the spool in pieces.
std::string fetched(Spool& spool, std::size_t from, std::size_t upto) {
    std::string made;

    while (from < upto) {
        const std::optional<std::string_view> got = spool.read(from, upto - from);

        if (!got.has_value() || got->empty()) break;

        made += *got;
        from += got->size();
    }

    return made;
}

// Every hit of `how` in the line content [at, upto) that starts at or after byte `from`, in order,
// as document bytes, given to `take`, which returns false to stop. A hit never crosses a line end,
// as before. A window that does not reach the end of the line is cut (`Comb::forth` is told so:
// "$" does not match at its end) and holds one byte more at each side, so that a whole word sees
// what stands next to it.
template <typename Take>
void combed(Spool& spool, const Comb& how, std::size_t at, std::size_t upto, std::size_t from, Take take) {
    std::size_t go = std::max(from, at);

    while (go <= upto) {
        const std::size_t view = go > at ? go - 1 : at;
        const std::size_t reach = std::min(upto, go + 2 * span);
        const bool cut = reach < upto;

        const std::string text = fetched(spool, view, cut ? reach + 1 : upto);

        const std::size_t limit = go + span;

        std::size_t off = go - view;
        std::size_t next = limit;

        while (true) {
            const std::optional<Hit> one = how.forth(text, off, cut);

            if (!one.has_value()) break;

            if (cut && (view + one->from >= limit || view + one->upto > reach)) break;

            if (!take(Hit{view + one->from, view + one->upto})) return;

            off = one->upto > one->from ? one->upto : one->from + 1;
            next = std::max(next, view + off);
        }

        if (!cut) return;

        go = next;
    }
}

}

// The comb searches, but the editor walks the document, because only the editor knows where a
// line starts. The place found is selected and shown at once; whoever wants the question alone,
// without moving the view, can ask the comb about a line taken on their own.
//
// The search does not wrap at the end of the document: calling again from zero is a decision of
// the application, not of the widget.
std::optional<Hit> Editor::find(const Comb& how, std::size_t from, bool back) {
    if (!spool() || !how.ready()) return {};

    const std::size_t at = std::min(from, spool()->whole());
    const std::size_t which = spool()->found(at);

    const auto picked = [this](Hit one) {
        selection_.set(one.from, one.upto);

        return std::optional<Hit>{one};
    };

    if (!back) {
        for (std::size_t line = which;; ++line) {
            const std::optional<Spool::Line> got = spool()->line(line);

            if (!got.has_value()) break;

            std::optional<Hit> hit;

            combed(*spool(), how, got->at, got->upto, line == which ? at : got->at, [&hit](Hit one) {
                hit = one;

                return false;
            });

            if (hit.has_value()) return picked(*hit);
        }

        return {};
    }

    // Backward: the last hit that starts before the place, in the line of the place, then in the
    // lines above. A regular expression reads in one direction only, so each line is walked from
    // its start and the last hit kept.
    for (std::size_t line = which;; --line) {
        const std::optional<Spool::Line> got = spool()->line(line);

        if (got.has_value()) {
            const std::size_t limit = line == which ? at : SIZE_MAX;

            std::optional<Hit> hit;

            combed(*spool(), how, got->at, got->upto, got->at, [&hit, limit](Hit one) {
                if (one.from >= limit) return false;

                hit = one;

                return true;
            });

            if (hit.has_value()) return picked(*hit);
        }

        if (line == 0) break;
    }

    return {};
}

namespace {

// Typing nothing is not a change, so replacing with nothing goes through deleting the selection.
void put(Editor& one, const std::string& with) {
    if (with.empty())
        (void)one.selection().erase();
    else
        one.insert(with);
}

}

// Replacing one hit: the current selection, if it is a match itself, and showing the next one, as
// every editor does: one press of the key replaces what is lit and moves on. When the selection is
// not a hit, it only searches for the next one. True when something was replaced.
bool Editor::replace(const Comb& how, const std::string& with) {
    Kept& kept = *kept_;

    if (kept.readonly || kept.saving || !kept.quilt || !how.ready()) return false;

    if (!selection_.empty()) {
        const std::string got = selection_.text();
        const std::optional<Hit> one = how.forth(got, 0);

        if (one.has_value() && one->from == 0 && one->upto == got.size()) {
            const std::size_t began = selection_.from();

            put(*this, with);

            (void)find(how, began + with.size(), false);

            return true;
        }
    }

    (void)find(how, kept.cursor, false);

    return false;
}

// Replacing all: one pass over the document from before the replacement collects the hits
// (without selecting and without events), and then one change puts `with` in the place of each of
// them, as one entry that is undone in one move. Handling every hit separately through searching,
// selecting, inserting and notifying the spool would rebuild the quilt each time, which is
// quadratic for a hundred thousand hits, and so would its undo. The search runs in the text from
// before the replacement, so the inserted text is never searched: a pattern hidden in it does not
// loop the replacement. Returns how many were replaced.
std::size_t Editor::replace_all(const Comb& how, const std::string& with) {
    Kept& kept = *kept_;

    if (kept.readonly || kept.saving || !kept.quilt || !how.ready() || !spool()) return 0;

    std::vector<Kept::Splice> many;

    // The comb never returns empty hits (it skips them itself), so every hit moves the search
    // forward.
    for (std::size_t line = 0;; ++line) {
        const std::optional<Spool::Line> got = spool()->line(line);

        if (!got.has_value()) break;

        combed(*spool(), how, got->at, got->upto, got->at, [&many, &kept](Hit one) {
            many.push_back(Kept::Splice{one.from, kept.text(one.from, one.upto)});

            return true;
        });
    }

    if (many.empty()) return 0;

    const std::size_t was = kept.cursor;

    kept.marking = false;
    kept.splice(*spool(), many, with, false);

    if (kept.washed > kept.done) kept.washed = SIZE_MAX;

    kept.log.resize(kept.done);

    Kept::Change made;
    made.at = many.front().at;
    made.with = with;
    made.was = was;
    made.many = std::move(many);
    made.before = kept.held;
    made.after = kept.block;

    const std::size_t count = made.many.size();
    const std::size_t last = Kept::ended(made.many, with);

    kept.log.push_back(std::move(made));
    kept.done = kept.log.size();
    kept.joining = false;

    settled(last);

    return count;
}

// --- Undo ----------------------------------------------------------------------------------------

// Whether the document differs from the last save, judged by the place in the log of changes.
bool Editor::dirty() const { return kept_->done != kept_->washed; }

// Clean from now on, and sealed: a character typed after the save starts a new entry; otherwise it
// would be added to the last one without changing the number, and `dirty()` would say "clean"
// about text that differs from what is on the disk.
void Editor::clean() {
    kept_->washed = kept_->done;
    kept_->joining = false;
}

namespace {

// How many bytes go to the sink at once. As many as the spool's window: a document can be larger
// than memory, so it has to flow through saving in pieces too.
constexpr std::size_t pouring = 64 * 1024;

}

// The library does not touch files. The application gives the sink and the editor gives the
// content, in pieces through the pool. Every piece is read from the quilt on the main thread (it is
// memory, microseconds) and goes to the sink as a promise; the next one is read only after that
// promise is fulfilled, so during the whole save one write runs at a time, in order. The sink sees
// the end of the document as an empty piece: the last call, after which nothing more comes; an
// empty document is that one call alone. The sink then closes what it has to close (the file, the
// rename), and the log is washed only after that promise is fulfilled: "clean" then means "really
// on the disk", not "handed to the sink". A sink that promises instead of writing would otherwise
// let the editor say "clean" about a file that does not exist yet. A refusal by the sink at any
// point leaves the log dirty, `busy()` goes off, and the caller gets a rejection with the sink's
// sentence: an unfinished save is not a save.
//
// Meanwhile the editor is busy: keys that change the content bounce off, so that the pieces that
// have already flowed and those still to go are one document. A second save during the first one
// and a save without a sink are rejected at once; that is the caller's mistake, not a disk problem.
// The application writes beside the file and renames, as always.
//
// The chain holds the editor weakly: when the editor dies in the middle, the piece that has just
// come back has nowhere to read the next one from, and the save ends with a rejection, not with a
// touch of freed memory.
Promise<void> Editor::save(Drain into) {
    Kept& kept = *kept_;

    if (!kept.quilt || !into || kept.saving)
        return failed<void>(std::make_exception_ptr(std::logic_error{"The editor cannot save now."}));

    kept.saving = true;

    const std::size_t whole = kept.quilt->whole();
    const std::weak_ptr<Node> weak = weak_from_this();

    // A link that calls itself: `then` flattens a promise returned from the continuation, so every
    // link returns the promise of the rest of the save, and the first one the promise of the
    // whole. The link holds itself weakly; only the continuation of the waiting piece holds it
    // strongly. A strong pointer to itself would be a cycle that never breaks, and every save would
    // leave the sink in memory, together with everything it holds.
    const auto step = std::make_shared<std::function<Promise<void>(std::size_t)>>();
    const std::weak_ptr<std::function<Promise<void>(std::size_t)>> self = step;

    *step = [weak, into, whole, self](std::size_t at) -> Promise<void> {
        const std::shared_ptr<Node> held = weak.lock();
        auto* mine = dynamic_cast<Editor*>(held.get());

        if (mine == nullptr || !mine->kept_->quilt)
            return failed<void>(std::make_exception_ptr(std::runtime_error{"The editor is gone."}));

        if (at >= whole) return into(Bytes{});

        Bytes got = mine->kept_->quilt->bytes(at, pouring);

        // The quilt promised more than it gives. Better to say "not saved" than to save a stub.
        if (got.empty()) return failed<void>(std::make_exception_ptr(std::runtime_error{"The document ended early."}));

        const std::size_t next = at + got.size();

        return into(std::move(got)).here().then([step = self.lock(), next] { return (*step)(next); });
    };

    // The first piece goes on the caller's thread: a quilt or a sink that throws before it returns
    // a promise must not leave the editor busy forever, so the exception becomes a rejection.
    Promise<void> first;

    try {
        first = (*step)(0);
    } catch (...) {
        kept.saving = false;

        return failed<void>(std::current_exception());
    }

    // The editor stops being busy however the save ends: written, refused, or abandoned by the
    // caller. `finally` after `here` does it on the loop's thread in every one of these cases.
    return std::move(first)
        .here()
        .then([weak] {
            if (auto* mine = dynamic_cast<Editor*>(weak.lock().get()); mine != nullptr) {
                mine->kept_->saving = false;
                mine->clean();
            }
        })
        .finally([weak] {
            if (auto* mine = dynamic_cast<Editor*>(weak.lock().get()); mine != nullptr) mine->kept_->saving = false;
        });
}

// Whether a save is in progress.
bool Editor::busy() const { return kept_->saving; }

// Whether there is something to undo and the editor allows changes now.
bool Editor::undoable() const { return !kept_->readonly && !kept_->saving && kept_->done > 0; }

// Whether there is something to redo and the editor allows changes now.
bool Editor::redoable() const { return !kept_->readonly && !kept_->saving && kept_->done < kept_->log.size(); }

// Undo is the same change read backward: what disappeared comes back, what came disappears. The
// cursor lands where it stood before the change, because that is the place the document returns
// to.
void Editor::undo() {
    Kept& kept = *kept_;

    if (!undoable()) return;

    std::size_t where = kept.cursor;

    // Tied entries are undone together: "replace all" has to come back in one move.
    kept.marking = false;

    while (kept.done > 0) {
        Kept::Change& one = kept.log[kept.done - 1];

        if (one.many.empty()) kept.apply(*spool(), one.at, one.came, one.gone);
        else kept.splice(*spool(), one.many, one.with, true);
        kept.block = one.before;

        --kept.done;
        where = one.was;

        if (!one.tied) break;
    }

    kept.joining = false;

    settled(where);
}

// Redo applies the next change again, with the tied entries after it; the cursor lands at the end
// of what came.
void Editor::redo() {
    Kept& kept = *kept_;

    if (!redoable()) return;

    std::size_t where = kept.cursor;

    kept.marking = false;

    while (kept.done < kept.log.size()) {
        Kept::Change& one = kept.log[kept.done];

        if (one.many.empty()) kept.apply(*spool(), one.at, one.gone, one.came);
        else kept.splice(*spool(), one.many, one.with, false);

        kept.block = one.after;

        ++kept.done;
        where = one.many.empty() ? one.at + one.came.much : Kept::ended(one.many, one.with);

        if (kept.done >= kept.log.size() || !kept.log[kept.done].tied) break;
    }

    kept.joining = false;

    settled(where);
}

namespace {

// How far the editor looks around the cursor for the character it stands at. A cluster longer than
// this (hundreds of stacked marks) is taken in pieces of this size; nothing real comes close.
constexpr std::size_t glance = 256;

// How many bytes the character before this place has: the whole cluster, as it stands on the
// screen. Deleting counts in bytes, and a character can be several code points; without this
// Backspace would take the accent off "e" with U+0301 and leave the letter, or cut a family emoji
// into its members, where the screen shows one character.
std::size_t behind(Quilt& quilt, std::size_t at) {
    if (at == 0) return 0;

    const std::size_t look = std::min(at, glance);
    const std::string what = talcite::text(quilt.bytes(at - look, look));

    return what.size() - cluster_before(what, what.size());
}

// How many bytes the character at this place has, so that Delete and the arrow take the whole
// cluster.
std::size_t ahead(Quilt& quilt, std::size_t at) {
    const std::string what = talcite::text(quilt.bytes(at, glance));

    return cluster_after(what, 0);
}

// The byte at `at`, read through the spool; a zero byte past the end of the document.
char byte_at(Spool& spool, std::size_t at) {
    const std::optional<std::string_view> got = spool.read(at, 1);

    return got.has_value() && !got->empty() ? got->front() : '\0';
}

// Whether the byte at `at` belongs to a word, by the "word byte" rule of `encoding.hpp`.
bool lettered(Spool& spool, std::size_t at) {
    return in_word(static_cast<char32_t>(static_cast<unsigned char>(byte_at(spool, at))));
}

// A jump by words. The "word byte" rule comes from `encoding.hpp`, the same one by which the comb
// searches for whole words and a double click takes a word.
//
// Forward: skip the run of the same kind you stand on, then what is not a word, that is, land at
// the start of the next word. Backward, symmetrically: skip what is not a word, then the word
// itself, that is, land at the start of this word or the previous one. At the edge of a line the
// jump goes to the neighboring line. The bytes are read one by one around the cursor, so a jump in
// a long line costs the word, not the line.
std::size_t worded(Spool& spool, std::size_t at, bool forth) {
    const std::size_t which = spool.found(at);
    const std::optional<Spool::Line> line = spool.line(which);

    if (!line.has_value() || at < line->at) return at;

    std::size_t go = std::min(at, line->upto);

    if (forth) {
        if (go >= line->upto) return line->next < spool.whole() ? line->next : at;

        const bool kind = lettered(spool, go);

        while (go < line->upto && lettered(spool, go) == kind) ++go;

        while (go < line->upto && !lettered(spool, go)) ++go;

        return go;
    }

    if (go == line->at) {
        if (which == 0) return at;

        const std::optional<Spool::Line> before = spool.line(which - 1);

        return before.has_value() ? before->upto : at;
    }

    while (go > line->at && !lettered(spool, go - 1)) --go;

    while (go > line->at && lettered(spool, go - 1)) --go;

    return go;
}

}

// The editor's keys: typing, Enter, Backspace and Delete, and movement by characters, words, lines
// and pages, Home and End. Other keys go to `Page`, and a move past the first or the last line goes
// up to an ancestor.
//
// The editor binds no key to an action of its own: undo, redo, the block and the clipboard are
// methods (`undo`, `block().mark()`, `copy`, `cut`, `paste`...), and which keys call them is the
// application's choice, made in one table of its own. A key bound here could not be taken away:
// the application would have to work around it, and a second set beside it (Midnight Commander's
// Ctrl+Ins, Shift+Ins and Shift+Del, or Ctrl+C, Ctrl+V and Ctrl+X from elsewhere) would be
// written in two places. In raw mode Ctrl+C is not an interrupt but an ordinary key, so taking it
// here would also hide it from an application that wants it for quitting.
bool Editor::handle(KeyEvent& what) {
    Kept& kept = *kept_;

    if (!kept.quilt) return false;

    const Mods mods = what.mods();

    if (what.key().letter() != 0 && !mods.ctrl && !mods.alt) {
        insert(utf8::encode(what.key().letter()), Moved::by_hand);

        return true;
    }

    // A move with Shift stretches the selection, without Shift it turns it off. The branch stands
    // in one place, because every move computes its target byte itself, and this is the only
    // difference between them.
    const auto went = [this, &mods](std::size_t where) {
        if (mods.shift)
            selection_.extend(where, Moved::by_hand);
        else
            cursor(where, Moved::by_hand);
    };

    switch (what.key().code()) {
        case Code::enter: insert("\n", Moved::by_hand); return true;

        case Code::backspace: back_erase(behind(*kept.quilt, kept.cursor), Moved::by_hand); return true;

        // Delete with Shift is not a deletion: it is the cut key of Midnight Commander, and goes on
        // to the application like Insert with a modifier.
        case Code::remove:
            if (mods.shift) break;

            front_erase(ahead(*kept.quilt, kept.cursor), Moved::by_hand);
            return true;

        // With Ctrl the arrow goes by words, without it by characters. A character, not a byte:
        // otherwise the arrow would stop in the middle of a diacritic.
        case Code::left:
            went(mods.ctrl ? worded(*spool(), kept.cursor, false)
                           : kept.cursor - behind(*kept.quilt, kept.cursor));
            return true;

        case Code::right:
            went(mods.ctrl ? worded(*spool(), kept.cursor, true)
                           : kept.cursor + ahead(*kept.quilt, kept.cursor));
            return true;

        // The vertical arrows and the page keys go by screen rows, as they are drawn: with wrapping
        // a long line is walked row by row, not jumped over, and without wrapping a row is a line.
        // The column stays the one remembered, not the one of the row passed through.
        case Code::up:
        case Code::down:
        case Code::pageup:
        case Code::pagedown: {
            const Code code = what.key().code();
            const bool upwards = code == Code::up || code == Code::pageup;

            const int much = code == Code::up || code == Code::down ? 1 : std::max(area().size.high, 1);

            const std::size_t which = spool()->found(kept.cursor);
            const std::optional<std::size_t> began = spool()->at(which);

            if (!began.has_value()) return false;

            const std::size_t column = kept.column;

            // There is nowhere to go above the first row or below the last one. The key is then
            // handed to an ancestor instead of being eaten: a tab or a list above may use it. A page
            // key near an edge goes as far as there is.
            const std::optional<Place> to = rows_away(Place{which, kept.cursor - *began}, upwards ? -much : much, column);

            if (!to.has_value()) return false;

            const std::optional<std::size_t> start = spool()->at(to->line);

            if (!start.has_value()) return false;

            went(*start + to->byte);

            // `extend` stored the new column, and the old one has to be remembered; otherwise the
            // cursor going down past a short row would lose its column for good.
            kept.column = column;

            return true;
        }

        case Code::home: {
            const std::size_t which = spool()->found(kept.cursor);

            went(spool()->at(which).value_or(kept.cursor));

            return true;
        }

        case Code::end: {
            const std::optional<Spool::Line> line = spool()->line(spool()->found(kept.cursor));

            if (!line.has_value()) return false;

            went(line->upto);

            return true;
        }

        default: break;
    }

    return Page::handle(what);
}

// A click puts the cursor where the click was, rather than only taking the focus, as in the
// viewer. The page converts the row and column into a line and a byte: a screen row is not a line
// number, because with wrapping one line takes several rows, and a column is not a byte, because a
// tab takes eight cells. Computing it here by hand would put the cursor somewhere else entirely.
bool Editor::handle(ClickEvent& what) {
    if (root() != nullptr) focus();

    if (!spool()) return true;

    const std::optional<Place> where = place_of(local(what.where()));

    if (!where.has_value()) return true;

    const std::optional<std::size_t> began = spool()->at(where->line);

    if (!began.has_value()) return true;

    cursor(*began + where->byte, Moved::by_hand);

    return true;
}

// Selecting by dragging. The press places the cursor together with the anchor, and motion with
// the button held stretches the selection with the anchor in place; that is the whole difference
// between `cursor` and `Selection::extend`. The terminal sends motion with a held button by
// itself, because `?1002` is on; `?1003` would be needed only for motion without a button, that
// is, for highlighting under the pointer.
//
// `ClickEvent` stays as it is and does not conflict with this: the root raises it only when the
// release fell in the same cell as the press. With a real drag it does not arise at all, and with
// an ordinary click it places the same cursor that the press has already placed.
//
// Autoscrolling runs on events, not on a timer. Motion in the edge row scrolls by a line, so the
// speed of going down comes from how much the user really moves the mouse; when the mouse stops,
// scrolling stops with it. A timer would give a speed that depends on time and would keep
// scrolling with the mouse standing still.
//
// The drag goes beyond the editor. The press asks the root to capture the mouse, so motion and
// the release come back here from outside the editor too. Motion above or below it scrolls by a
// line as on the edge row, and the selection reaches the first or the last visible line; the row
// is clamped here, because `place_of` returns nothing outside the area, while sideways it clamps
// on its own.
bool Editor::handle(MouseEvent& what) {
    Kept& kept = *kept_;

    if (!spool()) return true;

    if (what.press() == Press::up) {
        kept.dragging = false;

        return true;
    }

    const Spot where = local(what.where());

    const auto reached = [this, &kept](Spot aim, bool anchored) {
        const std::optional<Place> place = place_of(aim);

        if (!place.has_value()) return;

        const std::optional<std::size_t> began = spool()->at(place->line);

        if (!began.has_value()) return;

        if (anchored)
            cursor(*began + place->byte, Moved::by_hand);
        else
            selection_.extend(*began + place->byte, Moved::by_hand);
    };

    if (what.press() == Press::down) {
        if (what.button() != Mouse::left) return true;

        if (Root* top = root(); top != nullptr) {
            focus();
            top->capture(*this);
        }

        reached(where, true);

        kept.dragging = true;

        return true;
    }

    if (!kept.dragging || what.button() == Mouse::none) return true;

    // The edge row scrolls by a line, and only then is what lies under the mouse read, because
    // after scrolling something else stands there.
    const int high = area().size.high;

    if (high > 1) {
        if (where.y >= high - 1)
            slide(1, Moved::by_hand);
        else if (where.y <= 0)
            slide(-1, Moved::by_hand);
    }

    reached({where.x, std::clamp(where.y, 0, std::max(high - 1, 0))}, false);

    return true;
}

// A double click takes a word. The "word byte" rule comes from `encoding.hpp`, the same one by
// which the comb searches for whole words; otherwise double click and search would start to
// differ on diacritics.
//
// A click on a place that is not a word takes a run of the same kind: spaces only or punctuation
// only. It always selects something, so a double click never looks ignored.
bool Editor::handle(DoubleClickEvent& what) {
    if (root() != nullptr) focus();

    if (!spool()) return true;

    const std::optional<Place> where = place_of(local(what.where()));

    if (!where.has_value()) return true;

    const std::optional<Spool::Line> line = spool()->line(where->line);

    if (!line.has_value() || line->upto == line->at) return true;

    Spool& one = *spool();

    const std::size_t at = std::min(line->at + where->byte, line->upto - 1);

    const bool kind = lettered(one, at);

    std::size_t from = at;
    std::size_t upto = at + 1;

    while (from > line->at && lettered(one, from - 1) == kind) --from;

    while (upto < line->upto && lettered(one, upto) == kind) ++upto;

    selection_.set(from, upto, Moved::by_hand);

    return true;
}

// A bracketed paste goes in as one insertion at the cursor.
bool Editor::handle(PasteEvent& what) {
    insert(what.text(), Moved::by_hand);

    return true;
}

// --- The mcedit block ----------------------------------------------------------------------------
//
// A lasting span of the document, independent of the selection (`anchor..cursor`) and of the
// clipboard, as in mcedit, where the block and the clipboard are separate too. F3 starts it at the
// cursor, movement stretches it, the second F3 ends it, and the block stays highlighted although
// the cursor goes on; only then F5 inserts it at the cursor, F6 moves it and F8 deletes it. The
// block is a third position, not the selection, because `insert` replaces the selection, and the
// cursor is its end: "copy the block to the cursor" would make no sense in that model. It holds
// nothing of its own: it looks into the editor's `Kept` through its owner.

Editor::Block::Block(Editor& owner) : owner_(owner) {}

// True when there is no block, finished or being stretched.
bool Editor::Block::empty() const { return !owner_.kept_->block.has_value(); }

// True while the block is being stretched, between the two presses of F3.
bool Editor::Block::pending() const { return owner_.kept_->marking; }

// The start of the block; zero without a block.
std::size_t Editor::Block::from() const {
    const Kept& kept = *owner_.kept_;

    return kept.block.has_value() ? kept.block->first : 0;
}

// While stretching, the end is the cursor; it can stand before the start when the block goes
// backward, and `text()` and painting put min and max in order.
std::size_t Editor::Block::upto() const {
    const Kept& kept = *owner_.kept_;

    if (!kept.block.has_value()) return 0;

    return kept.marking ? kept.cursor : kept.block->second;
}

// The text of the block; empty without a block.
std::string Editor::Block::text() const {
    if (empty()) return {};

    return owner_.kept_->text(std::min(from(), upto()), std::max(from(), upto()));
}


// F3, three states: without a block it starts one at the cursor; during stretching it ends it;
// with a finished block it drops the old one and starts a new one at the cursor.
void Editor::Block::mark() {
    Kept& kept = *owner_.kept_;

    if (!kept.quilt) return;

    if (kept.marking) {
        kept.blocked();
    } else {
        kept.block = std::pair{kept.cursor, kept.cursor};
        kept.marking = true;
    }

    owner_.invalidate();
}

// Removing the block without a trace in the document; it does not go to the log, because the
// content does not change.
void Editor::Block::clear() {
    Kept& kept = *owner_.kept_;

    kept.block.reset();
    kept.marking = false;

    owner_.invalidate();
}

// F5: the block's content goes in at the cursor, and the block stays where it was; when it lay
// after the cursor, it moves by its length by the ordinary "change before the block" rule. One
// change in the log, sealed: a character typed right after it must not stick to it.
bool Editor::Block::copy() {
    Kept& kept = *owner_.kept_;

    if (kept.readonly || kept.saving || !kept.quilt) return false;

    kept.blocked();

    if (!kept.block.has_value()) return false;

    const auto [from, upto] = *kept.block;
    const Quilt::Scrap what = kept.quilt->taken(from, upto - from);
    const std::size_t at = kept.cursor;

    kept.joining = false;
    kept.apply(*owner_.spool(), at, {}, what);
    kept.note(at, {}, what, at);
    kept.joining = false;

    owner_.settled(at + what.much);

    return true;
}

// F6: the block leaves its place and goes in where the cursor stood after the cut; when it lay
// before the cursor, the target moves back by its length. A cursor in the block or on its edge is
// "move to itself": nothing. Two log entries tied into one, so undo restores both the text and the
// block where it came from. After the move the block covers the inserted text, so that it can be
// moved further.
bool Editor::Block::move() {
    Kept& kept = *owner_.kept_;

    if (kept.readonly || kept.saving || !kept.quilt) return false;

    kept.blocked();

    if (!kept.block.has_value()) return false;

    const auto [from, upto] = *kept.block;
    const std::size_t at = kept.cursor;

    if (at >= from && at <= upto) return false;

    const Quilt::Scrap what = kept.quilt->taken(from, upto - from);
    const std::size_t target = at > upto ? at - what.much : at;

    kept.joining = false;
    kept.apply(*owner_.spool(), from, what, {});
    kept.note(from, what, {}, at);

    kept.tying = true;
    kept.apply(*owner_.spool(), target, {}, what);
    kept.block = std::pair{target, target + what.much};
    kept.note(target, {}, what, at);
    kept.tying = false;
    kept.joining = false;

    owner_.settled(target + what.much);

    return true;
}

// F8: the block disappears from the document together with its text. A cursor after the block
// moves back by its length, one inside lands at its start, one before it stays. One change; undo
// restores the text and the block.
bool Editor::Block::erase() {
    Kept& kept = *owner_.kept_;

    if (kept.readonly || kept.saving || !kept.quilt) return false;

    kept.blocked();

    if (!kept.block.has_value()) return false;

    const auto [from, upto] = *kept.block;
    const std::size_t at = kept.cursor;

    const Quilt::Scrap what = kept.quilt->taken(from, upto - from);

    kept.joining = false;
    kept.apply(*owner_.spool(), from, what, {});
    kept.block.reset();
    kept.note(from, what, {}, at);
    kept.joining = false;

    owner_.settled(at >= upto ? at - what.much : (at > from ? from : at));

    return true;
}

}
