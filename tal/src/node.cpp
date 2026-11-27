#include "tal/node.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

#include "tal/document.hpp"
#include "damage.hpp"


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

    if (owner_.document_ != nullptr) owner_.document_->forgotten(one);

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

    if (owner_.document_ != nullptr)
        for (const std::shared_ptr<Node>& one : held_) owner_.document_->forgotten(*one);

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

        if (document_ != this) document_->arranged_.push_back(shared_from_this());
    }
}

// Makes the node active or not. An inactive node is greyed out (the state `disabled`), and it and
// everything under it take no events and no focus: a focus inside moves on as Tab moves it, and a
// capture inside ends. Then every node whose activity changed hears `Enabled` or `Disabled`: the
// node and those under it, but not under a node inactive on its own, and none while a node above
// is inactive, since then nothing changes.
void Node::enable(bool on) {
    if (enabled_ == on) return;

    changing();

    enabled_ = on;

    state(State::disabled, !on);

    if (!on && document_ != nullptr) {
        if (document_->captured_ != nullptr && holds(*document_->captured_)) document_->captured_ = nullptr;

        if (document_->focus_ != nullptr && holds(*document_->focus_)) document_->moved(document_->following(document_->focus_, false, this));
    }

    for (const Node* above = parent_; above != nullptr; above = above->parent_)
        if (!above->enabled_) return;

    std::vector<std::weak_ptr<Node>> under;
    std::vector<Node*> waiting{this};

    while (!waiting.empty()) {
        Node* one = waiting.back();

        waiting.pop_back();

        if (one != this) under.push_back(one->weak_from_this());

        for (auto at = one->children_.held_.rbegin(); at != one->children_.held_.rend(); ++at)
            if ((*at)->enabled_) waiting.push_back(at->get());
    }

    const auto told = [on](Node& one) {
        if (on) {
            Enabled made;

            one.notified(made);
        } else {
            Disabled made;

            one.notified(made);
        }
    };

    told(*this);

    for (const std::weak_ptr<Node>& one : under)
        if (const std::shared_ptr<Node> held = one.lock()) told(*held);
}

// Whether the node can take the focus now: it asks for it, it and every node above it are active,
// and it stands placed in a document with cells to show.
bool Node::focusable() const {
    if (!focusable_ || document_ == nullptr || inert()) return false;

    const std::optional<Area> where = absolute();

    return where && where->size.wide > 0 && where->size.high > 0;
}

// Sets how the node takes room in a row, a column or a grid: with weight 0 as much as it wants,
// otherwise `least` and a part of what is left in proportion to its weight. A negative weight or
// least counts as zero. The parent lays its children out again.
void Node::share(Share how) {
    how = Share{std::max(how.weight, 0), std::max(how.least, 0)};

    if (how == share_) return;

    changing();

    share_ = how;

    if (parent_ != nullptr) parent_->relayout();
}

// Takes the focus, when the node can take it (`focusable`); otherwise nothing happens. While the
// document confines the focus elsewhere (`Document::confine`), the request waits and is granted
// once the confinement lets the node in.
void Node::focus() {
    if (!focusable()) return;

    if (document_->confined_ != nullptr && !document_->confined_->holds(*this)) {
        document_->deferred_ = weak_from_this();

        return;
    }

    document_->moved(this);
}

// Whether the node has the focus of its document.
bool Node::focused() const { return document_ != nullptr && document_->focus_ == this; }

// Says whether the node takes the state `hover` while the mouse is over it or over a node under it.
// A node that is only the ground under its children does not, so that a sheet's hover look meant
// for them does not cover it whole; turned off, it loses the state at once.
void Node::hoverable(bool on) {
    if (hoverable_ == on) return;

    hoverable_ = on;

    if (!on) state(State::hover, false);
}

// Says whether the node wants the focus. A node that has it and stops wanting it passes it on.
void Node::focusable(bool on) {
    if (focusable_ == on) return;

    focusable_ = on;

    if (!on && focused()) document_->moved(document_->following(this, false, this));
}

// Makes the mouse go to this node, wherever the pointer is, until the button is released: a drag
// that leaves the node keeps it. A node outside a document has no mouse to capture.
void Node::capture() {
    if (document_ != nullptr) document_->captured_ = this;
}

