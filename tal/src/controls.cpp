#include "tal/controls.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <utility>

#include "tal/key.hpp"
#include "tal/style.hpp"
#include "tal/text.hpp"

#include "controlling.hpp"
#include "lettering.hpp"

namespace tal {

namespace {

// How long a busy gauge's block stands before it moves on: the eye sees motion, and an idle program
// wakes ten times a second, and only while something is really busy.
constexpr std::chrono::milliseconds beat{100};

// The widest text that never needs a second line: what a label measures without wrapping against.
constexpr int unbounded = 1 << 20;

}

namespace controlling {

// Whether `cluster` is the letter `letter`, the same character or, for ASCII, the same letter in
// the other case.
bool lettered(std::string_view cluster, std::optional<char32_t> letter) {
    std::size_t at = 0;
    const std::optional<char32_t> first = text::decoded(cluster, at);

    return first && lettering::same(*first, letter);
}

// Writes `said` at `at`, its first cluster that is `letter` in `marked` and the rest in `plain`;
// returns how many columns it took.
int spelled(Canvas& onto, Spot at, std::string_view said, std::optional<char32_t> letter, const Look& plain, const Look& marked,
            text::Rendering how) {
    bool found = false;
    int x = at.x;

    for (const std::string_view one : text::clusters(said, how)) {
        const bool here = !found && lettered(one, letter);

        found = found || here;
        x += onto.put({x, at.y}, one, here ? marked : plain);
    }

    return x - at.x;
}

// Whether `key` is Enter or the space bar without modifiers: the keys that press a button.
bool pressing(Key key) {
    if (key.mods() != Mods{}) return false;

    return key.code() == Code::enter || key.letter() == U' ';
}

// Whether the mouse event is a click of the left button.
bool clicking(const MouseEvent& one) { return one.press == Press::click && one.button == MouseButton::left; }

// The name of a line's kind in the glyphs of a sheet; no border draws the thin lines.
std::string_view lined(Border kind) {
    switch (kind) {
        case Border::twin: return "twin";
        case Border::thick: return "thick";
        case Border::none:
        case Border::thin: break;
    }

    return "thin";
}

}

using controlling::clicking;
using controlling::lined;
using controlling::pressing;
using controlling::spelled;

// A label showing `said`, in the class "label".
Label::Label(std::string said) : text(*this, Effect::layout, std::move(said)) { classes({"label"}); }

// As wide as its widest line and as high as its lines, wrapped to the room when it wraps.
Size Label::measure(Limits room) const {
    const std::string& said = *text;
    const text::Rendering how = rendering();
    const std::vector<Stretch> shown = lines(*wrap ? room.most.wide : unbounded);
    int wide = 0;

    for (const Stretch& one : shown) wide = std::max(wide, text::width(std::string_view{said}.substr(one.from, one.upto - one.from), how));

    return Size{wide, static_cast<int>(shown.size())};
}

// The lines that show, as stretches of the text: one per line of the text, or, when it wraps, the
// lines `text::wrapped` makes at `wide` columns, without the spaces it leaves at the end of a line
// broken after them, since they show nothing and would only make the line look too wide.
std::vector<Label::Stretch> Label::lines(int wide) const {
    const std::string& said = *text;
    std::vector<Stretch> made;

    if (*wrap) {
        for (const std::string_view piece : text::wrapped(said, wide, rendering())) {
            const auto from = static_cast<std::size_t>(piece.data() - said.data());
            std::size_t upto = from + piece.size();

            while (upto > from && said[upto - 1] == ' ') --upto;

            made.push_back(Stretch{from, upto});
        }

        return made;
    }

    std::size_t start = 0;

    for (;;) {
        const std::size_t found = said.find('\n', start);

        if (found == std::string::npos) {
            made.push_back(Stretch{start, said.size()});

            return made;
        }

        made.push_back(Stretch{start, found});
        start = found + 1;
    }
}

// The lines one under another, each standing as `justify` says. A line wider than the label is cut
// where `elide` says, with the sheet's `elide` glyph in the gap; without elision its end is cut.
void Label::paint(Canvas& onto) const {
    const std::string& said = *text;
    const text::Rendering how = rendering();
    const int wide = onto.size().wide;
    const std::vector<Stretch> shown = lines(wide);
    const std::string_view mark = glyph("elide");
    const int marked = text::width(mark, how);

    for (std::size_t y = 0; y < shown.size() && static_cast<int>(y) < onto.size().high; ++y) {
        const Stretch line = shown[y];
        const std::string_view view = std::string_view{said}.substr(line.from, line.upto - line.from);
        const int width = text::width(view, how);

        // The pieces of the line in order: stretches of the text and, where it was cut, the glyph.
        struct Piece {
            Stretch part;
            bool glyph = false;
        };

        std::vector<Piece> pieces{Piece{line}};

        if (width > wide && *elide != Elide::none) {
            std::vector<std::string_view> clusters = text::clusters(view, how);

            // The first clusters that fit `room` columns, as a stretch, and their width.
            const auto front = [&](int room, int& taken) {
                std::size_t upto = line.from;

                taken = 0;

                for (const std::string_view one : clusters) {
                    const int cells = text::width(one, how);

                    if (taken + cells > room) break;

                    taken += cells;
                    upto = static_cast<std::size_t>(one.data() - said.data()) + one.size();
                }

                return Stretch{line.from, upto};
            };

            // The last clusters that fit `room` columns, as a stretch.
            const auto back = [&](int room) {
                std::size_t from = line.upto;
                int taken = 0;

                for (auto one = clusters.rbegin(); one != clusters.rend(); ++one) {
                    const int cells = text::width(*one, how);

                    if (taken + cells > room) break;

                    taken += cells;
                    from = static_cast<std::size_t>(one->data() - said.data());
                }

                return Stretch{from, line.upto};
            };

            const int room = wide - marked;
            int taken = 0;

            if (room < 0) pieces = {Piece{front(wide, taken)}};
            else if (*elide == Elide::end) pieces = {Piece{front(room, taken)}, Piece{{}, true}};
            else if (*elide == Elide::start) pieces = {Piece{{}, true}, Piece{back(room)}};
            else {
                const Stretch first = front((room + 1) / 2, taken);

                pieces = {Piece{first}, Piece{{}, true}, Piece{back(room - taken)}};
            }
        }

        int length = 0;

        for (const Piece& one : pieces)
            length += one.glyph ? marked : text::width(std::string_view{said}.substr(one.part.from, one.part.upto - one.part.from), how);

        const int spare = std::max(wide - length, 0);
        int x = *justify == Edge::start ? 0 : *justify == Edge::middle ? spare / 2 : spare;

        for (const Piece& one : pieces)
            x += one.glyph ? onto.put({x, static_cast<int>(y)}, mark, look()) : written(onto, {x, static_cast<int>(y)}, one.part);
    }
}

// Writes the stretch `part` of the text at `at`, cut where marks begin and end: what lies under a
// mark in the look of its part and, when it has an address, as a link; the rest in the label's
// look. Returns how many columns it took.
int Label::written(Canvas& onto, Spot at, Stretch part) const {
    const std::string& said = *text;
    const std::vector<Mark>& all = *marks;
    std::vector<std::size_t> cuts{part.from, part.upto};

    for (const Mark& one : all)
        for (const std::size_t edge : {one.from, one.upto})
            if (edge > part.from && edge < part.upto) cuts.push_back(edge);

    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    int x = at.x;

    for (std::size_t piece = 0; piece + 1 < cuts.size(); ++piece) {
        const std::size_t from = cuts[piece];
        const auto found = std::find_if(all.begin(), all.end(), [from](const Mark& one) { return one.from <= from && from < one.upto; });
        const int width = onto.put({x, at.y}, std::string_view{said}.substr(from, cuts[piece + 1] - from), found == all.end() ? look() : look(found->part));

        if (found != all.end() && !found->address.empty()) onto.link(Area{{x, at.y}, {width, 1}}, found->address);

        x += width;
    }

    return x - at.x;
}

// A status line showing `said`, in the class "status", cut at its end when it does not fit.
Status::Status(std::string said) : Label(std::move(said)) {
    classes({"status"});

    elide = Elide::end;
}

// One row over the whole width it gets.
Size Status::measure(Limits room) const { return Size{room.most.wide, 1}; }

// A rule along `axis` with `named` in its middle as its title, in the class "rule".
Rule::Rule(Axis axis, std::string named) : title(*this, Effect::layout, std::move(named)), axis_(axis) { classes({"rule"}); }

// One row (or column) as long as the room's least, and long enough for the title with a line on
// either side.
Size Rule::measure(Limits room) const {
    if (axis_ == Axis::vertical) return Size{1, std::max(room.least.high, 1)};

    const int named = title->empty() ? 1 : text::width(*title, rendering()) + 4;

    return Size{std::max(room.least.wide, named), 1};
}

// The line drawn with the sheet's glyphs of the rule's kind (thin for none), and the title in the
// middle between two spaces, in the look of the part "title". The kind is the rule's own: a border
// from the sheet would frame the rule's box instead.
void Rule::paint(Canvas& onto) const {
    const std::string stem = std::string{"line."} + std::string{lined(*kind)} + ".";

    if (axis_ == Axis::vertical) {
        const std::string_view upright = glyph(stem + "upright");

        for (int y = 0; y < onto.size().high; ++y) (void)onto.put({0, y}, upright, look());

        return;
    }

    const std::string_view across = glyph(stem + "across");

    for (int x = 0; x < onto.size().wide; ++x) (void)onto.put({x, 0}, across, look());

    if (title->empty()) return;

    const std::string named = " " + *title + " ";
    const int width = text::width(named, rendering());

    (void)onto.put({std::max((onto.size().wide - width) / 2, 0), 0}, named, look("title"));
}

// A button for `one`: its text, its letter and whether it is enabled follow the action. Enter, the
// space bar and a click run the action; they are default actions, so the program may take the
// event before them.
Button::Button(std::shared_ptr<Action> one) : action_(std::move(one)) {
    classes({"button"});
    focusable(true);
    enable(action_->enabled());

    hooks_.add(action_->on<Altered>([this](Altered& what) {
        enable(what.action.enabled());
        relayout();
    }));

    hooks_.add(otherwise<KeyEvent>([this](KeyEvent& what) {
        if (!pressing(what.key)) return;

        what.take();
        action_->run();
    }));

    hooks_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        if (!clicking(what)) return;

        what.take();
        action_->run();
    }));
}

