#include "tal/controls.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "tal/editing.hpp"
#include "tal/key.hpp"
#include "tal/text.hpp"
#include "tal/views.hpp"
#include "tal/window.hpp"

#include "dropping.hpp"
#include "lettering.hpp"
#include "lining.hpp"

namespace tal {

namespace {

// `said` with its line ends turned into spaces, as one line of a field takes a pasted text.
std::string flattened(std::string_view said) {
    std::string made;

    for (std::size_t at = 0; at < said.size(); ++at) {
        if (said[at] == '\r' && at + 1 < said.size() && said[at + 1] == '\n') continue;

        made.push_back(said[at] == '\n' || said[at] == '\r' ? ' ' : said[at]);
    }

    return made;
}

}

// An edit of a field: one piece of its line, of the kind `kind`, with the cursor before it. Undoing
// puts back what was gone and the cursor where it stood; redoing puts what came and the cursor
// after it. The edit right after it joins it by the rules an editor keeps too (`lining::joined`):
// typing goes on where it ended until a space, erasing next to where it was in one direction. The
// field seals its history when the cursor moves or the text changes otherwise.
struct Field::Edit : Step {
    Edit(Field& owner, lining::Doing made, lining::Piece<std::size_t> one, std::size_t was) : field(owner), kind(made), piece(std::move(one)), before(was) {}

    // Puts back what was gone and the cursor where it stood.
    void undo() override {
        const std::string& said = *field.text;

        field.restored(said.substr(0, piece.from) + piece.gone + said.substr(lining::ended(piece.from, piece.came)), before);
    }

    // Puts what came again, the cursor after it.
    void redo() override {
        const std::string& said = *field.text;

        field.restored(said.substr(0, piece.from) + piece.came + said.substr(lining::ended(piece.from, piece.gone)), lining::ended(piece.from, piece.came));
    }

    // What the edit was, for a menu.
    [[nodiscard]] std::string said() const override {
        return kind == lining::Doing::typing ? "Typing" : kind == lining::Doing::other ? "Replace" : "Erasing";
    }

    // Takes in `later` while typing or erasing goes on; the field's history holds only its edits.
    bool merged(const Step& later) override {
        const auto& next = static_cast<const Edit&>(later);

        return lining::joined(kind, piece, next.kind, next.piece);
    }

    // Puts `one` into the line of `field` with the cursor after it, as a step of the field's
    // history of the kind `kind`; nothing when neither the text nor the cursor would change, as
    // Backspace at the start.
    static void made(Field& field, lining::Doing kind, lining::Piece<std::size_t> one) {
        const std::size_t was = field.at();

        if (one.gone == one.came && lining::ended(one.from, one.came) == was) return;

        auto step = std::make_unique<Edit>(field, kind, std::move(one), was);

        step->redo();
        field.undo_.push(std::move(step));
    }

    Field& field;
    lining::Doing kind;
    lining::Piece<std::size_t> piece;
    std::size_t before;
};

// A field holding `said`, its cursor at the end, in the class "input": one line of text to type.
// The arrows, Home, End, Backspace and Delete work on the text's characters, Ctrl with an arrow
// goes a word, letters and pastes type; Ctrl+Z and Ctrl+Y undo and redo. Before a letter goes in
// the field tells `Typing`, and taking it refuses the letter; after every change it tells
// `Guessing`, and what a handler puts in `ahead` stands after the cursor at the end in the part
// "guess": the right arrow there takes one letter of it, End all of it. Tab and Enter are left to
// the nodes above. A click puts the cursor where it was. Files dropped on it type the text the
// terminal wrote for them, before the nodes above see the drop.
Field::Field(std::string said) : text(*this, Effect::paint, std::move(said)) {
    classes({"input"});
    focusable(true);

    cursor = text->size();

    hooks_.add(on<Changed>([this](Changed& what) {
        if (editing_ || (!what.of(text) && !what.of(cursor) && !what.of(hidden))) return;

        if (what.of(text)) undo_.clear();
        else undo_.seal();

        followed();
        guessing();
    }));

    hooks_.add(otherwise<KeyEvent>([this](KeyEvent& what) {
        const Key key = what.key;
        const Mods mods = key.mods();
        const std::size_t here = at();
        const std::string& line = *text;

        if (mods.ctrl && !mods.alt) {
            if (lettering::same(key.letter(), U'z')) (void)undo_.undo();
            else if (lettering::same(key.letter(), U'y')) (void)undo_.redo();
            else if (key.code() == Code::left || key.code() == Code::right) moved(word(key.code() == Code::right));
            else return;

            what.take();

            return;
        }

        if (mods.ctrl || mods.alt) return;

        switch (key.code()) {
            case Code::left: moved(text::before(line, here, rendering())); break;
            case Code::right:
                if (here == line.size() && !ahead_.empty()) typed(ahead_.substr(0, text::after(ahead_, 0, rendering())));
                else moved(text::after(line, here, rendering()));
                break;
            case Code::home: moved(0); break;
            case Code::end:
                if (here == line.size() && !ahead_.empty()) typed(ahead_);
                else moved(line.size());
                break;
            case Code::backspace: erased(text::before(line, here, rendering()), here); break;
            case Code::remove: erased(here, text::after(line, here, rendering())); break;
            case Code::none: {
                if (key.letter() == 0) return;

                std::string one;

                text::encoded(one, key.letter());
                typed(one);

                break;
            }
            default: return;
        }

        what.take();
    }));

    hooks_.add(otherwise<PasteEvent>([this](PasteEvent& what) {
        what.take();
        typed(flattened(what.text));
    }));

    hooks_.add(on<DropEvent>([this](DropEvent& what) {
        if (&what.target() != this) return;

        what.take();
        typed(flattened(what.text));
    }));

    hooks_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        if (what.press != Press::click || what.button != MouseButton::left) return;

        const int line = style().border == Border::none ? 0 : 1;

        what.take();
        moved(under(what.at.x - line - style().padding.left));
    }));
}

