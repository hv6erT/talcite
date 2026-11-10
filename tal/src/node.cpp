#include "tal/node.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include "tal/document.hpp"

namespace tal {

namespace {

// `one` plus `more`, never past `endless`: a size that has no bound stays without one.
int widened(int one, int more) {
    if (one == endless) return endless;

    const std::int64_t made = static_cast<std::int64_t>(one) + more;

    return made >= endless ? endless : static_cast<int>(std::max<std::int64_t>(0, made));
}

// `one` less `less`, never below zero; `endless` stays `endless`.
int narrowed(int one, int less) {
    if (one == endless) return endless;

    return std::max(0, one - less);
}

// `one` between `least` and `most`; when they cross, `least` wins.
int bounded(int one, int least, int most) { return std::max(least, std::min(one, most)); }

}

// Adds `one` as the last child. A node that has a parent already, the owner itself or one of its
// ancestors, or a document throws std::logic_error: a node stands in one place of one tree.
Node& Children::add(std::shared_ptr<Node> one) { return insert(held_.size(), std::move(one)); }

// Puts `one` among the children before the one at `at` (at the end for `size()`); the rest as at
// `add`, and a place past the end throws std::out_of_range.
Node& Children::insert(std::size_t at, std::shared_ptr<Node> one) {
    if (one == nullptr) throw std::logic_error{"tal::Children: no node to add"};

    if (one->parent_ != nullptr) throw std::logic_error{"tal::Children: the node has a parent already"};

    if (dynamic_cast<Document*>(one.get()) != nullptr) throw std::logic_error{"tal::Children: a document is a root"};

    for (const Node* above = &owner_; above != nullptr; above = above->parent_)
        if (above == one.get()) throw std::logic_error{"tal::Children: a node cannot hold its own ancestor"};

    if (at > held_.size()) throw std::out_of_range{"tal::Children: no such place"};

    owner_.changing();

    Node& made = *one;

    held_.insert(held_.begin() + static_cast<std::ptrdiff_t>(at), std::move(one));
    made.attached(&owner_);
    owner_.relayout();

    return made;
}

// Takes the child `one` away and gives it back, without a parent; its place is repainted. A node
// that is not a child throws std::logic_error.
std::shared_ptr<Node> Children::remove(Node& one) {
    const auto at = std::find_if(held_.begin(), held_.end(), [&one](const std::shared_ptr<Node>& held) { return held.get() == &one; });

    if (at == held_.end()) throw std::logic_error{"tal::Children: the node is not a child"};

    owner_.changing();
    one.repaint();

    std::shared_ptr<Node> made = std::move(*at);

    held_.erase(at);
    made->detached();
    owner_.relayout();

    return made;
}

// Takes every child away, as `remove` does one by one.
void Children::clear() {
    owner_.changing();

    for (const std::shared_ptr<Node>& one : held_) one->repaint();

    std::vector<std::shared_ptr<Node>> gone = std::move(held_);

    held_.clear();

    for (const std::shared_ptr<Node>& one : gone) one->detached();

    owner_.relayout();
}

// A node outside any tree, without classes, styled plainly until a document styles it.
Node::Node() = default;

// The children that live on elsewhere lose their parent and document.
Node::~Node() {
    for (const std::shared_ptr<Node>& one : children_.held_) one->detached();
}

// Names the node, for `find`.
void Node::id(std::string name) { id_ = std::move(name); }

// Sets the classes; the node and everything under it is styled again at the next frame.
void Node::classes(std::vector<std::string> names) {
    if (names == classes_) return;

    changing();

    classes_ = std::move(names);

    restyle();
}

// Adds the class `one`, unless the node has it.
void Node::add_class(std::string_view one) {
    if (has_class(one)) return;

    changing();

    classes_.emplace_back(one);

    restyle();
}

// Drops the class `one`, if the node has it.
void Node::drop_class(std::string_view one) {
    const auto at = std::find(classes_.begin(), classes_.end(), one);

    if (at == classes_.end()) return;

    changing();

    classes_.erase(at);

    restyle();
}

// Whether the node has the class `one`.
bool Node::has_class(std::string_view one) const {
    return std::find(classes_.begin(), classes_.end(), one) != classes_.end();
}

// The size of the node with its margin in `room`: the box (margin, border, padding) around what
// `measure` says the content needs, bounded by `room`. Remembered for the same room until
// `relayout`.
Size Node::measured(Limits room) {
    if (sized_ && !sizing_ && sized_->first == room) return sized_->second;

    const Sides all = box();
    const int across = all.left + all.right;
    const int down = all.top + all.bottom;
    const Limits inner{{narrowed(room.least.wide, across), narrowed(room.least.high, down)},
                       {narrowed(room.most.wide, across), narrowed(room.most.high, down)}};
    const Size content = measure(inner);

    if (document_ != nullptr) ++document_->tally_.measured;

    const Size made{bounded(widened(content.wide, across), room.least.wide, room.most.wide),
                    bounded(widened(content.high, down), room.least.high, room.most.high)};

    sized_ = std::pair{room, made};
    sizing_ = false;

    return made;
}

// Places the node in `outer` (with its margin, in the parent's coordinates) and arranges what is
// inside, unless it stands where it stood and nothing under it waits. A node that moves or changes
// size is repainted where it was and where it is. In a debug build a node arranged twice in one
// frame throws std::logic_error.
void Node::placed(Area outer) {
    const Area border = outer.inner(style_.margin);

    if (placed_ && border == area_ && !arranging_) return;

#ifndef NDEBUG
    if (document_ != nullptr && document_->frames_ != 0 && arranged_in_ == document_->frames_)
        throw std::logic_error{"tal::Node: arranged twice in one frame"};
#endif

    const bool moved = !placed_ || border != area_;

    if (moved) repaint();

    area_ = border;
    placed_ = true;
    arranging_ = false;

    if (moved) repaint();

    const int line = style_.border == Border::none ? 0 : 1;
    const Sides padding = style_.padding;

    arrange(Area{{0, 0}, area_.size}.inner(
        Sides{padding.top + line, padding.right + line, padding.bottom + line, padding.left + line}));

    if (document_ != nullptr) {
        ++document_->tally_.arranged;
        arranged_in_ = document_->frames_;
    }
}

// Sets the state `one` on or off; the node and everything under it is styled again.
void Node::state(State one, bool on) {
    if (states_.has(one) == on) return;

    changing();

    states_.set(one, on);

    restyle();
}

// The look of the node, or of its part `part` (such as "key" for a hot letter), from the sheet of
// its document at the document's color level; outside a document a plain look.
Look Node::look(std::string_view part) const {
    if (part.empty()) return style_.look;

    if (document_ == nullptr) return Look{};

    const std::vector<std::vector<std::string>> classes = lineage();

    return document_->sheet_.style(Selection{classes, states_, part}, document_->colors_).look;
}

// The glyph `name` from the sheet of the document, for its terminal; outside a document from the
// built-in sheet without Unicode.
std::string_view Node::glyph(std::string_view name) const {
    if (document_ == nullptr) return StyleSheet{}.glyph(name, false);

    return document_->sheet_.glyph(name, document_->unicode_);
}

// What the content needs: by default as much as the largest child, and at least the least room.
Size Node::measure(Limits room) const {
    Size made = room.least;

    for (Node& one : children_) {
        const Size wanted = one.measured(room);

        made = Size{std::max(made.wide, wanted.wide), std::max(made.high, wanted.high)};
    }

    return made;
}

// Places the children: by default each one over the whole inside, one on top of another.
void Node::arrange(Area inside) {
    for (Node& one : children_) one.placed(inside);
}

// Paints the content; a plain node has none, only the background and border the document paints.
void Node::paint(Canvas& onto) const { (void)onto; }

// Marks the node to be measured and arranged again, and so every node above it, whose size may
// depend on it; it is repainted too.
void Node::relayout() {
    changing();
    repaint();

    for (Node* at = this; at != nullptr; at = at->parent_) {
        at->sizing_ = true;
        at->arranging_ = true;
    }
}

// Marks where the node stands to be painted again. A node not yet placed is painted when it is.
// In a debug build a repaint while the frame is painted throws std::logic_error.
void Node::repaint() {
    if (document_ == nullptr) return;

#ifndef NDEBUG
    if (document_->phase_ == Document::Phase::painting) throw std::logic_error{"tal::Node: repainted while painting"};
#endif

    if (const std::optional<Area> where = absolute()) document_->damaged(*where);
}

// The first node named `id`, in the whole document (or the tree, outside one), or none.
Node* Node::found(std::string_view id) {
    Node* top = this;

    while (top->parent_ != nullptr) top = top->parent_;

    std::vector<Node*> waiting{top};

    while (!waiting.empty()) {
        Node* one = waiting.back();

        waiting.pop_back();

        if (one->id_ == id) return one;

        for (auto at = one->children_.held_.rbegin(); at != one->children_.held_.rend(); ++at) waiting.push_back(at->get());
    }

    return nullptr;
}

// The margin, border and padding together, side by side.
Sides Node::box() const {
    const int line = style_.border == Border::none ? 0 : 1;
    const Sides& margin = style_.margin;
    const Sides& padding = style_.padding;

    return Sides{margin.top + line + padding.top, margin.right + line + padding.right, margin.bottom + line + padding.bottom,
                 margin.left + line + padding.left};
}

// The classes of the node and of every node above it, the node's first.
std::vector<std::vector<std::string>> Node::lineage() const {
    std::vector<std::vector<std::string>> made;

    for (const Node* at = this; at != nullptr; at = at->parent_) made.push_back(at->classes_);

    return made;
}

// Where the node stands on the screen, or nothing while it or a node above it is not placed.
std::optional<Area> Node::absolute() const {
    Spot at = area_.at;

    if (!placed_) return std::nullopt;

    for (const Node* above = parent_; above != nullptr; above = above->parent_) {
        if (!above->placed_) return std::nullopt;

        at = Spot{at.x + above->area_.at.x, at.y + above->area_.at.y};
    }

    return Area{at, area_.size};
}

// A property of the node changed in the way `effect` says.
void Node::touched(Effect effect) {
    changing();

    if (effect == Effect::layout) relayout();

    if (effect == Effect::paint) repaint();
}

// In a debug build, throws std::logic_error when the tree is changed while the frame is laid out
// or painted: a layout that changes what it lays out would never settle.
void Node::changing() const {
#ifndef NDEBUG
    if (document_ != nullptr && (document_->phase_ == Document::Phase::laying || document_->phase_ == Document::Phase::painting))
        throw std::logic_error{"tal::Node: the tree changed during layout or painting"};
#endif
}

// Marks the node and everything under it to be styled again, since a descendant's selector may
// name this node's classes.
void Node::restyle() {
    std::vector<Node*> waiting{this};

    while (!waiting.empty()) {
        Node* one = waiting.back();

        waiting.pop_back();

        one->styled_ = false;

        for (const std::shared_ptr<Node>& child : one->children_.held_) waiting.push_back(child.get());
    }

    if (document_ != nullptr) document_->restyling_ = true;
}

// Joins the node and everything under it to the tree of `parent`: their document is the parent's,
// and they are styled, measured and placed anew.
void Node::attached(Node* parent) {
    parent_ = parent;

    std::vector<Node*> waiting{this};

    while (!waiting.empty()) {
        Node* one = waiting.back();

        waiting.pop_back();

        one->document_ = parent->document_;
        one->styled_ = false;
        one->sizing_ = true;
        one->arranging_ = true;
        one->placed_ = false;

        for (const std::shared_ptr<Node>& child : one->children_.held_) waiting.push_back(child.get());
    }

    if (document_ != nullptr) document_->restyling_ = true;
}

// Takes the node out of its tree: no parent, and no document for it or anything under it.
void Node::detached() {
    parent_ = nullptr;

    std::vector<Node*> waiting{this};

    while (!waiting.empty()) {
        Node* one = waiting.back();

        waiting.pop_back();

        one->document_ = nullptr;
        one->placed_ = false;

        for (const std::shared_ptr<Node>& child : one->children_.held_) waiting.push_back(child.get());
    }
}

}
