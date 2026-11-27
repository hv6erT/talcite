#include "tal/views.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "tal/key.hpp"
#include "tal/text.hpp"

#include "controlling.hpp"
#include "dividing.hpp"

namespace tal {

namespace {

// How many rows a step of the wheel moves a view, as in Midnight Commander.
constexpr std::size_t wheeled = 3;

// The columns between two columns of a table.
constexpr int spacing = 1;

// `at` moved by `by` rows, not past the first; the view keeps itself before the last.
std::size_t stepped(std::size_t at, long long by) {
    const long long made = static_cast<long long>(at) + by;

    return made < 0 ? 0 : static_cast<std::size_t>(made);
}

}

// A list of the rows of `model`, in the class "list": the first column of each, one row of the
// screen to a row. The arrows, the page keys, Home and End move the current row, Enter tells
// `Picked`; these are default actions, so the program may take the key first. A click makes a row
// current (the press gave the list the focus), and a click on a mark with an address tells
// `Followed` too, which the document opens when nobody takes it; a double click tells `Picked`, and
// the wheel moves the view only.
List::List(std::shared_ptr<Model> model) : model_(std::move(model)) {
    if (model_ == nullptr) throw std::invalid_argument{"tal::List: no model"};

    classes({"list"});
    focusable(true);

    told_ = model_->on<Changes>([this](Changes& what) { changed(what); });

    hooks_.add(on<Changed>([this](Changed& what) {
        if (what.of(current)) lit();
    }));

    hooks_.add(otherwise<KeyEvent>([this](KeyEvent& what) {
        const std::size_t many = extent();

        if (many == 0 || what.key.mods() != Mods{}) return;

        const std::size_t page = std::max<std::size_t>(window_, 1);
        const std::size_t here = shown();

        switch (what.key.code()) {
            case Code::up: current = here == 0 ? 0 : here - 1; break;
            case Code::down: current = std::min(here + 1, many - 1); break;
            case Code::pageup: moved(here > page ? here - page : 0, top_ > page ? top_ - page : 0); break;
            case Code::pagedown: moved(std::min(here + page, many - 1), top_ + page); break;
            case Code::home: current = 0; break;
            case Code::end: current = many - 1; break;
            case Code::enter: {
                Picked one{here};

                what.take();
                notified(one);

                return;
            }
            default: return;
        }

        what.take();
    }));

    hooks_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        if (what.press == Press::wheel) {
            if (what.axis != Axis::vertical) return;

            const std::size_t was = top_;

            top(stepped(top_, -static_cast<long long>(wheeled) * what.steps));

            if (top_ != was) what.take();

            return;
        }

        if (what.button != MouseButton::left || (what.press != Press::click && what.press != Press::double_click)) return;

        const std::optional<std::size_t> row = under(what.at);

        if (!row) return;

        what.take();

        if (what.press == Press::double_click) {
            Picked one{*row};

            notified(one);

            return;
        }

        current = *row;
        clicked(*row, what.at.x - inset().left);

        const Sides in = inset();
        const int wide = area().size.wide - in.left - in.right;
        const std::size_t at = *row;

        if (at >= extent()) return;

        std::string address = controlling::addressed(wide, what.at.x - in.left, rendering(), [this, at](Canvas& onto) { drawn(onto, at, States{}); });

        if (address.empty()) return;

        Followed one{std::move(address)};

        notified(one);
        controlling::unheeded(*this, one);
    }));
}

// Marks the row `row` (`on`) or takes its mark away; a row past the last throws std::out_of_range.
void List::mark(std::size_t row, bool on) {
    if (row >= extent()) throw std::out_of_range{"tal::List::mark: no such row"};

    const auto at = std::lower_bound(marks_.begin(), marks_.end(), row);
    const bool was = at != marks_.end() && *at == row;

    if (was == on) return;

    if (on) marks_.insert(at, row);
    else marks_.erase(at);

    redrawn(row, 1);
}