// One row of one column, the least it may have: the layout, not the text, sizes a field.
Size Field::measure(Limits room) const {
    (void)room;

    return Size{1, 1};
}

// Takes the width and keeps the cursor in view.
void Field::arrange(Area inside) {
    wide_ = inside.size.wide;

    followed();
}

// The text from where it is shifted to, or stood as `justify` says while it fits; each character
// as the glyph `input.conceal` when hidden; the guess after it in the part "guess".
void Field::paint(Canvas& onto) const {
    const std::string& said = *text;
    const text::Rendering how = rendering();
    int x = origin();

    for (const std::string_view one : text::clusters(said, how)) x += onto.put({x, 0}, *hidden ? glyph("input.conceal") : one, look());

    if (!ahead_.empty()) (void)onto.put({x, 0}, ahead_, look("guess"));
}

// The cursor's cell.
std::optional<Caret> Field::caret() const { return Caret{Spot{origin() + column(at()), 0}}; }

// Sets the text to `said` with the cursor at its end, as an edit to undo on its own, since an edit
// of another kind joins nothing: a history walked, a guess taken in by the program.
void Field::put(std::string said) { Edit::made(*this, lining::Doing::other, lining::Piece<std::size_t>{0, *text, std::move(said)}); }

// The cursor as it stands: on the border of a cluster, not past the end.
std::size_t Field::at() const {
    const std::string& said = *text;
    const std::size_t made = std::min(*cursor, said.size());

    return made == said.size() ? made : text::before(said, text::after(said, made, rendering()), rendering());
}

// How many columns the text takes up to the byte `byte`, as drawn.
int Field::column(std::size_t byte) const {
    const std::string_view said = std::string_view{*text}.substr(0, byte);

    if (!*hidden) return text::width(said, rendering());

    return static_cast<int>(text::clusters(said, rendering()).size()) * text::width(glyph("input.conceal"), rendering());
}

// Where the text starts in the field: as `justify` says while the text and its guess fit with a
// cell left for the cursor after them, otherwise shifted left to keep the cursor in view.
int Field::origin() const {
    const int whole = column(text->size()) + text::width(ahead_, rendering());

    if (whole < wide_) {
        const int spare = wide_ - whole - 1;

        return *justify == Edge::start ? 0 : *justify == Edge::middle ? spare / 2 : spare;
    }

    return -shift_;
}

// The border before the cluster in the column `x` of the field, as a click finds it; past the text,
// its end.
std::size_t Field::under(int x) const {
    const std::string& said = *text;
    const int target = x - origin();
    std::size_t made = 0;

    while (made < said.size()) {
        const std::size_t next = text::after(said, made, rendering());

        if (column(next) > target) break;

        made = next;
    }

    return made;
}

// The start of the word before the cursor, or the end of the word after it (`forth`): past what is
// not a word, then past the word, as in an editor (`lining::worded`).
std::size_t Field::word(bool forth) const { return lining::worded(*text, at(), forth, rendering()); }

// Types `what` at the cursor: each character the field is told about (`Typing`), the refused ones
// left out.
void Field::typed(std::string_view what) {
    std::string kept;

    for (std::size_t at = 0; at < what.size();) {
        const std::size_t from = at;
        const std::optional<char32_t> one = text::decoded(what, at);

        if (!one) continue;

        Typing asked{*one};

        notified(asked);

        if (!asked.taken()) kept.append(what.substr(from, at - from));
    }

    Edit::made(*this, lining::Doing::typing, lining::Piece<std::size_t>{at(), {}, std::move(kept)});
}

