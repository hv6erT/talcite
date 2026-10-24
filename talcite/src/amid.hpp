#pragma once

#include <algorithm>
#include <memory>

#include "talcite/frame.hpp"
#include "talcite/node.hpp"
#include "talcite/view.hpp"

namespace talcite::ui {

// A gap that disappears together with a neighbor. An empty row between two parts makes sense only
// when both are present: a message without content or without answers has nothing to separate. We
// ask on every frame rather than once at construction, because both the content and the buttons
// may be added later, and then the row has to appear by itself, without rebuilding the column.
class Gap : public Node {
public:
    // Remembers the two neighbors whose height decides whether the gap shows; both must outlive it.
    Gap(const Node& above, const Node& below) : above_(&above), below_(&below) {}

    // A gap holds no children.
    [[nodiscard]] int capacity() const override { return 0; }

    // One row (if the room allows) when both neighbors want some height, otherwise nothing.
    [[nodiscard]] Size wants(Size room) const override {
        if (above_->wants(room).high == 0 || below_->wants(room).high == 0) return {};

        return {0, std::min(1, room.high)};
    }

private:
    const Node* above_;
    const Node* below_;
};

// The title takes room too. A frame computes its width from its content and draws the title in
// whatever is left, so a narrow window with a long title would cut it off. This node asks both the
// content and the title and wants the wider of the two: the title with one cell of padding on
// each side plus the two borders.
class Titled : public Node {
public:
    // Remembers the frame whose title is measured; the frame must outlive the node.
    explicit Titled(const Frame& frame) : frame_(&frame) {}

    // The content's size, widened to fit the title (4 cells more than its text), within the room.
    [[nodiscard]] Size wants(Size room) const override {
        const Size inside = Node::wants(room);
        const int title = frame_->title().empty() ? 0 : measured(frame_->title()).wide + 4;

        return {std::min(std::max(inside.wide, title), room.wide), inside.high};
    }

private:
    const Frame* frame_;
};

}