// Whether the row `row` is marked.
bool List::marked(std::size_t row) const { return std::binary_search(marks_.begin(), marks_.end(), row); }

// Takes every mark away.
void List::unmark() {
    const std::vector<std::size_t> were = std::move(marks_);

    marks_.clear();

    for (const std::size_t one : were) redrawn(one, 1);
}

// How many rows the model has.
std::size_t List::extent() const { return model_->rows(); }

// Shows the rows from `row` on, as far as the last row lets the view stay full; only a paint, and
// the view tells `Scrolled`.
void List::top(std::size_t row) {
    const std::size_t many = extent();
    const std::size_t made = std::min(row, many > window_ ? many - window_ : 0);

    if (made == top_) return;

    top_ = made;

    repaint();
    scrolled();
}

// The least room: the layout gives a view its size (a weight, a seat), not the number of its rows,
// which would lay the screen out again at every row that comes or goes.
Size List::measure(Limits room) const { return room.least; }

// Takes the rows the inside has for the window, keeps the view inside the rows and the current row
// in the view, and tells `Scrolled` when the window or the view moved.
void List::arrange(Area inside) {
    const std::size_t was = window_;
    const std::size_t from = top_;
    const std::size_t many = extent();
    const std::size_t here = shown();

    window_ = static_cast<std::size_t>(std::max(inside.size.high - above(), 0));
    top_ = std::min(top_, many > window_ ? many - window_ : 0);

    if (window_ > 0 && here < top_) top_ = here;

    if (window_ > 0 && here >= top_ + window_) top_ = here - window_ + 1;

    if (window_ != was || top_ != from) scrolled();
}

// The ground and the rows this frame paints, each in the look of its states, past the last row the
// look of a plain row. The view paints its ground itself, since the focus of the view is the look
// of its current row, not of all of it. The row under the cursor is `current` always and `focus`
// too while the view has the focus, so that a sheet can show where the cursor stands in a view
// the focus left.
void List::paint(Canvas& onto) const {
    const Area seen = onto.seen();
    const int head = above();
    const std::size_t many = extent();
    const std::size_t here = shown();
    const bool lit = states().has(State::focus);
    States ground;

    ground.set(State::disabled, states().has(State::disabled));

    onto.fill(seen, look({}, ground));

    for (int y = std::max(seen.at.y, head); y < seen.at.y + seen.size.high; ++y) {
        const std::size_t row = top_ + static_cast<std::size_t>(y - head);

        if (row >= many) break;

        States states = ground;

        states.set(State::current, row == here);
        states.set(State::focus, lit && row == here);
        states.set(State::mark, marked(row));

        Canvas line = onto.within(Area{{0, y}, {onto.size().wide, 1}});

        line.fill(Area{{0, 0}, line.size()}, look({}, states));
        drawn(line, row, states);
    }
}

// No rows above the first: a list has no heading.
int List::above() const { return 0; }

// The first column of the row `row` on `onto`, a canvas of the row's width.
void List::drawn(Canvas& onto, std::size_t row, States states) const { written(onto, model_->entry(row, 0), Edge::start, states); }

// A click at the column `x` of the row `row`, after it became current: nothing more for a list.
void List::clicked(std::size_t row, int x) {
    (void)row;
    (void)x;
}

// The entry `one` on `onto`, a canvas of its width, stood as `align` says: in the look of its part
// in `states` (a part of its own fills the canvas), or of the row without one; its marks in the
// looks of their parts, cut with the glyph `elide` as it asks.
void List::written(Canvas& onto, const Entry& one, Edge align, States states) const {
    const Look plain = one.part.empty() ? look({}, states) : look(one.part, states);
    std::vector<Look> marked;

    marked.reserve(one.marks.size());

    for (const Mark& mark : one.marks) marked.push_back(look(mark.part, states));

    if (!one.part.empty()) onto.fill(Area{{0, 0}, onto.size()}, plain);

    (void)controlling::line(onto, {0, 0}, onto.size().wide, one.text, 0, one.text.size(), one.marks, marked, one.elide, align, plain,
                            glyph("elide"), rendering());
}