// Begins carrying `carried` from the node: the document follows the pointer with it instead of
// telling the mouse (`DragOver` to the nodes under it, `Dropped` to the one that takes it when the
// button comes up; Esc gives up). Nothing outside a document; no load throws std::invalid_argument.
void Node::dragged(std::shared_ptr<Carried> carried) {
    if (carried == nullptr) throw std::invalid_argument{"tal::Node::dragged: nothing carried"};

    if (document_ != nullptr) document_->began(std::move(carried));
}

// The spot `screen` of the screen counted from the node's corner; none while the node or one above
// it is not placed.
std::optional<Spot> Node::local(Spot screen) const {
    const std::optional<Area> where = absolute();

    if (!where) return std::nullopt;

    return Spot{screen.x - where->at.x, screen.y - where->at.y};
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

    return document_->sheet_.style(Selection{classes, states_, part}, document_->display_.colors).look;
}

// The look the node's part `part` (none for the node itself) would have in the states `states`
// instead of its own: a view draws its rows so, as if each were a node of its own. Outside a
// document a plain look.
Look Node::look(std::string_view part, States states) const {
    if (document_ == nullptr) return Look{};

    const std::vector<std::vector<std::string>> classes = lineage();

    return document_->sheet_.style(Selection{classes, states, part}, document_->display_.colors).look;
}

// The glyph `name` from the sheet of the document, for its terminal; outside a document from the
// built-in sheet without Unicode.
std::string_view Node::glyph(std::string_view name) const {
    if (document_ == nullptr) return StyleSheet{}.glyph(name, false);

    return document_->sheet_.glyph(name, document_->display_.unicode);
}

// How the terminal of the document draws emoji sequences, so that the node measures text as the
// surface paints it; outside a document tal's usual rendering.
text::Rendering Node::rendering() const {
    if (document_ == nullptr) return {};

    return document_->display_.rendering;
}

// What the document is shown on (`Document::display`); outside a document what a terminal that
// answers nothing gives: full color, Unicode, block characters and a cell of unknown size.
const Display& Node::display() const {
    static const Display silent;

    return document_ == nullptr ? silent : document_->display_;
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

// Draws on the node's box, its border included, after the border and before the content; a plain
// node draws nothing there. A window puts its title on its top border this way.
void Node::outline(Canvas& box) const { (void)box; }

// Where the node's caret stands, in the coordinates of its content, and how it looks; a plain node
// has none. The document asks the node that has the focus.
std::optional<Caret> Node::caret() const { return std::nullopt; }

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
    if (document_->stage_ == Document::Stage::painting) throw std::logic_error{"tal::Node: repainted while painting"};
#endif

    const std::optional<Area> where = absolute();

    if (!where) return;

    document_->damaged(*where);

    if (style_.shadow)
        for (const Area one : damage::shadowed(*where)) document_->damaged(one);
}

