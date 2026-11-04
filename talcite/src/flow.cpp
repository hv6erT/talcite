#include "talcite/flow.hpp"

#include <algorithm>

namespace talcite::ui {

// A row that wraps: a child that does not fit in what is left of the line starts the next one, and
// each line stands in the middle. That way a window's row of buttons does not lose the last ones
// when there are more than the screen can hold, in 80 columns and in a language with long labels.
//
// Its own class rather than a switch on `Row`: lines are a different layout, not a variant of one
// line, and a row that is one line has floats to place, which a flow of centered lines does not.
Flow::Flow(int gap) : gap_(std::max(gap, 0)) {}

// Sets the gap between children in a line, in columns; a negative gap counts as zero.
void Flow::gap(int given) {
    gap_ = std::max(given, 0);

    changed();
}

// A flow takes any number of children.
int Flow::capacity() const { return plenty; }

// Splits the children into lines for a room of size `room`. A gap is due only between children that
// occupy something, the rule described at `lineup::placed`. A child wider than the whole room gets
// a line to itself, trimmed.
std::vector<Flow::Line> Flow::laid(Size room) const {
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

// The width of the widest line and the heights of all lines, clamped to `room`.
Size Flow::wants(Size room) const {
    Size made;

    for (const Line& line : laid(room)) {
        made.wide = std::max(made.wide, line.wide);
        made.high += line.high;
    }

    return {std::min(made.wide, room.wide), std::min(made.high, room.high)};
}

// Lays the lines out one under another, each in the middle of the given width.
void Flow::layout(Size given) {
    const std::vector<Line> lines = laid(given);

    // A child that occupies nothing still gets a zero rectangle, so that its area() tells the truth
    // about this frame.
    for (const std::shared_ptr<Node>& one : children()) {
        const bool placed = std::any_of(lines.begin(), lines.end(), [&one](const Line& line) {
            return std::any_of(line.placed.begin(), line.placed.end(), [&one](const auto& each) { return each.first == one.get(); });
        });

        if (!placed) one->arrange({{0, 0}, {0, 0}});
    }

    int y = 0;

    for (const Line& line : lines) {
        int x = std::max((given.wide - line.wide) / 2, 0);

        for (const auto& [one, size] : line.placed) {
            const Area where{{x, y}, {size.wide, line.high}};
            one->arrange(where);

            x += size.wide + gap_;
        }

        y += line.high;
    }
}

// Paints the children where the lines put them.
void Flow::paint(Canvas& onto) const {
    for (const std::shared_ptr<Node>& one : children()) one->render(onto);
}

}