// The current row as shown: `current`, or the last row when it lies past it.
std::size_t List::shown() const {
    const std::size_t many = extent();

    return many == 0 ? 0 : std::min(*current, many - 1);
}

// The border and padding: how far the inside lies from the edges of the box.
Sides List::inset() const { return controlling::boxed(style()); }

// Paints again the rows from `from` on, `count` of them, where they stand in the view.
void List::redrawn(std::size_t from, std::size_t count) {
    const std::size_t start = std::max(from, top_);
    const std::size_t end = std::min(from + count, top_ + window_);

    if (start >= end) return;

    const Sides in = inset();
    const int y = in.top + above() + static_cast<int>(start - top_);

    repaint(Area{{in.left, y}, {area().size.wide - in.left - in.right, static_cast<int>(end - start)}});
}

// The row under `at` (in the box), or none: above the rows, past the last, or outside the inside.
std::optional<std::size_t> List::under(Spot at) const {
    const Sides in = inset();
    const int y = at.y - in.top - above();
    const int x = at.x - in.left;

    if (y < 0 || static_cast<std::size_t>(y) >= window_ || x < 0 || x >= area().size.wide - in.left - in.right) return std::nullopt;

    const std::size_t row = top_ + static_cast<std::size_t>(y);

    if (row >= extent()) return std::nullopt;

    return row;
}

// Follows what the model changed. Rows that come or go above the view move the view, the current
// row and the marks along, so that the screen stays; in the view the rows from the change on are
// painted again, a change paints its rows, and a reset takes the marks away and paints all.
void List::changed(const Changes& what) {
    const std::size_t many = extent();
    const std::size_t from = what.from;
    const std::size_t count = what.count;

    switch (what.kind) {
        case Changes::Kind::inserted: {
            const bool before = from < top_;

            for (std::size_t& one : marks_)
                if (one >= from) one += count;

            if (before) top_ += count;

            if (many > count && from <= lit_) lit_ += count;

            if (many > count && from <= *current) current = *current + count;

            if (!before) redrawn(from, window_);

            break;
        }

        case Changes::Kind::removed: {
            const std::size_t end = from + count;
            const bool before = end <= top_;
            const auto pulled = [from, end, count](std::size_t one) { return one >= end ? one - count : std::min(one, from); };

            std::erase_if(marks_, [from, end](std::size_t one) { return one >= from && one < end; });

            for (std::size_t& one : marks_) one = pulled(one);

            top_ = pulled(top_);
            lit_ = pulled(lit_);

            if (*current >= from) current = pulled(*current);

            if (const std::size_t most = many > window_ ? many - window_ : 0; top_ > most) {
                top_ = most;

                repaint();
            } else if (!before) {
                redrawn(std::max(from, top_), window_);
            }

            break;
        }

        case Changes::Kind::changed: redrawn(from, count); return;

        case Changes::Kind::reset:
            marks_.clear();
            top_ = std::min(top_, many > window_ ? many - window_ : 0);

            repaint();

            break;
    }

    scrolled();
}

// The current row changed: the row lit before and the new one are painted again, and the view
// moves to show the new one.
void List::lit() {
    const std::size_t here = shown();

    if (here == lit_) return;

    redrawn(lit_, 1);

    lit_ = here;

    if (window_ > 0 && here < top_) top(here);
    else if (window_ > 0 && here >= top_ + window_) top(here - window_ + 1);

    redrawn(here, 1);
}

// Makes `row` current and moves the view to start at `view`, as far as the rows let it; the current
// row then stays in the view.
void List::moved(std::size_t row, std::size_t view) {
    top(view);

    current = row;
}

