#include "tal/layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>

#include "dividing.hpp"

namespace tal {

namespace dividing {

// Splits `room` cells among stretches that want `wanted` cells and share as `shares` say: those
// with weight 0 get what they want, in order, while room lasts; then those with a weight get their
// `least`, in order, while room lasts; what is left goes to them in proportion to their weights,
// the cells the division leaves over one each to the first of them.
std::vector<int> divided(const std::vector<int>& wanted, const std::vector<Share>& shares, int room) {
    std::vector<int> made(wanted.size(), 0);
    int left = std::max(room, 0);
    int weights = 0;

    for (std::size_t at = 0; at < wanted.size(); ++at) {
        if (shares[at].weight != 0) continue;

        made[at] = std::min(std::max(wanted[at], 0), left);
        left -= made[at];
    }

    for (std::size_t at = 0; at < wanted.size(); ++at) {
        if (shares[at].weight == 0) continue;

        made[at] = std::min(shares[at].least, left);
        left -= made[at];
        weights += shares[at].weight;
    }

    if (weights == 0 || left == 0) return made;

    const int spread = left;

    for (std::size_t at = 0; at < wanted.size(); ++at) {
        if (shares[at].weight == 0) continue;

        const int part = static_cast<int>(static_cast<long long>(spread) * shares[at].weight / weights);

        made[at] += part;
        left -= part;
    }

    for (std::size_t at = 0; at < wanted.size() && left > 0; ++at) {
        if (shares[at].weight == 0) continue;

        ++made[at];
        --left;
    }

    return made;
}

}

using dividing::divided;

namespace {

// The size along an axis: the width across, the height down.
int along(Size one, Axis axis) { return axis == Axis::horizontal ? one.wide : one.high; }

// The size across an axis.
int across(Size one, Axis axis) { return axis == Axis::horizontal ? one.high : one.wide; }

// The size `much` along `axis` and `other` across it.
Size sized(int much, int other, Axis axis) { return axis == Axis::horizontal ? Size{much, other} : Size{other, much}; }

// The empty cells between `count` children `gap` apart; a negative gap counts as none.
int gaps(std::size_t count, int gap) { return count < 2 ? 0 : static_cast<int>(count - 1) * std::max(gap, 0); }

// What a line of children along `axis` wants in `room`: each child its own size, one with a weight
// at least its `least`, with the gaps between them; across, the largest child. A weighted child
// wants its content too, so that a line whose size its content decides (a window in the middle of
// the screen) shows it; where the line is given more, the weights share the rest.
Size lined(const Node& owner, Limits room, Axis axis, int gap) {
    std::size_t count = 0;
    int length = 0;
    int thickness = 0;

    for (Node& one : owner.children()) {
        const Size wanted = one.measured(Limits{{0, 0}, room.most});

        length += std::max(along(wanted, axis), one.share().least);
        thickness = std::max(thickness, across(wanted, axis));
        ++count;
    }

    return sized(length + gaps(count, gap), thickness, axis);
}

// Places the children of a line along `axis` in `inside`: one after another with `gap` cells
// between them, each as long as `divided` gives it and as thick as the line.
void laid(Node& owner, Area inside, Axis axis, int gap) {
    std::vector<Node*> children;
    std::vector<int> wanted;
    std::vector<Share> shares;

    for (Node& one : owner.children()) {
        children.push_back(&one);
        wanted.push_back(along(one.measured(Limits{{0, 0}, inside.size}), axis));
        shares.push_back(one.share());
    }

    const int space = std::max(gap, 0);
    const std::vector<int> lengths = divided(wanted, shares, along(inside.size, axis) - gaps(children.size(), gap));
    const int thick = across(inside.size, axis);
    int at = 0;

    for (std::size_t which = 0; which < children.size(); ++which) {
        const Spot start = axis == Axis::horizontal ? Spot{inside.at.x + at, inside.at.y} : Spot{inside.at.x, inside.at.y + at};

        children[which]->placed(Area{start, sized(lengths[which], thick, axis)});
        at += lengths[which] + space;
    }
}

}

// A row with `gap` empty columns between its children; a negative gap is none.
Row::Row(int gap) : gap_(gap) {}

// The children side by side, each as wide as it wants, a weighted one at least its least.
Size Row::measure(Limits room) const { return lined(*this, room, Axis::horizontal, gap_); }

// The children left to right, each over the full height; the widths as `Share` says.
void Row::arrange(Area inside) { laid(*this, inside, Axis::horizontal, gap_); }

// A column with `gap` empty rows between its children; a negative gap is none.
Column::Column(int gap) : gap_(gap) {}

// The children one under another, each as high as it wants, a weighted one at least its least.
Size Column::measure(Limits room) const { return lined(*this, room, Axis::vertical, gap_); }

// The children top to bottom, each over the full width; the heights as `Share` says.
void Column::arrange(Area inside) { laid(*this, inside, Axis::vertical, gap_); }

// A grid of the given columns and rows with `gap` empty cells between them both ways.
Grid::Grid(std::vector<Share> columns, std::vector<Share> rows, int gap)
    : columns_(std::move(columns)), rows_(std::move(rows)), gap_(gap) {
    for (Share& one : columns_) one = Share{std::max(one.weight, 0), std::max(one.least, 0)};
    for (Share& one : rows_) one = Share{std::max(one.weight, 0), std::max(one.least, 0)};
}

// Puts `child`, a child of the grid, in the cell `cell` (column, row) over `span` cells. A node
// that is not a child throws std::logic_error; a span below one cell or reaching past the grid
// throws std::out_of_range.
void Grid::at(Node& child, Spot cell, Size span) {
    if (child.parent() != this) throw std::logic_error{"tal::Grid::at: the node is not a child of the grid"};

    if (span.wide < 1 || span.high < 1 || cell.x < 0 || cell.y < 0 || cell.x + span.wide > static_cast<int>(columns_.size()) ||
        cell.y + span.high > static_cast<int>(rows_.size()))
        throw std::out_of_range{"tal::Grid::at: the cells lie outside the grid"};

    std::erase_if(placed_, [&child](const Placement& one) {
        const std::shared_ptr<Node> held = one.node.lock();

        return held == nullptr || held.get() == &child;
    });

    placed_.push_back(Placement{child.weak_from_this(), cell, span});

    relayout();
}

// Every child with its cells: those put somewhere stand there, the others take the free cells one
// each in reading order. A child for which no cell is left, or one put where another child already
// stands, gets none.
std::vector<std::pair<Node*, Area>> Grid::cells() const {
    const int wide = static_cast<int>(columns_.size());
    const int high = static_cast<int>(rows_.size());
    std::vector<bool> taken(static_cast<std::size_t>(wide) * static_cast<std::size_t>(high), false);
    std::vector<std::pair<Node*, Area>> made;
    std::vector<Node*> loose;

    const auto free = [&](Area part) {
        for (int y = part.at.y; y < part.at.y + part.size.high; ++y)
            for (int x = part.at.x; x < part.at.x + part.size.wide; ++x)
                if (taken[static_cast<std::size_t>(y * wide + x)]) return false;

        return true;
    };

    const auto take = [&](Area part) {
        for (int y = part.at.y; y < part.at.y + part.size.high; ++y)
            for (int x = part.at.x; x < part.at.x + part.size.wide; ++x) taken[static_cast<std::size_t>(y * wide + x)] = true;
    };

    for (Node& one : children()) {
        const auto found = std::find_if(placed_.begin(), placed_.end(), [&one](const Placement& held) {
            const std::shared_ptr<Node> node = held.node.lock();

            return node != nullptr && node.get() == &one;
        });

        if (found == placed_.end()) {
            loose.push_back(&one);

            continue;
        }

        const Area part{found->cell, found->span};

        if (!free(part)) continue;

        take(part);
        made.emplace_back(&one, part);
    }

    int next = 0;

    for (Node* one : loose) {
        while (next < wide * high && taken[static_cast<std::size_t>(next)]) ++next;

        if (next >= wide * high) break;

        const Area part{{next % wide, next / wide}, {1, 1}};

        take(part);
        made.emplace_back(one, part);
    }

    return made;
}

// The tracks side by side, each as large as its largest child that stands in it alone, and a
// weighted one at least its least; the gaps between them.
Size Grid::measure(Limits room) const {
    std::vector<int> widths(columns_.size(), 0);
    std::vector<int> heights(rows_.size(), 0);

    for (const auto& [one, part] : cells()) {
        const Size wanted = one->measured(Limits{{0, 0}, room.most});

        if (part.size.wide == 1) widths[static_cast<std::size_t>(part.at.x)] = std::max(widths[static_cast<std::size_t>(part.at.x)], wanted.wide);

        if (part.size.high == 1) heights[static_cast<std::size_t>(part.at.y)] = std::max(heights[static_cast<std::size_t>(part.at.y)], wanted.high);
    }

    int wide = gaps(columns_.size(), gap_);
    int high = gaps(rows_.size(), gap_);

    for (std::size_t at = 0; at < columns_.size(); ++at) wide += std::max(widths[at], columns_[at].least);

    for (std::size_t at = 0; at < rows_.size(); ++at) high += std::max(heights[at], rows_[at].least);

    return Size{wide, high};
}

// Divides the inside among the tracks as a row divides its width, and places each child over its
// cells and the gaps between them; a child without a cell is placed without room.
void Grid::arrange(Area inside) {
    const std::vector<std::pair<Node*, Area>> laid_out = cells();
    std::vector<int> widths(columns_.size(), 0);
    std::vector<int> heights(rows_.size(), 0);

    for (const auto& [one, part] : laid_out) {
        const Size wanted = one->measured(Limits{{0, 0}, inside.size});

        if (part.size.wide == 1) widths[static_cast<std::size_t>(part.at.x)] = std::max(widths[static_cast<std::size_t>(part.at.x)], wanted.wide);

        if (part.size.high == 1) heights[static_cast<std::size_t>(part.at.y)] = std::max(heights[static_cast<std::size_t>(part.at.y)], wanted.high);
    }

    const std::vector<int> columns = divided(widths, columns_, inside.size.wide - gaps(columns_.size(), gap_));
    const std::vector<int> lines = divided(heights, rows_, inside.size.high - gaps(rows_.size(), gap_));
    const int space = std::max(gap_, 0);

    // The start of track `at` among `sizes`, from `origin`.
    const auto start = [space](const std::vector<int>& sizes, int at, int origin) {
        for (int one = 0; one < at; ++one) origin += sizes[static_cast<std::size_t>(one)] + space;

        return origin;
    };

    // The length of `count` tracks from `at` among `sizes`, with the gaps between them.
    const auto length = [space](const std::vector<int>& sizes, int at, int count) {
        int made = space * (count - 1);

        for (int one = at; one < at + count; ++one) made += sizes[static_cast<std::size_t>(one)];

        return made;
    };

    std::vector<Node*> done;

    for (const auto& [one, part] : laid_out) {
        one->placed(Area{{start(columns, part.at.x, inside.at.x), start(lines, part.at.y, inside.at.y)},
                         {length(columns, part.at.x, part.size.wide), length(lines, part.at.y, part.size.high)}});
        done.push_back(one);
    }

    for (Node& one : children())
        if (std::find(done.begin(), done.end(), &one) == done.end()) one.placed(Area{inside.at, {0, 0}});
}

// A holder that stands its children at the start, in the middle or at the end along `axis`.
Align::Align(Axis axis, Edge where) : axis_(axis), where_(where) {}

// As large as the largest child.
Size Align::measure(Limits room) const {
    Size made = room.least;

    for (Node& one : children()) {
        const Size wanted = one.measured(Limits{{0, 0}, room.most});

        made = Size{std::max(made.wide, wanted.wide), std::max(made.high, wanted.high)};
    }

    return made;
}

// Each child as long as it wants along the axis, standing where asked, and over the full extent
// across it.
void Align::arrange(Area inside) {
    for (Node& one : children()) {
        const int room = along(inside.size, axis_);
        const int length = std::min(along(one.measured(Limits{{0, 0}, inside.size}), axis_), room);
        const int offset = where_ == Edge::start ? 0 : where_ == Edge::middle ? (room - length) / 2 : room - length;
        const Spot at = axis_ == Axis::horizontal ? Spot{inside.at.x + offset, inside.at.y} : Spot{inside.at.x, inside.at.y + offset};

        one.placed(Area{at, sized(length, across(inside.size, axis_), axis_)});
    }
}

// A split of the room along `along` among its children, in the class "split": each child gets a
// part of the room in proportion to its part (`part`, the same for all until set), none less than
// `least` cells while the room holds that much, with `gap` cells between them, where the split
// draws a line. A border between two children is dragged with the left button from its gap or the
// cell on either side of it, where nothing under the pointer takes the press; while it is dragged
// the split has the state `drag`. Every move of a border tells `Resized`.
Split::Split(Axis along, int gap) : axis(*this, Effect::layout, along), gap_(std::max(gap, 0)) {
    classes({"split"});

    hooks_.add(otherwise<MouseEvent>([this](MouseEvent& what) {
        if (what.press == Press::down && dragging_) {
            dragging_.reset();
            state(State::drag, false);
        }

        if (what.press == Press::up) {
            if (!dragging_) return;

            what.take();
            dragging_.reset();
            state(State::drag, false);

            return;
        }

        const std::optional<Spot> here = local(what.screen);

        if (!here) return;

        const int at = along_of(*here);

        if (dragging_) {
            if (what.press != Press::move) return;

            what.take();
            moved(*dragging_, at - grip_ - border_spot(*dragging_));

            return;
        }

        if (what.press != Press::down || what.button != MouseButton::left) return;

        const std::optional<std::size_t> border = border_at(at);

        if (!border) return;

        what.take();
        dragging_ = border;
        grip_ = at - border_spot(*border);
        state(State::drag, true);
        capture();
    }));
}

// Sets the part of `child` to `much`: the children share the room in proportion to their parts. A
// node that is not a child throws std::logic_error; a part that is not a positive number throws
// std::invalid_argument.
void Split::part(Node& child, double much) {
    if (child.parent() != this) throw std::logic_error{"tal::Split::part: the node is not a child of the split"};

    if (!std::isfinite(much) || much <= 0) throw std::invalid_argument{"tal::Split::part: a part is a positive number"};

    std::erase_if(parts_, [&child](const Part& one) {
        const std::shared_ptr<Node> held = one.node.lock();

        return held == nullptr || held.get() == &child;
    });

    parts_.push_back(Part{child.weak_from_this(), much});

    relayout();
}

// The part of `child`: as set, or, for a child without one, the mean of the parts set (one when
// none is). A node that is not a child throws std::logic_error.
double Split::part(const Node& child) const {
    if (child.parent() != this) throw std::logic_error{"tal::Split::part: the node is not a child of the split"};

    std::size_t which = 0;

    for (const Node& one : children()) {
        if (&one == &child) break;

        ++which;
    }

    return parts()[which];
}

// Moves the border after the child `border` by `by` cells (forward along the axis when positive):
// the child before it grows by as much as the one after it shrinks, neither below `least`. From
// then on every child's part is the length it has, so the room is shared as it now stands. A
// border that does not stand (past the last child, or before the split is first arranged) and a
// move that changes nothing do nothing; any other tells `Resized`.
void Split::moved(std::size_t border, int by) {
    if (border + 1 >= lengths_.size() || by == 0) return;

    const int fewest = std::max(*least, 0);
    const int first = lengths_[border];
    const int both = first + lengths_[border + 1];

    if (both < 2 * fewest) return;

    const int now = std::clamp(first + by, fewest, both - fewest);

    if (now == first) return;

    lengths_[border] = now;
    lengths_[border + 1] = both - now;

    std::vector<Part> made;
    std::size_t which = 0;

    for (Node& one : children()) {
        if (which < lengths_.size()) made.push_back(Part{one.weak_from_this(), static_cast<double>(std::max(lengths_[which], 1))});

        ++which;
    }

    parts_ = std::move(made);

    relayout();

    Resized told{border};

    notified(told);
}

// How many borders stand between the children: one fewer than there are children.
std::size_t Split::borders() const {
    const std::size_t count = children().size();

    return count == 0 ? 0 : count - 1;
}

// Along the axis, every child's `least` with the gaps between them; across it, the largest child.
Size Split::measure(Limits room) const {
    const std::size_t count = children().size();
    int thickness = 0;

    for (Node& one : children()) thickness = std::max(thickness, across(one.measured(Limits{{0, 0}, room.most}), *axis));

    const int length = static_cast<int>(count) * std::max(*least, 0) + gaps(count, gap_);

    return sized(std::max(length, along(room.least, *axis)), std::max(thickness, across(room.least, *axis)), *axis);
}

// The children one after another along the axis with the gaps between them, over the full extent
// across it. The room left after the gaps is shared in proportion to the parts; a child whose share
// falls below `least` gets `least` and the rest is shared again among the others; the cells the
// rounding leaves over go one each to the children with the largest remainders, the first of equal
// ones first. Where the room is less than `least` for each child, it is shared evenly.
void Split::arrange(Area inside) {
    inside_ = inside;

    std::vector<Node*> nodes;

    for (Node& one : children()) nodes.push_back(&one);

    const std::size_t count = nodes.size();
    const int room = std::max(along(inside.size, *axis) - gaps(count, gap_), 0);
    const int fewest = std::max(*least, 0);
    const bool enough = room >= static_cast<int>(count) * fewest;
    std::vector<double> weights = enough ? parts() : std::vector<double>(count, 1.0);
    std::vector<int> made(count, 0);
    std::vector<bool> fixed(count, false);
    std::vector<double> ideal(count, 0);

    while (count != 0) {
        int rest = room;
        double sum = 0;

        for (std::size_t one = 0; one < count; ++one) {
            if (fixed[one]) {
                rest -= fewest;
            } else {
                sum += weights[one];
            }
        }

        bool again = false;

        for (std::size_t one = 0; one < count; ++one) {
            if (fixed[one]) continue;

            ideal[one] = sum > 0 ? static_cast<double>(rest) * weights[one] / sum : 0;

            if (enough && ideal[one] < fewest) {
                fixed[one] = true;
                again = true;
            }
        }

        if (!again) break;
    }

    int left = room;

    for (std::size_t one = 0; one < count; ++one) {
        made[one] = fixed[one] ? fewest : static_cast<int>(std::floor(ideal[one]));
        left -= made[one];
    }

    std::vector<std::size_t> order;

    for (std::size_t one = 0; one < count; ++one)
        if (!fixed[one]) order.push_back(one);

    std::stable_sort(order.begin(), order.end(), [&](std::size_t one, std::size_t two) { return ideal[one] - std::floor(ideal[one]) > ideal[two] - std::floor(ideal[two]); });

    for (std::size_t one = 0; left > 0 && !order.empty(); one = (one + 1) % order.size()) {
        ++made[order[one]];
        --left;
    }

    lengths_ = made;

    const int thick = across(inside.size, *axis);
    int at = 0;

    for (std::size_t one = 0; one < count; ++one) {
        const Spot start = *axis == Axis::horizontal ? Spot{inside.at.x + at, inside.at.y} : Spot{inside.at.x, inside.at.y + at};

        nodes[one]->placed(Area{start, sized(made[one], thick, *axis)});
        at += made[one] + gap_;
    }
}

// The line of the sheet's thin glyphs down each gap (across it for a split down the screen).
void Split::paint(Canvas& onto) const {
    if (gap_ == 0) return;

    const std::string_view line = glyph(*axis == Axis::horizontal ? "line.thin.upright" : "line.thin.across");
    const Look plain = look();
    const int thick = across(inside_.size, *axis);

    for (std::size_t border = 0; border + 1 < lengths_.size(); ++border) {
        const int from = border_spot(border);

        for (int step = 0; step < gap_; ++step)
            for (int side = 0; side < thick; ++side) {
                const Spot cell = *axis == Axis::horizontal ? Spot{from + step, inside_.at.y + side} : Spot{inside_.at.x + side, from + step};

                (void)onto.put(cell, line, plain);
            }
    }
}

// Every child's part in order: as set, or the mean of those set (one when none is).
std::vector<double> Split::parts() const {
    std::vector<double> made;
    std::vector<bool> known;
    double sum = 0;
    std::size_t set = 0;

    for (const Node& one : children()) {
        const auto found = std::find_if(parts_.begin(), parts_.end(), [&one](const Part& held) {
            const std::shared_ptr<Node> node = held.node.lock();

            return node != nullptr && node.get() == &one;
        });

        made.push_back(found == parts_.end() ? 0 : found->much);
        known.push_back(found != parts_.end());

        if (found != parts_.end()) {
            sum += found->much;
            ++set;
        }
    }

    const double usual = set == 0 ? 1.0 : sum / static_cast<double>(set);

    for (std::size_t one = 0; one < made.size(); ++one)
        if (!known[one]) made[one] = usual;

    return made;
}

// Where along the axis the border after the child `border` stands: the first cell past that child,
// counted from the split's own corner like the spots of its children.
int Split::border_spot(std::size_t border) const {
    int at = along_of(inside_.at);

    for (std::size_t one = 0; one <= border && one < lengths_.size(); ++one) at += lengths_[one] + (one < border ? gap_ : 0);

    return at;
}

// The coordinate of `one` along the axis.
int Split::along_of(Spot one) const { return *axis == Axis::horizontal ? one.x : one.y; }

// The border whose gap, or the last cell of the child before the gap, holds the cell `at` along the
// axis; none for any other cell. The first cell of the child after the gap is that child's: it is
// where a frame's top edge, which may hold names to click, lies in a split down the screen.
std::optional<std::size_t> Split::border_at(int at) const {
    for (std::size_t border = 0; border + 1 < lengths_.size(); ++border) {
        const int from = border_spot(border);

        if (at >= from - 1 && at < from + gap_) return border;
    }

    return std::nullopt;
}

}
