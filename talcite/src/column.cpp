#include "talcite/column.hpp"

#include <algorithm>

#include "lineup.hpp"

namespace talcite::ui {

// A column with `gap` empty rows between its children; a negative gap is none.
Column::Column(int gap) : gap_(std::max(gap, 0)) {}

// Sets the gap between children, in rows; a negative value becomes zero.
void Column::gap(int given) {
    gap_ = std::max(given, 0);

    changed();
}

// A column lays out any number of children, one under another.
int Column::capacity() const { return plenty; }

// Where each child stands when every one gets the height it wants, top to bottom; the width is the
// child's wish, by the rules at `lineup::placed`.
std::vector<Area> Column::placed(Size room) const { return lineup::placed(children(), room, gap_, Axis::vertical); }

// The children's heights add up and the width is that of the widest child, clamped to `room`, as
// `lineup::wanted` says.
Size Column::wants(Size room) const { return lineup::wanted(placed(room), room); }

// Lays the children out one under another, each over the full width; the spare height goes to the
// vertical floats among them, by the rule of automatic margins described at `lineup::laid`.
void Column::layout(Size given) { lineup::laid(children(), given, gap_, Axis::vertical); }

// Paints the children one under another, where the layout put them.
void Column::paint(Canvas& onto) const {
    for (const std::shared_ptr<Node>& one : children()) one->render(onto);
}

}