// Tells the view's bar that the rows, the window or the view moved.
void List::scrolled() {
    Scrolled one;

    notified(one);
}

// A table of the rows of `model` in the columns `headings`, in the class "table", under a row of
// their titles in the part "head". A click on a title tells `Sorted` with its column: the program
// sorts its model and tells a reset.
Table::Table(std::shared_ptr<Model> model, std::vector<Heading> headings)
    : List(std::move(model)), columns(*this, Effect::paint, std::move(headings)) {
    classes({"table"});

    heard_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        if (what.press != Press::click || what.button != MouseButton::left || !*heading) return;

        const Sides in = inset();
        const int wide = area().size.wide - in.left - in.right;
        const int x = what.at.x - in.left;

        if (what.at.y != in.top || x < 0 || x >= wide) return;

        const std::vector<int> widths = spread(wide);
        int at = 0;

        for (std::size_t one = 0; one < widths.size(); ++one) {
            if (widths[one] == 0) continue;

            if (x < at + widths[one] + spacing) {
                Sorted told{one};

                what.take();
                notified(told);

                return;
            }

            at += widths[one] + spacing;
        }
    }));
}

// The rows, then the titles over them when the table shows them, each in its column as the cells
// stand, cut with the glyph `elide` at the end.
void Table::paint(Canvas& onto) const {
    List::paint(onto);

    if (!*heading) return;

    const Look head = look("head");
    const std::vector<int> widths = spread(onto.size().wide);
    int at = 0;

    onto.fill(Area{{0, 0}, {onto.size().wide, 1}}, head);

    for (std::size_t one = 0; one < widths.size(); ++one) {
        if (widths[one] == 0) continue;

        const Heading& column = (*columns)[one];
        Canvas cell = onto.within(Area{{at, 0}, {widths[one], 1}});

        (void)controlling::line(cell, {0, 0}, widths[one], column.title, 0, column.title.size(), {}, {}, Elide::end, column.align, head,
                                glyph("elide"), rendering());

        at += widths[one] + spacing;
    }
}

// The row of titles, when the table shows it.
int Table::above() const { return *heading ? 1 : 0; }

// The cells of the row `row`, each in its column; a cell that spans takes the rest of the row.
void Table::drawn(Canvas& onto, std::size_t row, States states) const {
    const std::vector<int> widths = spread(onto.size().wide);
    int at = 0;

    for (std::size_t one = 0; one < widths.size(); ++one) {
        if (widths[one] == 0) continue;

        const Entry entry = model().entry(row, one);
        const int room = entry.spans ? onto.size().wide - at : widths[one];
        Canvas cell = onto.within(Area{{at, 0}, {std::max(room, 0), 1}});

        written(cell, entry, (*columns)[one].align, states);

        if (entry.spans) return;

        at += widths[one] + spacing;
    }
}

// The widths of the columns in `wide` columns, a column apart: as `Row` shares its room, a column
// of weight 0 wanting exactly its `least`; where the room is too narrow for all, the columns that
// yield give up their place first (`Share::yields`) and have width 0, with no gap beside them.
std::vector<int> Table::spread(int wide) const {
    std::vector<int> wanted;
    std::vector<Share> shares;

    for (const Heading& one : *columns) {
        wanted.push_back(one.share.least);
        shares.push_back(one.share);
    }

    return dividing::yielded(wanted, shares, wide, spacing);
}