// The text in brackets: `[ text ]`, or `[< text >]` for the primary button.
Size Button::measure(Limits room) const {
    (void)room;

    return Size{text::width(action_->text(), rendering()) + (*primary ? 6 : 4), 1};
}

// The brackets and the text, its letter in the look of the part "key".
void Button::paint(Canvas& onto) const {
    const Look plain = look();
    int x = onto.put({0, 0}, *primary ? "[< " : "[ ", plain);

    x += spelled(onto, {x, 0}, action_->text(), action_->letter(), plain, look("key"), rendering());

    (void)onto.put({x, 0}, *primary ? " >]" : " ]", plain);
}

// A check box for `one`, ticked when `on`: its text, letter and whether it is enabled follow the
// action. Enter, the space bar and a click turn it over and then run the action. The state is
// `ticked`, not `on`, which would hide `Node::on`.
Tick::Tick(std::shared_ptr<Action> one, bool on) : ticked(*this, Effect::paint, on), action_(std::move(one)) {
    classes({"tick"});
    focusable(true);
    enable(action_->enabled());

    hooks_.add(action_->on<Altered>([this](Altered& what) {
        enable(what.action.enabled());
        relayout();
    }));

    hooks_.add(otherwise<KeyEvent>([this](KeyEvent& what) {
        if (!pressing(what.key)) return;

        what.take();
        toggled();
    }));

    hooks_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        if (!clicking(what)) return;

        what.take();
        toggled();
    }));
}

