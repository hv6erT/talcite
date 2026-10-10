#include "talcite/legend.hpp"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "talcite/legenditem.hpp"

#include "slot.hpp"

namespace talcite::ui {

// The legend takes the whole screen and places the content row above itself, just like the menu
// bar and for the same reason: a child may overlap its siblings but cannot reach outside its
// parent.
//
// The legend is the outer window and the menu bar sits inside it, not the other way round. The
// menu then gets the rows from zero to the next-to-last, and an open list cannot paint over the
// key bar, exactly as in Midnight Commander. With the opposite nesting a tall menu would run over
// the legend.
Legend::Legend() : body_(std::make_shared<Row>()) {
    children().add(body_);
}

// The content underneath, or `nullptr` when there is none. The slot is described in `slot.hpp`.
Node* Legend::content() const { return held(*body_); }

// Puts the given node in the content slot, replacing what was there.
void Legend::content(std::shared_ptr<Node> given) { hold(*body_, std::move(given)); }

// The legend wants all the room it is offered.
Size Legend::wants(Size room) const { return room; }

// From outside it accepts exactly one child, the content, and every registered field opens
// exactly one more place, which the field itself takes right away.
int Legend::capacity() const { return 1 + static_cast<int>(items_.size()); }

// Creates a field for the given key and label and returns it. The bar makes the field, not the
// caller. It goes directly under the bar, not into the content row: the legend paints it itself,
// in the last row, at widths it divides itself. It goes into the vector before being attached,
// because the vector is what opens a place in `capacity()`.
std::shared_ptr<LegendItem> Legend::add(Key key, std::string text) {
    auto one = std::make_shared<LegendItem>(key, std::move(text));

    items_.push_back(one);

    try {
        children().add(one);
    } catch (...) {
        items_.pop_back();

        throw;
    }

    return one;
}

// Removes all fields. Switching context (panel, viewer, editor) means emptying the bar and adding
// new fields. They are removed both from the vector and from the tree, or `capacity()` would
// count places for fields that no longer exist.
void Legend::clear() {
    const std::vector<std::shared_ptr<LegendItem>> these = items_;

    items_.clear();

    for (const std::shared_ptr<LegendItem>& one : these) (void)children().remove(one.get());
}

// The width of the given field in a row of the given width. The remainder of the division goes to
// the fields from the left, so the bar fills the row exactly and no stub is left at the right
// edge.
int Legend::width(int which, int wide) const {
    const int count = static_cast<int>(items_.size());

    if (count <= 0 || wide <= 0) return 0;

    return wide / count + (which < wide % count ? 1 : 0);
}

// The column where the given field starts. It is computed from the same widths that paint uses;
// otherwise mouse hits would drift from the picture by the remainder of the division.
int Legend::along(int which, int wide) const {
    int made = 0;

    for (int at = 0; at < which; ++at) made += width(at, wide);

    return made;
}

// Paints the content above and the fields in the last row.
View Legend::paint(Size given, Area seen) {
    View made{seen.size};

    // Transparent where nothing is drawn, the rule described at `Node::paint`.

    made.clear({{0, 0}, seen.size});

    // An empty bar does not take a row. A legend without fields is a legend that is not there,
    // not an empty strip at the bottom of the screen.
    const int high = items_.empty() ? given.high : std::max(given.high - 1, 0);

    const Area above{{0, 0}, {given.wide, high}};
    const Area part = Area::crossed(seen, above);

    made.draw(body_->render(above, part - above.at), part.at - seen.at);

    if (items_.empty() || given.high <= 0) return made;

    const int y = given.high - 1;

    for (int at = 0; at < static_cast<int>(items_.size()); ++at) {
        const Area where{{along(at, given.wide), y}, {width(at, given.wide), 1}};
        const Area cut = Area::crossed(seen, where);

        made.draw(items_[static_cast<std::size_t>(at)]->render(where, cut - where.at),
                    cut.at - seen.at);
    }

    return made;
}

// Offers a key to the fields. The key arrives here by bubbling up from the focus, because the
// legend is an ancestor of all the content. This is the same trick by which the menu bar gets F9,
// so nothing in the core is needed for it.
//
// Nothing is filtered here. Modifiers are part of the key, so `Keys::contains` does it: a field
// registered for F5 does not answer to Ctrl+F5, and a field registered for Ctrl+F5 answers only to
// that. Shift is handled by the same rule, because terminals report it inconsistently with
// function keys.
//
// The bar hands out the hotkey; it does not decide for the fields. It only passes the event to its
// fields in turn, and the matching, the skipping of disabled fields and the action itself belong
// to `KeyedNode` and to `LegendItem::activate()`.
//
// The key is offered only to `items_`, not through `broadcast(what)`, because that would go into
// the content row first. The key bar stands beside the layout and has to get the key instead of
// the content, not before it.
//
// The loop runs over a snapshot, not over the live list: a field's handler may rebuild the bar in
// response, and then the vector moves under the loop's feet.
bool Legend::handle(KeyEvent& what) {
    if (what.key().empty()) return false;

    const std::vector<std::shared_ptr<LegendItem>> these = items_;

    for (const std::shared_ptr<LegendItem>& one : these)
        if (offer(what, *one)) return true;

    return false;
}

}