// A tree of the rows of `model`, in the class "tree" too: each row two columns in for each level of
// depth, then the mark of its branch, a space and its first column. The right arrow opens a closed
// branch, the left one closes an open branch and goes from any other row to its parent; a click on
// the mark opens or closes the branch.
Tree::Tree(std::shared_ptr<TreeModel> model) : List(std::move(model)) {
    classes({"list", "tree"});

    heard_.add(otherwise<KeyEvent>([this](KeyEvent& what) {
        const Code code = what.key.code();

        if (extent() == 0 || what.key.mods() != Mods{} || (code != Code::left && code != Code::right)) return;

        const std::size_t here = shown();
        const Branch branch = tree().branch(here);

        if (code == Code::right) {
            if (branch != Branch::closed) return;

            what.take();
            tree().open(here, true);

            return;
        }

        what.take();

        if (branch == Branch::open) {
            tree().open(here, false);

            return;
        }

        const int depth = tree().depth(here);

        for (std::size_t row = here; row-- > 0;)
            if (tree().depth(row) < depth) {
                current = row;

                return;
            }
    }));
}

// The row `row`: its indent, the glyph of its branch (`tree.open`, `tree.closed`, or a space for a
// leaf), a space and its first column. With `branches` the indent holds the lines of the branches
// (the part "branch"): down from every level whose row has more below it, and into the row from
// the left, a joint when more follow it at its level and a corner at the last.
void Tree::drawn(Canvas& onto, std::size_t row, States states) const {
    const int depth = std::max(tree().depth(row), 0);
    const int indent = 2 * depth;
    const Branch branch = tree().branch(row);
    const std::string_view mark = branch == Branch::open ? glyph("tree.open") : branch == Branch::closed ? glyph("tree.closed") : " ";

    if (*branches && depth > 0) {
        const Look lines = look("branch", states);

        for (int level = 1; level < depth; ++level)
            if (followed(row, level)) (void)onto.put({2 * (level - 1), 0}, glyph("line.thin.upright"), lines);

        (void)onto.put({indent - 2, 0}, glyph(followed(row, depth) ? "line.thin.left.joint" : "line.thin.lower.left"), lines);
        (void)onto.put({indent - 1, 0}, glyph("line.thin.across"), lines);
    }

    (void)onto.put({indent, 0}, mark, look({}, states));

    Canvas rest = onto.within(Area{{indent + 2, 0}, {std::max(onto.size().wide - indent - 2, 0), 1}});

    written(rest, model().entry(row, 0), Edge::start, states);
}

// A click on the mark of a branch at `x` opens or closes it.
void Tree::clicked(std::size_t row, int x) {
    const Branch branch = tree().branch(row);

    if (branch == Branch::leaf || x != 2 * std::max(tree().depth(row), 0)) return;

    tree().open(row, branch == Branch::closed);
}

// The model, which a tree is made with as a tree.
TreeModel& Tree::tree() const { return static_cast<TreeModel&>(model()); }

// Whether a row at `depth` comes after `row` before any row less deep: whether the branch at that
// level goes on below it.
bool Tree::followed(std::size_t row, int depth) const {
    for (std::size_t next = row + 1; next < model().rows(); ++next) {
        const int here = tree().depth(next);

        if (here < depth) return false;

        if (here == depth) return true;
    }

    return false;
}

// A bar for the view `one`, in the class "scroll", with the state `focus` while the view has it:
// the glyph `scroll.track.upright` down its column and the thumb (`scroll.thumb`, the part "thumb")
// where the view stands among its rows. A press above or below the thumb moves the view a window,
// dragging the thumb moves it along, and the wheel moves it as over the view. The bar holds the
// view weakly; a view gone leaves it empty. A view no one holds throws std::invalid_argument.
Scroll::Scroll(Scrollable& one) : view_(one.weak_from_this()) {
    if (view_.expired()) throw std::invalid_argument{"tal::Scroll: the view is not held"};

    classes({"scroll"});
    state(State::focus, one.focused());

    hooks_.add(one.on<Scrolled>([this](Scrolled&) { moved(); }));
    hooks_.add(one.on<Focused>([this](Focused&) { state(State::focus, true); }));
    hooks_.add(one.on<Blurred>([this](Blurred&) { state(State::focus, false); }));

    hooks_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        Scrollable* shown = view();

        if (shown == nullptr) return;

        if (what.press == Press::wheel) {
            if (what.axis != Axis::vertical) return;

            what.take();
            shown->top(stepped(shown->top(), -static_cast<long long>(wheeled) * what.steps));

            return;
        }

        if (what.press == Press::up) {
            grip_.reset();

            return;
        }

        const int y = what.at.y - controlling::boxed(style()).top;

        if (grip_) {
            what.take();
            dragged(y - *grip_);

            return;
        }

        if (what.press != Press::down || what.button != MouseButton::left) return;

        const std::optional<Thumb> thumb = this->thumb(track());

        if (!thumb) return;

        what.take();

        const std::size_t window = std::max<std::size_t>(shown->window(), 1);

        if (y < thumb->from) {
            shown->top(shown->top() > window ? shown->top() - window : 0);
        } else if (y >= thumb->from + thumb->length) {
            shown->top(shown->top() + window);
        } else {
            grip_ = y - thumb->from;
            capture();
        }
    }));
}