// Paints the node again where `part` (counted from the corner of its box) meets it at the next
// frame: only there, without its shadow, which does not change with what lies inside.
void Node::repaint(Area part) {
    if (document_ == nullptr) return;

#ifndef NDEBUG
    if (document_->stage_ == Document::Stage::painting) throw std::logic_error{"tal::Node: repainted while painting"};
#endif

    const std::optional<Area> where = absolute();

    if (!where) return;

    document_->damaged(Area{{where->at.x + part.at.x, where->at.y + part.at.y}, part.size}.common(*where));
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

// Adds a handler of events of `type` to the node; the hook it returns takes it away again.
Hook Node::hooked(std::type_index type, std::shared_ptr<detail::Slot> slot) {
    std::erase_if(slots_, [](const auto& one) { return !one.second->live; });

    std::weak_ptr<detail::Slot> held = slot;

    slots_.emplace_back(type, std::move(slot));

    return Hook{std::move(held)};
}

// The live handlers of the node for an event of the kind of `one`: the default actions when
// `fallback`, otherwise those of the phase `phase`. A copy, so that a handler may add or remove
// handlers while the event is delivered.
std::vector<std::shared_ptr<detail::Slot>> Node::slots(const Event& one, bool fallback, Phase phase) const {
    std::vector<std::shared_ptr<detail::Slot>> made;
    const std::type_index type{typeid(one)};

    for (const auto& [kind, slot] : slots_)
        if (kind == type && slot->live && slot->fallback == fallback && (fallback || slot->phase == phase)) made.push_back(slot);

    return made;
}

// Calls the handlers of the node for `one` (`slots`) in the order they were added, skipping those
// whose hook went meanwhile; stops at the first that takes the event. Returns whether it is taken.
bool Node::called(Event& one, bool fallback, Phase phase) {
    for (const std::shared_ptr<detail::Slot>& slot : slots(one, fallback, phase)) {
        if (!slot->live) continue;

        slot->called(one);

        if (one.taken()) return true;
    }

    return one.taken();
}

// Delivers a notification to this node alone, its capturing handlers first.
void Node::notified(Event& one) {
    one.target_ = this;

    if (!called(one, false, Phase::capture)) (void)called(one, false, Phase::bubble);
}

// Tells the node that its property at `source` changed (`Changed`).
void Node::changed(const void* source) {
    Changed one{source};

    notified(one);
}

// Whether the node or a node above it is inactive.
bool Node::inert() const {
    for (const Node* at = this; at != nullptr; at = at->parent_)
        if (!at->enabled_) return true;

    return false;
}

// Whether `one` is this node or lies under it.
bool Node::holds(const Node& one) const {
    for (const Node* at = &one; at != nullptr; at = at->parent_)
        if (at == this) return true;

    return false;
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
    if (document_ != nullptr && (document_->stage_ == Document::Stage::laying || document_->stage_ == Document::Stage::painting))
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


namespace {

// A color as #rrggbb, or "default" for none.
std::string spelled(const std::optional<Color>& one) {
    if (!one) return "default";

    constexpr std::string_view digits = "0123456789abcdef";
    std::string made = "#";

    for (const std::uint8_t part : {one->red, one->green, one->blue}) {
        made += digits[part >> 4];
        made += digits[part & 0x0F];
    }

    return made;
}

}

// The node and those under it as text, a line each, indented two spaces by depth: its id ("-" for
// none), its classes after dots, its states in brackets, its place on its parent and its size, and
// its style: the ink and the paper, the looks it has on, the border and the padding.
std::string Node::described() const {
    constexpr std::array<std::pair<State, std::string_view>, 7> named{{{State::focus, "focus"},
                                                                       {State::disabled, "disabled"},
                                                                       {State::select, "select"},
                                                                       {State::current, "current"},
                                                                       {State::mark, "mark"},
                                                                       {State::drag, "drag"},
                                                                       {State::hover, "hover"}}};
    constexpr std::array<std::string_view, 4> borders{"none", "thin", "twin", "thick"};
    std::string made;

    const auto line = [&](const auto& self, const Node& one, int depth) -> void {
        made.append(static_cast<std::size_t>(2 * depth), ' ');
        made += one.id_.empty() ? "-" : one.id_;

        for (const std::string& name : one.classes_) made += "." + name;

        std::string states;

        for (const auto& [state, name] : named)
            if (one.states_.has(state)) states += (states.empty() ? "" : " ") + std::string{name};

        if (!states.empty()) made += " [" + states + "]";

        const Look& look = one.style_.look;
        const Sides& padding = one.style_.padding;

        made += " " + std::to_string(one.area_.at.x) + "," + std::to_string(one.area_.at.y) + " " + std::to_string(one.area_.size.wide) + "x" +
                std::to_string(one.area_.size.high);
        made += " ink " + spelled(look.ink) + " paper " + spelled(look.paper);

        for (const auto& [on, name] : {std::pair{look.bold, "bold"}, std::pair{look.faint, "faint"}, std::pair{look.italic, "italic"},
                                       std::pair{look.under, "under"}, std::pair{look.reverse, "reverse"}, std::pair{look.crossed, "crossed"}})
            if (on) made += std::string{" "} + name;

        made += " border " + std::string{borders[static_cast<std::size_t>(one.style_.border)]};
        made += " padding " + std::to_string(padding.top) + " " + std::to_string(padding.right) + " " + std::to_string(padding.bottom) + " " +
                std::to_string(padding.left) + "\n";

        for (const Node& child : one.children_) self(self, child, depth + 1);
    };

    line(line, *this, 0);

    return made;
}

}
