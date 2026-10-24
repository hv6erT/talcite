#include "talcite/row.hpp"

#include <algorithm>
#include <utility>

namespace talcite::ui {

Row::Row(int gap) : gap_(std::max(gap, 0)) {}

Row::Row(RowShape shape) : gap_(std::max(shape.gap, 0)), wrapping_(shape.wrapping), spread_(shape.spread) {}

// Sets the gap between children, in columns; a negative gap counts as zero.
void Row::gap(int given) { gap_ = std::max(given, 0); }

// A row takes any number of children.
int Row::capacity() const { return plenty; }

// Splits the children into lines for a wrapping row of size `room`. A gap is due only between
// children that occupy something. A child of zero width (an empty label, a disabled button, a node
// without content) opens no gap, because it would leave a hole where nothing is visible. That is
// why we remember not "which child in turn this is" but "whether something already stands here".
// This is a matter of layout, not of content: otherwise every window would have to separate its
// buttons with empty nodes and fix them at every removal.
//
// A row that wraps (`wrapping`): a child that does not fit in what is left of the line starts the
// next one; each line is centered. That way a window's row of buttons does not lose the last ones
// when there are more than the screen can hold, in 80 columns and in a language with long labels.
// A child wider than the whole row gets a line to itself, trimmed.
std::vector<Row::Line> Row::laid(Size room) const {
    std::vector<Line> made(1);

    for (const std::shared_ptr<Node>& one : children()) {
        const Size wants = one->wants({room.wide, room.high});
        const int wide = std::min(wants.wide, room.wide);

        if (wide == 0) continue;

        if (!made.back().placed.empty() && made.back().wide + gap_ + wide > room.wide) made.emplace_back();

        Line& line = made.back();

        line.wide += (line.placed.empty() ? 0 : gap_) + wide;
        line.high = std::max(line.high, wants.high);
        line.placed.emplace_back(one.get(), Size{wide, wants.high});
    }

    return made;
}

// The size the children want side by side, with gaps, or line under line when the row wraps,
// clamped to `room`.
Size Row::wants(Size room) const {
    if (wrapping_) {
        Size made;

        for (const Line& line : laid(room)) {
            made.wide = std::max(made.wide, line.wide);
            made.high += line.high;
        }

        return {std::min(made.wide, room.wide), std::min(made.high, room.high)};
    }

    Size made;

    for (const std::shared_ptr<Node>& one : children()) {
        const int apart = made.wide > 0 ? gap_ : 0;
        const Size wants =
            one->wants({std::max(room.wide - made.wide - apart, 0), room.high});

        if (wants.wide == 0) continue;

        made.wide += apart + wants.wide;
        made.high = std::max(made.high, wants.high);
    }

    return {std::min(made.wide, room.wide), std::min(made.high, room.high)};
}

// Lays the children out left to right, or in centered lines when the row wraps, and draws them.
//
// A spread row (`spread`) that gets more room than it wants gives the rest to the place after its
// first child that occupies something: that child stays at the left edge and the others stand at
// the right one. A window's last row is made so, with what the window says on the left and its
// buttons on the right. Without room to spare it is an ordinary row; a wrapping row centers its
// lines and does not spread.
View Row::paint(Size given, Area seen) {
    View made{seen.size};

    // Transparent where it draws nothing, the rule described at `Node::paint`.

    made.clear({{0, 0}, seen.size});

    if (wrapping_) {
        const std::vector<Line> lines = laid(given);

        // A child that occupies nothing still gets a zero rectangle, as in a row without wrapping.
        for (const std::shared_ptr<Node>& one : children()) {
            const bool placed = std::any_of(lines.begin(), lines.end(), [&one](const Line& line) {
                return std::any_of(line.placed.begin(), line.placed.end(), [&one](const auto& each) { return each.first == one.get(); });
            });

            if (!placed) static_cast<void>(one->render({{0, 0}, {0, 0}}, {{0, 0}, {0, 0}}));
        }

        int y = 0;

        for (const Line& line : lines) {
            int x = std::max((given.wide - line.wide) / 2, 0);

            for (const auto& [one, size] : line.placed) {
                const Area where{{x, y}, {size.wide, line.high}};
                const Area part = Area::crossed(seen, where);

                made.draw(one->render(where, part - where.at), part.at - seen.at);

                x += size.wide + gap_;
            }

            y += line.high;
        }

        return made;
    }

    int x = 0;
    int spare = spread_ ? std::max(given.wide - wants(given).wide, 0) : 0;

    for (const std::shared_ptr<Node>& one : children()) {
        const int apart = x > 0 ? gap_ : 0;
        const int left = std::max(given.wide - x - apart, 0);
        const Size wants = one->wants({left, given.high});
        const Size gets{std::min(wants.wide, left), given.high};

        // Nobody drops out of the loop, even when the room has run out: a node that is not
        // visible gets a rectangle of zero width, so that its area() tells the truth about this
        // frame. But it opens no gap before itself.
        const int from = gets.wide > 0 ? x + apart : x;

        const Area where{{from, 0}, gets};
        const Area part = Area::crossed(seen, where);

        made.draw(one->render(where, part - where.at), part.at - seen.at);

        x = from + gets.wide;

        if (gets.wide > 0) x += std::exchange(spare, 0);
    }

    return made;
}

}