// One column wide and as high as the room.
Size Scroll::measure(Limits room) const { return Size{1, room.least.high}; }

// Takes where the thumb stands in the new size.
void Scroll::arrange(Area inside) {
    (void)inside;

    shown_ = thumb(track());
}

// The track down the column and the thumb over it, when the view has more rows than it shows.
void Scroll::paint(Canvas& onto) const {
    const int high = onto.size().high;
    const Look plain = look();
    const std::string_view track = glyph("scroll.track.upright");

    for (int y = 0; y < high; ++y) (void)onto.put({0, y}, track, plain);

    const std::optional<Thumb> made = thumb(high);

    if (!made) return;

    const Look grip = look("thumb");
    const std::string_view mark = glyph("scroll.thumb");

    for (int y = made->from; y < made->from + made->length; ++y) (void)onto.put({0, y}, mark, grip);
}

// The view, while it lives.
Scrollable* Scroll::view() const { return dynamic_cast<Scrollable*>(view_.lock().get()); }

// How many rows the track has: the inside of the bar.
int Scroll::track() const {
    const Sides in = controlling::boxed(style());

    return area().size.high - in.top - in.bottom;
}

// The view moved, or its rows or its window changed: the bar is painted again where the thumb
// moves, and not at all when it stands as it stood, as when a row comes far from the view.
void Scroll::moved() {
    const std::optional<Thumb> now = thumb(track());

    if (now == shown_) return;

    shown_ = now;

    repaint();
}

// Where the thumb stands in a track `high` rows long: as long as the window is to all the rows (one
// row at least), as far down as the view is among the places it can stand. None while the view
// shows all its rows, or is gone.
std::optional<Scroll::Thumb> Scroll::thumb(int high) const {
    const Scrollable* shown = view();

    if (shown == nullptr || high <= 0) return std::nullopt;

    const std::size_t many = shown->extent();
    const std::size_t window = shown->window();

    if (many <= window) return std::nullopt;

    const auto length = static_cast<int>(std::clamp<long long>(static_cast<long long>(high) * static_cast<long long>(window) / static_cast<long long>(many), 1, high));
    const auto most = static_cast<long long>(many - window);
    const auto from = static_cast<int>(static_cast<long long>(high - length) * static_cast<long long>(shown->top()) / most);

    return Thumb{from, length};
}

// The thumb dragged to start at the row `y` of the track: the view moves to the place that stands
// for it, rounded to the nearest.
void Scroll::dragged(int y) {
    Scrollable* shown = view();
    const int high = track();
    const std::optional<Thumb> made = thumb(high);

    if (shown == nullptr || !made) return;

    const auto most = static_cast<long long>(shown->extent() - shown->window());
    const long long room = high - made->length;
    const long long at = std::clamp<long long>(y, 0, room);

    shown->top(static_cast<std::size_t>((at * most + room / 2) / room));
}

}