// The box and the text: `[x] text`.
Size Tick::measure(Limits room) const {
    (void)room;

    return Size{text::width(action_->text(), rendering()) + 4, 1};
}

// The box, ticked or not, and the text, its letter in the look of the part "key".
void Tick::paint(Canvas& onto) const {
    const Look plain = look();
    const int x = onto.put({0, 0}, *ticked ? "[x] " : "[ ] ", plain);

    (void)spelled(onto, {x, 0}, action_->text(), action_->letter(), plain, look("key"), rendering());
}

// Turns the box over and runs the action, which sees the new state.
void Tick::toggled() {
    ticked = !*ticked;

    action_->run();
}

// A group of radio options with the option `which` chosen (the last when it is past the end), in
// the class "choice". The group takes the focus as a whole, as a list does: Tab enters it once and
// leaves it once, the arrows, Home and End choose at once, as in Midnight Commander, and a click
// chooses the option under it. A group without options takes no focus.
Choice::Choice(std::vector<std::string> options, std::size_t which)
    : chosen(*this, Effect::paint, options.empty() ? 0 : std::min(which, options.size() - 1)), options_(std::move(options)) {
    classes({"choice"});
    focusable(!options_.empty());

    hooks_.add(otherwise<KeyEvent>([this](KeyEvent& what) {
        if (options_.empty() || what.key.mods() != Mods{}) return;

        const std::size_t now = std::min(*chosen, options_.size() - 1);
        const std::size_t last = options_.size() - 1;

        switch (what.key.code()) {
            case Code::up: chosen = now == 0 ? 0 : now - 1; break;
            case Code::down: chosen = std::min(now + 1, last); break;
            case Code::home: chosen = 0; break;
            case Code::end: chosen = last; break;
            default: return;
        }

        what.take();
    }));

    hooks_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        if (!clicking(what)) return;

        if (what.at.y < 0 || static_cast<std::size_t>(what.at.y) >= options_.size()) return;

        what.take();
        chosen = static_cast<std::size_t>(what.at.y);
    }));
}

