#include "tal/controls.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "tal/editing.hpp"
#include "tal/key.hpp"
#include "tal/text.hpp"
#include "tal/views.hpp"
#include "tal/window.hpp"

#include "lettering.hpp"

namespace tal {

namespace {

// The first character of the cluster at the byte `at` of `said`, or none past its end.
std::optional<char32_t> leading(std::string_view said, std::size_t at) {
    if (at >= said.size()) return std::nullopt;

    return text::decoded(said, at);
}

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

// An edit of a field: its text and cursor before and after. Typing joins the next typing until a
// space, erasing the next erasing. The field seals its history when the cursor moves or the text
// changes otherwise, so an edit that is not sealed off goes on where the last one ended.
struct Field::Edit : Step {
    Edit(Field& owner, std::string old, std::size_t was, std::string made, std::size_t now, bool typing)
        : field(owner), before(std::move(old)), from(was), after(std::move(made)), to(now), typed(typing) {}

    // Puts back the text and cursor before.
    void undo() override { field.restored(before, from); }

    // Puts the text and cursor after again.
    void redo() override { field.restored(after, to); }

    // What the edit was, for a menu.
    [[nodiscard]] std::string said() const override { return typed ? "Typing" : "Erasing"; }

    // Takes in `later` when it is the same kind of edit and, for typing, did not type a space.
    bool merged(const Step& later) override {
        const auto* next = dynamic_cast<const Edit*>(&later);

        if (next == nullptr || next->typed != typed) return false;

        if (typed && next->to > next->from && next->after.substr(next->from, next->to - next->from).find(' ') != std::string::npos) return false;

        after = next->after;
        to = next->to;

        return true;
    }

    Field& field;
    std::string before;
    std::size_t from;
    std::string after;
    std::size_t to;
    bool typed;
};

// A field holding `said`, its cursor at the end, in the class "input": one line of text to type.
// The arrows, Home, End, Backspace and Delete work on the text's characters, Ctrl with an arrow
// goes a word, letters and pastes type; Ctrl+Z and Ctrl+Y undo and redo. Before a letter goes in
// the field tells `Typing`, and taking it refuses the letter; after every change it tells
// `Guessing`, and what a handler puts in `ahead` stands after the cursor at the end in the part
// "guess": the right arrow there takes one letter of it, End all of it. Tab and Enter are left to
// the nodes above. A click puts the cursor where it was.
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

// Sets the text to `said` with the cursor at its end, as an edit to undo on its own: a history
// walked, a guess taken in by the program.
void Field::put(std::string said) {
    const std::size_t end = said.size();

    undo_.seal();
    edited(std::move(said), end, false);
    undo_.seal();
}

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
// not a word, then past the word.
std::size_t Field::word(bool forth) const {
    const std::string& said = *text;
    const text::Rendering how = rendering();
    std::size_t made = at();

    const auto inside = [&said](std::size_t from) {
        const std::optional<char32_t> one = leading(said, from);

        return one && text::in_word(*one);
    };

    if (forth) {
        while (made < said.size() && !inside(made)) made = text::after(said, made, how);
        while (made < said.size() && inside(made)) made = text::after(said, made, how);
    } else {
        while (made > 0 && !inside(text::before(said, made, how))) made = text::before(said, made, how);
        while (made > 0 && inside(text::before(said, made, how))) made = text::before(said, made, how);
    }

    return made;
}

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

    const std::size_t here = at();
    const std::string& said = *text;

    edited(said.substr(0, here) + kept + said.substr(here), here + kept.size(), true);
}

// Erases the bytes from `from` to `upto`.
void Field::erased(std::size_t from, std::size_t upto) {
    const std::string& said = *text;

    edited(said.substr(0, from) + said.substr(upto), from, false);
}

// The text becomes `made` with the cursor at `now`, as a step of the field's history; nothing when
// both stay as they are, as Backspace at the start.
void Field::edited(std::string made, std::size_t now, bool typing) {
    const std::string before = *text;
    const std::size_t was = at();

    if (made == before && now == was) return;

    restored(made, now);
    undo_.push(std::make_unique<Edit>(*this, before, was, std::move(made), now, typing));
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

// The window the history opens in under its field: modal, in the class "popup" too, holding the
// list of entries in a holder of the class "history", which the sheet frames. Enter, a click or a
// double click on an entry puts it in the field and closes the window; Esc and a press outside it
// close it.
class HistoryField::Drop : public Window {
public:
    Drop(HistoryField& owner, Size size);

private:
    // The list of entries, which takes an entry at a click.
    class Entries : public List {
    public:
        Entries(std::shared_ptr<Text> lines, Drop& drop) : List(std::move(lines)), drop_(drop) {}

    protected:
        // Takes the entry clicked.
        void clicked(std::size_t row, int x) override {
            (void)x;

            drop_.taken(row);
        }

    private:
        Drop& drop_;
    };

    // A holder of a fixed size, so that the window has the size of the list it shows.
    class Holder : public Node {
    public:
        explicit Holder(Size size) : size_(size) { classes({"history"}); }

    protected:
        // The size it was made with.
        [[nodiscard]] Size measure(Limits room) const override {
            (void)room;

            return size_;
        }

    private:
        Size size_;
    };

    void taken(std::size_t row);

    HistoryField& field_;
    Hooks heard_;
};

// A window for `owner`'s history showing a list of `size`.
HistoryField::Drop::Drop(HistoryField& owner, Size size) : Window({}, Modal::yes), field_(owner) {
    add_class("popup");

    std::string all;

    for (const std::string& one : owner.history_) all += (all.empty() ? "" : "\n") + one;

    Holder& holder = children().add<Holder>(size);
    Entries& entries = holder.children().add<Entries>(std::make_shared<Text>(all), *this);

    heard_.add(entries.on<Picked>([this](Picked& what) { taken(what.row); }));

    heard_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        if (&what.target() != this || what.press != Press::down) return;

        what.take();
        close();
    }));
}

// Puts the entry `row` in the field and closes.
void HistoryField::Drop::taken(std::size_t row) {
    HistoryField& field = field_;
    const std::string chosen = field.history_.at(row);

    close();
    field.walked_.reset();
    field.put(chosen);
}

// A field with the entries `earlier`, the newest first, in the class "input". Up
// and down walk through the history, the text typed coming back below the newest entry; typing
// leaves the walk. `open` shows the history in a list under the field.
HistoryField::HistoryField(std::vector<std::string> earlier) : history_(std::move(earlier)) {
    kept();

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

// Shows the history as a list under the field, in the nearest layers above it, as wide as the field
// and as high as the entries, ten at most. Nothing without entries or layers, or while it shows.
void HistoryField::open() {
    if (history_.empty() || opened()) return;

    Layers* layers = nullptr;

    for (Node* at = parent(); at != nullptr && layers == nullptr; at = at->parent()) layers = dynamic_cast<Layers*>(at);

    if (layers == nullptr) return;

    Spot at = area().at;

    for (const Node* above = parent(); above != nullptr; above = above->parent()) at = Spot{at.x + above->area().at.x, at.y + above->area().at.y};

    auto drop = std::make_shared<Drop>(*this, Size{std::max(area().size.wide, 1), static_cast<int>(std::min<std::size_t>(history_.size(), 10))});

    drop->anchor(layers->local(Spot{at.x, at.y + area().size.high}));
    drop_ = drop;

    (void)layers->open(drop);
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
