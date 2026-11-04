#include "talcite/row.hpp"

#include <algorithm>

#include "lineup.hpp"

namespace talcite::ui {

Row::Row(int gap) : gap_(std::max(gap, 0)) {}

// Sets the gap between children, in columns; a negative gap counts as zero.
void Row::gap(int given) {
    gap_ = std::max(given, 0);

    changed();
}

// A row takes any number of children.
int Row::capacity() const { return plenty; }

// Where each child stands when every one gets the width it wants, left to right, in one line; the
// height is the child's wish, by the rules at `lineup::placed`.
std::vector<Area> Row::placed(Size room) const { return lineup::placed(children(), room, gap_, Axis::horizontal); }

// The size the children want side by side, with gaps, clamped to `room`, as `lineup::wanted` says.
Size Row::wants(Size room) const { return lineup::wanted(placed(room), room); }

// Lays the children out left to right, each over the full height; the spare width goes to the
// horizontal floats among them, by the rule of automatic margins described at `lineup::laid`.
void Row::layout(Size given) { lineup::laid(children(), given, gap_, Axis::horizontal); }

// Paints the children side by side, where the layout put them.
void Row::paint(Canvas& onto) const {
    for (const std::shared_ptr<Node>& one : children()) one->render(onto);
}

}