// As wide as the widest option with its bracket, one row per option.
Size Choice::measure(Limits room) const {
    (void)room;

    int wide = 0;

    for (const std::string& one : options_) wide = std::max(wide, text::width(one, rendering()) + 4);

    return Size{wide, static_cast<int>(options_.size())};
}

// One row per option: `( ) option`, the chosen one with the sheet's dot in its bracket.
void Choice::paint(Canvas& onto) const {
    const Look plain = look();
    const std::string_view dot = glyph("choice.dot");

    for (std::size_t at = 0; at < options_.size() && static_cast<int>(at) < onto.size().high; ++at) {
        const int y = static_cast<int>(at);
        int x = onto.put({0, y}, "(", plain);

        x += onto.put({x, y}, at == *chosen ? dot : std::string_view{" "}, plain);
        x += onto.put({x, y}, ") ", plain);

        (void)onto.put({x, y}, options_[at], plain);
    }
}

// In the bracket of the chosen option.
std::optional<Caret> Choice::caret() const {
    if (*chosen >= options_.size()) return std::nullopt;

    return Caret{{1, static_cast<int>(*chosen)}};
}

// A progress bar in the class "gauge", empty and showing its percentage.
Gauge::Gauge() {
    classes({"gauge"});

    hooks_.add(on<Changed>([this](Changed& what) {
        if (what.of(busy)) beating();
    }));

    hooks_.add(on<Arranged>([this](Arranged&) { beating(); }));
}

// At least ten cells for the bar with its brackets, and five before it for the percentage.
Size Gauge::measure(Limits room) const { return Size{std::max(room.least.wide, (*percent ? 5 : 0) + 10), 1}; }

// The percentage, then the bar in the sheet's glyphs: done cells in the look of the part "done",
// the rest in the gauge's look. A busy gauge shows a block running to and fro instead, and no
// number. The fraction is cut to [0, 1]; the inverted test also takes NaN, which is neither, as
// zero.
void Gauge::paint(Canvas& onto) const {
    const Look plain = look();
    const Look filled = look("done");
    const double part = !(*done >= 0.0) ? 0.0 : std::min(*done, 1.0);
    int x = 0;

    if (*percent) {
        std::string shown(5, ' ');

        if (!*busy) {
            const std::string number = std::to_string(std::lround(part * 100.0));

            shown = std::string(3 - number.size(), ' ') + number + "% ";
        }

        x += onto.put({0, 0}, shown, look("text"));
    }

    const int bar = onto.size().wide - x;

    if (bar < 2) return;

    const int inner = bar - 2;
    const std::string_view done_glyph = glyph("gauge.done");
    const std::string_view rest_glyph = glyph("gauge.rest");
    int from = 0;
    int upto = static_cast<int>(std::lround(part * inner));

    if (*busy) {
        const int block = std::max(1, inner / 5);
        const int span = std::max(inner - block, 0);
        int at = span == 0 ? 0 : at_ % (2 * span);

        if (at > span) at = 2 * span - at;

        from = at;
        upto = at + block;
    }

    (void)onto.put({x, 0}, glyph("gauge.left"), plain);

    for (int cell = 0; cell < inner; ++cell) {
        const bool lit = cell >= from && cell < upto;

        (void)onto.put({x + 1 + cell, 0}, lit ? done_glyph : rest_glyph, lit ? filled : plain);
    }

    (void)onto.put({x + 1 + inner, 0}, glyph("gauge.right"), plain);
}

// Keeps the busy block running on the document's clock while the gauge is busy and in a document,
// and stops it otherwise.
void Gauge::beating() {
    if (!*busy || document() == nullptr) {
        beat_ = Timer{};

        return;
    }

    if (beat_.waiting()) return;

    beat_ = document()->after(beat, [this](Timer& one) {
        ++at_;
        repaint();
        one.again(beat);
    });
}

}