// Erases the bytes from `from` to `upto`: back, as Backspace does, when they end at the cursor, and
// otherwise forth, as Delete does.
void Field::erased(std::size_t from, std::size_t upto) {
    const lining::Doing kind = upto == at() ? lining::Doing::back : lining::Doing::forth;

    Edit::made(*this, kind, lining::Piece<std::size_t>{from, text->substr(from, upto - from), {}});
}

// Sets the text and the cursor from an edit, keeps the cursor in view and asks for a guess.
void Field::restored(const std::string& made, std::size_t now) {
    editing_ = true;
    text = made;
    cursor = now;
    editing_ = false;

    followed();
    guessing();
}

// Moves the cursor to `to`; typing after it no longer joins the typing before.
void Field::moved(std::size_t to) {
    undo_.seal();

    editing_ = true;
    cursor = to;
    editing_ = false;

    followed();
    guessing();
}

// Shifts the text as little as it takes to show the cursor, and no further than the text needs. It
// goes with a change of the cursor, the text or the size, which paint anyway.
void Field::followed() {
    if (wide_ <= 0) return;

    const int here = column(at());
    const int whole = column(text->size()) + text::width(ahead_, rendering());

    if (here - shift_ >= wide_) shift_ = here - wide_ + 1;

    if (here < shift_) shift_ = here;

    shift_ = std::clamp(shift_, 0, std::max(whole - wide_ + 1, 0));
}

// Asks for a guess (`Guessing`) while the cursor stands at the end of a shown text; none otherwise.
void Field::guessing() {
    std::string made;

    if (!*hidden && at() == text->size()) {
        Guessing asked{*text};

        notified(asked);

        made = std::move(asked.ahead);
    }

    if (made == ahead_) return;

    ahead_ = std::move(made);

    repaint();
}

// The list of the history under its field, ten entries at most, in a holder of the class "history"
// too (`dropping::Drop`): the entry taken goes in the field.
class HistoryField::Drop : public dropping::Drop {
public:
    explicit Drop(HistoryField& owner) : dropping::Drop(owner, owner.history_, 0, 10, "history") {}

protected:
    // Puts the entry `row` in the field.
    void taken(Node& owner, std::size_t row) override {
        auto& field = static_cast<HistoryField&>(owner);
        const std::string chosen = field.history_.at(row);

        field.walked_.reset();
        field.put(chosen);
    }
};

// A field with the entries `earlier`, the newest first, in the class "input". Up
// and down walk through the history, the text typed coming back below the newest entry; typing
// leaves the walk. `open` shows the history in a list under the field, and disabling closes it.
HistoryField::HistoryField(std::vector<std::string> earlier) : history_(std::move(earlier)) {
    kept();

    heard_.add(on<Disabled>([this](Disabled&) { close(); }));

    heard_.add(on<Changed>([this](Changed& what) {
        if (what.of(text)) walked_.reset();

        if (what.of(keeps)) kept();
    }));

    heard_.add(otherwise<KeyEvent>([this](KeyEvent& what) {
        const Key key = what.key;

        if (key.mods() != Mods{} || (key.code() != Code::up && key.code() != Code::down)) return;

        what.take();

        if (key.code() == Code::up) {
            if (history_.empty()) return;

            walked(walked_ ? std::min(*walked_ + 1, history_.size() - 1) : 0);

            return;
        }

        if (!walked_) return;

        walked(*walked_ == 0 ? std::nullopt : std::optional<std::size_t>{*walked_ - 1});
    }));
}

// Puts `one` at the front of the history, taking away an equal entry further back and the oldest
// past `keeps`; an empty one is not kept.
void HistoryField::remember(std::string one) {
    if (one.empty()) return;

    std::erase(history_, one);
    history_.insert(history_.begin(), std::move(one));
    walked_.reset();
    kept();
}

// Shows the history as a list under the field, or over it when there is no room under it
// (`dropping::Drop`). Nothing without entries or layers, or while it shows.
void HistoryField::open() {
    if (history_.empty() || opened()) return;

    auto drop = std::make_shared<Drop>(*this);

    if (drop->dropped()) drop_ = drop;
}

// Closes the list of the history, when it shows.
void HistoryField::close() {
    if (const std::shared_ptr<Node> drop = drop_.lock()) static_cast<Drop&>(*drop).close();
}

// Whether the list of the history shows.
bool HistoryField::opened() const { return !drop_.expired(); }

// Shows the entry `to` of the history, or with none the text typed before the walk began. The text
// put ends the walk, as every change of the text does, so the walk is set again after it.
void HistoryField::walked(std::optional<std::size_t> to) {
    if (!walked_) typed_ = *text;

    put(to ? history_[*to] : typed_);

    walked_ = to;
}

// Keeps no more entries than `keeps`.
void HistoryField::kept() {
    if (history_.size() > *keeps) history_.resize(*keeps);
}

}
