#include "talcite/node.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "talcite/root.hpp"
#include "talcite/skin.hpp"

namespace talcite::ui {

Node::Node() : inside_(*this) {}

// A child may outlive its parent if someone holds a shared pointer to it. The link to the parent
// then has to be cut, or a pointer to the dead would remain, and event bubbling follows it. The
// cut is made by the destructor of `Children`.
Node::~Node() = default;

// Sets the role: the stem of the name in the skin from which the node builds its variants, such
// as .focus, .key, .off, .title. It lives in `Node` rather than in each widget, because it is a
// property of a node that paints, and which nodes paint is decided by `paint`, not by the type.
// Containers carry an empty string and get nothing from it; the cost is 32 bytes per node. The
// setter is virtual, because a node made of others, such as a message window, has to pass the
// stem on to its parts.
void Node::role(std::string given) {
    if (role_ == given) return;

    role_ = std::move(given);

    changed();
}

// Cuts the children's links to the dying parent. This belongs to the children, because they hold
// the links. A `Node` dies together with this member, so it runs exactly once and exactly when it
// should.
Children::~Children() {
    for (const std::shared_ptr<Node>& one : inside_) one->up_ = nullptr;
}

// How many children a node accepts; only the node itself knows. One by default: a plain node is
// a wrapper, not a layout. Laying out several side by side belongs to `Row` and `Column`, and only
// they return plenty here. A label returns zero, because it has nothing to hold.
int Node::capacity() const { return 1; }

// `capacity()` says how many, `accepts()` says what kind. A plain node knows nothing about kinds
// and takes what it is given: a layout, a frame and a curtain arrange everything the same way. It
// is overridden by those whose children have a role to fill: a menu has items, a list has rows,
// and both later walk over these children assuming what they are.
//
// Without this the assumption would only be stated. `Menu::item()` casts to `MenuItem` on every
// access (from arrow traversal, from painting, from a press) and gives nothing for a foreign child
// that nobody forbade attaching. Checking once, at attachment, is cheaper and more honest than
// returning nothing over and over.
bool Node::accepts(const Node&) const { return true; }

// Gives the focus away from this node: to the nearest focusable ancestor, and only when there is
// none, to nobody. Clearing it outright would break modality: a dialog swallows keys as the focus,
// so without the focus a key would go straight to the root, and a modal window would stop being
// modal.
//
// Every widget that becomes unfocusable while holding the focus calls this: a button, a text
// field, a list row, a menu item, a check box, a group of options and a choice. One rule in one
// place cannot drift apart.
void Node::unfocus() {
    if (!focused()) return;

    Root* top = root();

    if (top == nullptr) return;

    for (Node* at = up_; at != nullptr; at = at->up_)
        if (at->focusable()) {
            at->focus();

            return;
        }

    top->move_focus(nullptr);
}

// Attaches a child. A null node, a node that already has a parent, one more child than
// `capacity()` allows, a kind the parent does not accept, and a cycle all throw std::logic_error.
//
// Every attach and every detach counts in `changes()`. A parent that does not draw all its children
// in every frame (a list draws the rows in view) tells by it that the set has changed since it last
// looked; each one keeps the count it saw, so reading it changes nothing and nobody can spoil the
// answer for another.
void Children::add(std::shared_ptr<Node> given) {
    if (!given) throw std::logic_error("talcite::ui::Node: nothing to attach");

    if (given->up_ != nullptr) throw std::logic_error("talcite::ui::Node: the node already has a parent");

    if (static_cast<int>(inside_.size()) >= whose_->capacity())
        throw std::logic_error("talcite::ui::Node: the node takes no more children");

    if (!whose_->accepts(*given))
        throw std::logic_error("talcite::ui::Node: a node of the wrong kind");

    // A cycle in this tree would make event bubbling go round forever. The check costs a walk of
    // the tree's depth, that is, nothing, and it also catches attaching a node to itself.
    for (const Node* at = whose_; at != nullptr; at = at->up_)
        if (at == given.get())
            throw std::logic_error("talcite::ui::Node: a node cannot be its own ancestor");

    given->up_ = whose_;

    inside_.push_back(std::move(given));

    ++changes_;

    whose_->changed();
}

// Attaches several children: all or none. On an error halfway, what has already gone in is
// removed and the exception is rethrown; otherwise the caller would be left with a half-built tree
// without knowing which half.
void Children::add(std::vector<std::shared_ptr<Node>> given) {
    const std::size_t was = inside_.size();

    try {
        for (std::shared_ptr<Node>& one : given) add(std::move(one));
    } catch (...) {
        while (inside_.size() > was) (void)remove(inside_.back().get());

        throw;
    }
}

// Detaches a child and returns it, so moving a node under another parent is a single expression.
// A node that is not a child gives null, not an exception; whoever does not check will still get
// a clear error at the next add, because attaching null is forbidden.
std::shared_ptr<Node> Children::remove(Node* one) {
    const auto at = std::find_if(inside_.begin(), inside_.end(),
                                 [one](const std::shared_ptr<Node>& mine) {
                                     return mine.get() == one;
                                 });

    if (at == inside_.end()) return nullptr;

    std::shared_ptr<Node> made = std::move(*at);

    inside_.erase(at);

    ++changes_;

    made->up_ = nullptr;

    whose_->changed();

    // The root drops the focus, and then keeps the node alive until the end of the delivery,
    // because removal is sometimes called from inside an event passing through this very node.
    if (Root* top = whose_->root()) {
        top->losing(made.get());
        top->linger(made);
    }

    return made;
}

// Detaches all children. They are removed from the end, so the vector shifts nobody, and the root
// gets the nodes in the reverse order of attaching, as when the tree is destroyed.
void Children::clear() {
    Root* top = whose_->root();

    if (!inside_.empty()) whose_->changed();

    while (!inside_.empty()) {
        std::shared_ptr<Node> made = std::move(inside_.back());

        inside_.pop_back();

        ++changes_;

        made->up_ = nullptr;

        if (top != nullptr) {
            top->losing(made.get());
            top->linger(made);
        }
    }
}

// Whether the given node is one of these children.
bool Children::contains(const Node* one) const {
    return std::any_of(inside_.begin(), inside_.end(),
                       [one](const std::shared_ptr<Node>& mine) { return mine.get() == one; });
}

// The position of a node among the children, or nothing when it is not a child. Whoever has a
// pointer to a row but needs its number (to reach into an array of their own alongside) asks here.
std::optional<std::size_t> Children::find(const Node& one) const {
    for (std::size_t at = 0; at < inside_.size(); ++at)
        if (inside_[at].get() == &one) return at;

    return std::nullopt;
}

// The root of the tree, or `nullptr` for a node not in a tree with a `Root`. It is found by
// walking up, not remembered. The tree is about five deep, and a remembered field would have to be
// maintained on every attach and detach, and would dangle after the root died. The walk is also
// more truthful: a node detached from the tree has no root, and that is exactly what it returns.
Root* Node::root() const {
    const Node* at = this;

    while (at->up_ != nullptr) at = at->up_;

    return const_cast<Root*>(dynamic_cast<const Root*>(at));
}

// One flag, one meaning. Only a node that admits to it takes the focus, and Tab walks over the
// same nodes. If `focus()` worked on anything while the flag governed only the traversal, the
// focus could land where Tab never goes: two notions under one name. False by default, because a
// node that only lays out or paints has nothing to do with the keyboard.
bool Node::focusable() const { return false; }

// The skin in effect. The root holds the skin, and the node reaches it by the same walk up by
// which it reaches everything else. Without a root (a node not yet attached, or a detached
// subtree) the built-in skin is used, so asking about looks never fails.
//
// Call it once per `paint`, into a local variable: every call is a walk up and a dynamic_cast at
// the top.
const Skin& Node::skin() const {
    const Root* top = root();

    return top != nullptr ? top->skin() : Skin::plain();
}

// Whether this node holds the focus of its root.
bool Node::focused() const {
    const Root* top = root();

    return top != nullptr && top->focused_node() == this;
}

// Whether this node is `under` or lies somewhere below it, by its chain of parents: the one place
// that asks it, for a focus inside a window, a menu, a list or a layer that closes.
bool Node::within(const Node& under) const {
    for (const Node* at = this; at != nullptr; at = at->parent())
        if (at == &under) return true;

    return false;
}

// Moves the focus to this node; a node outside a tree throws std::logic_error. It is virtual,
// because some widgets do not take the focus themselves but hand it to their inside: a list puts
// it on its current item, not on itself. Tab traversal holds plain `Node*` pointers and calls
// `focus()` on a stop, so without virtuality the focus would land on the widget instead of where
// it belongs. The root's own focus setter stays non-virtual: it sets, while this only points.
void Node::focus() {
    Root* top = root();

    if (top == nullptr)
        throw std::logic_error("talcite::ui::Node: a node outside the tree cannot take focus");

    top->move_focus(this);
}

// Lays the node out in the given rectangle (in the parent's coordinates), as much of it visible as
// of the parent. The rectangle is recorded, because mouse hits depend on it, and so is the visible
// part (`Root::under`: a row scrolled under a bar has a rectangle there but shows nothing); then
// the node places its children (`layout`). Non-virtual for the same reason trigger is: recording
// must always happen, and a widget does not show that its author forgot it. A node that cannot be
// seen gets an empty rectangle, so its area() tells the truth about this frame instead of
// remembering the previous one.
void Node::arrange(const Area& where) {
    recorded(where, up_ == nullptr ? Area{{0, 0}, where.size} : Area::crossed(up_->seen_, where) - where.at);
}

// The same, with only `room` of the parent (in its coordinates) letting the node be seen: the
// window of a scroller without its bars.
void Node::arrange(const Area& where, const Area& room) {
    const Area open = up_ == nullptr ? room : Area::crossed(up_->seen_, room);

    recorded(where, Area::crossed(open, where) - where.at);
}

// Records the rectangle and its visible part (in the node's own coordinates), and where they stand
// on the screen, and lays the children out. A visible part that stands elsewhere on the screen than
// in the last layout, or is another size, is a change of both places, which the root repaints: so
// a node that moved needs no `changed` of its own.
void Node::recorded(const Area& where, const Area& seen) {
    area_ = where;
    seen_ = seen;
    origin_ = (up_ == nullptr ? Spot{} : up_->origin_) + where.at;

    const Area now = seen.size.wide > 0 && seen.size.high > 0 ? seen + origin_ : Area{};

    if (now != shown_) {
        if (Root* top = root(); top != nullptr) {
            top->damage(shown_);
            top->damage(now);
        }

        shown_ = now;
    }

    layout(where.size);
}

// The node looks different now: the root repaints where it stood in the last layout, at the next
// frame. Whoever changes what `paint` reads says so; a node that moves or changes size is found by
// the layout itself (`recorded`). A node outside the tree has no screen to repaint; once attached,
// the layout finds its new place anyway.
void Node::changed() {
    if (Root* top = root(); top != nullptr) top->announced(*this, shown_);
}

// Only `part` of the node (in its own coordinates) looks different: a table's two rows of the
// cursor rather than the whole table.
void Node::changed(const Area& part) {
    if (Root* top = root(); top != nullptr) top->announced(*this, Area::crossed(part + origin_, shown_));
}

// Paints the node, laid out before (`arrange`), onto its parent's canvas: its own canvas is the
// parent's narrowed to the node's rectangle, and a node of which nothing can be seen paints
// nothing, which is all the saving in scrolling.
void Node::render(Canvas& onto) const {
    Canvas mine = onto.within(area_);

    const Area seen = mine.seen();

    if (seen.size.wide <= 0 || seen.size.high <= 0) return;

    paint(mine);
}

// Lays out and paints the node, fully visible in the given rectangle, on a sheet of its own, in
// which whatever the node does not paint stays undrawn. The frame of a terminal and the trials
// start here.
View Node::render(const Area& where) {
    recorded(where, {{0, 0}, where.size});

    View made{where.size};

    made.clear({{0, 0}, where.size});

    if (where.size.wide <= 0 || where.size.high <= 0) return made;

    Canvas whole{made};

    paint(whole);

    return made;
}

// A plain node gives its only child its whole rectangle. Laying out several children is the
// business of `Row`, `Column` and `Stack`.
void Node::layout(Size given) {
    if (!inside_.empty()) inside_.front()->arrange({{0, 0}, given});
}

// Paints the only child; the rest stays transparent. The rule for every node: paint what you are,
// not the rectangle you were given, so that the parent's background shows wherever nothing is.
void Node::paint(Canvas& onto) const {
    if (!inside_.empty()) inside_.front()->render(onto);
}

// Where this node wants the cursor, in its own coordinates, or nothing. The cursor is not a
// property of a sheet. There is one per frame and it belongs to the root, like the skin; keeping
// it in `View` would mean carrying it through every sheet, a field in each of them, and coordinate
// arithmetic in a place that knows nothing about coordinates.
//
// So a widget places the cursor nowhere; it says where it wants it. The root asks the question,
// and only of the focus, because there is exactly one blinking caret. None by default: the vast
// majority of widgets have nothing to show.
std::optional<Spot> Node::caret() const { return {}; }

// How the cursor of this node looks while it has one: the shape it was given (`caret_shape`), by
// default the terminal's usual cursor. A shape is a wish to the terminal, which draws the cursor:
// a terminal that knows no shapes, or one where blinking is turned off, shows its own.
CaretShape Node::caret_shape() const { return caret_shape_; }

// Gives the cursor of this node a shape and blinking, for the frames in which it has the focus.
void Node::caret_shape(CaretShape given) { caret_shape_ = given; }

// A plain node wants as much as its only child and gives it its whole rectangle. Laying out
// several children is the business of `Row` and `Column`.
Size Node::wants(Size room) const {
    if (inside_.empty()) return {};

    return inside_.front()->wants(room);
}

// The node's place on the screen, assembled by walking up over the rectangles, the same walk by
// which the root assembles the cursor position in a nested field and by which the mouse goes down.
//
// Computed on every question, not remembered: a coordinate recorded at opening would go stale at
// the first window resize, at a scroll, and when anything above grows, and then a box would stay
// where the widget used to be. Empty when the node has no rectangle yet, because nobody laid it
// out.
//
// Everyone who stands under another node needs this: an open choice list, an open menu, a window
// moved around the screen. It is a question to the node, like `area()`, and its name says which
// coordinate system it means.
std::optional<Area> Node::screen_area() const {
    Area made = area_;

    if (made.size.wide <= 0 || made.size.high <= 0) return {};

    for (const Node* at = up_; at != nullptr; at = at->up_) {
        made.at.x += at->area_.at.x;
        made.at.y += at->area_.at.y;
    }

    return made;
}

// The node's rectangle in the coordinates of the ancestor `top`: the sum of `area().at` from the
// node up to the child of `top` (excluding `top` itself: in a scroller its own `at` is the scroll
// offset, and we want content coordinates that do not depend on scrolling). A node without a
// rectangle (nobody laid it out, because its whole container lay outside the window and `render`
// never reached the children) is replaced by the nearest laid-out ancestor: scrolling to it pulls
// the container into the window, the next frame lays out the children, and the caller asks again,
// deeper. Nothing when the node does not lie under `top` at all.
//
// It is the same question as `screen_area()`, only about another frame of reference, so it stands
// next to it.
std::optional<Area> Node::area_in(const Node& top) const {
    const Node* from = this;

    while (from != nullptr && from != &top &&
           (from->area_.size.wide <= 0 || from->area_.size.high <= 0))
        from = from->up_;

    if (from == nullptr || from == &top) return std::nullopt;

    Area made = from->area_;

    for (const Node* at = from->up_; at != nullptr; at = at->up_) {
        if (at == &top) return made;

        made.at.x += at->area_.at.x;
        made.at.y += at->area_.at.y;
    }

    return std::nullopt;
}

// Converts a screen cell into this node's own coordinates. A mouse event carries a screen cell,
// and `area().at` speaks of one level only. Subtracting `area().at` alone gives the right result
// only when the offsets of all ancestors sum to zero, as in a test where the widget stands
// directly under the root. In a dialog, a frame or a row a click would land as many cells off as
// the ancestors' offset.
//
// The walk up is the same arithmetic that `Root::under` does going down. The tree is a few levels
// deep, so it costs next to nothing, and it has to be computed at the event, not at the frame,
// because the tree may have moved in the meantime. It is the inverse of `screen_area()` for a
// single cell.
Spot Node::local(Spot where) const {
    for (const Node* at = this; at != nullptr; at = at->up_) where = where - at->area_.at;

    return where;
}

}
