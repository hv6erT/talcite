#include "talcite/menu.hpp"

#include <algorithm>
#include <climits>
#include <memory>
#include <vector>

#include "talcite/line.hpp"
#include "talcite/menuitem.hpp"
#include "talcite/menuseparator.hpp"
#include "talcite/menubar.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"
#include "talcite/submenu.hpp"

#include "lining.hpp"

namespace talcite::ui {

namespace {

// The widths of the box's sides. Without a border there are no sides; with one they are as wide
// as the characters the skin draws it with, as described at `Frame::edges`.
Sides boxed(const Skin& how, Border border) {
    if (border == Border::none) return {0, 0};

    return sided(how.lines(border));
}

}

// A menu is a list of items: the box that the bar lays over the content when it is open. The
// title in the bar is a separate node (`MenuTitle`), which knows this menu and opens it. Each of
// the two rectangles, the title and the box, stands in its own place in the tree and knows where
// it is.
//
// A closed menu is not in the tree: the bar attaches it when it opens and detaches it when it
// closes. So "open" is a single question, whether the menu has a parent, and not a computation
// over the focus.
//
// The list of a submenu is a menu too, attached to the same bar on top of the menu it comes from.
// It has no title; the item it hangs on (`owner_`) stands in for one, as the place it opens beside
// and the place the focus goes back to when it closes.
Menu::Menu(std::string title, MenuShape shape) : title_(std::move(title)), shape_(shape) {
    role("talcite.menubar");
}

// A menu takes any number of items and separators.
int Menu::capacity() const { return plenty; }

// A menu consists of items and separators and nothing else. Anything else would be a hole for
// `item()` that the arrow keys skip over and that painting draws who knows how.
bool Menu::accepts(const Node& one) const {
    return dynamic_cast<const MenuItem*>(&one) != nullptr ||
           dynamic_cast<const MenuSeparator*>(&one) != nullptr;
}

// Open means attached to the bar. One source of truth, without asking about the focus: the bar
// attaches the menu at `open` and detaches it at close, and a menu without items is never
// attached, because a title with nothing below is a button in the bar, not an empty list.
bool Menu::opened() const { return parent() != nullptr; }

// Focusing a menu means opening it: a closed menu is outside the tree and has nothing to stand on.
void Menu::focus() { open(); }

// Opens the menu: the bar attaches it and the focus goes down to an item, the remembered one, or
// the first enabled one when there is none or it is disabled. A menu without items cannot open:
// the focus stays on the title, so the bar is active and folded.
void Menu::open() {
    if (bar_ == nullptr) return;

    if (children().empty()) {
        if (head_ != nullptr && head_->root() != nullptr) head_->focus();

        return;
    }

    bar_->unfold(*this);

    MenuItem* one = item(which_);

    if (one == nullptr || !one->enabled()) one = item(nearest(-1, 1));

    if (one != nullptr) {
        one->focus();

        return;
    }

    // Only disabled items: the box stands, but there is nothing to put the focus on, so it stays
    // on the menu.
    Node::focus();
}

// Closes the menu by detaching it from the bar, the counterpart of `open`. A closed menu starts at
// the top next time; detaching sends no `UnfocusEvent`, so the number is cleared here directly. A
// focus sitting in the menu moves to the title instead of going out: closing is not leaving the
// bar, and a focus lost without an event in `Root::losing` would later come back "from nowhere",
// and the bar would take it for a new entry and forget whom to return the focus to on exit. When
// the focus has already left the menu (a click in the content), there is nothing to move.
//
// A closed menu has nothing to close and keeps its remembered item: `which(n)` on a closed menu
// says where to start when it opens. Clearing the number of an open menu is a safeguard: the focus
// leaves the menu through the title, so the `UnfocusEvent` clears the number anyway.
void Menu::close() {
    Node* up = parent();

    if (up == nullptr) return;

    shut(nullptr);

    which_ = -1;

    changed();

    if (Root* top = root(); top != nullptr && head_ != nullptr)
        if (const Node* had = top->focused_node(); had != nullptr && had->within(*this)) head_->focus();

    (void)up->children().remove(this);
}

// Closes the open lists of this menu's submenus, all but the one of `kept`. A list lies in the bar,
// not in this menu, so closing this menu would not take it away by itself. The children are walked
// over a snapshot, because closing a list moves the focus, and a handler may rebuild the menu.
void Menu::shut(const Node* kept) {
    const std::vector<std::shared_ptr<Node>> these{children().begin(), children().end()};

    for (const std::shared_ptr<Node>& one : these)
        if (auto* deeper = dynamic_cast<Submenu*>(one.get()); deeper != nullptr && deeper != kept && deeper->menu().opened())
            deeper->menu().close();
}

// A menu can always take the focus, which opens it.
bool Menu::focusable() const { return true; }

// The item at the given position, or `nullptr` when the position is outside the menu or holds a
// separator.
MenuItem* Menu::item(int which) const {
    if (which < 0 || which >= static_cast<int>(children().many())) return nullptr;

    return dynamic_cast<MenuItem*>(children()[static_cast<std::size_t>(which)].get());
}

// The inside of the box: the widest item, and as many rows as items. Items are asked with plenty
// of room, because the menu's width comes from its content, not from what is left on the screen.
Size Menu::fold() const {
    int wide = 0;

    for (const std::shared_ptr<Node>& one : children())
        wide = std::max(wide, one->wants({plenty, 1}).wide);

    return {wide, static_cast<int>(children().many())};
}

// The box: the inside, the sides of the border, the shadow, and at least as wide as the title, so
// that it is not narrower than what it comes out of.
Size Menu::wants(Size room) const {
    const Size inner = fold();
    const Sides side = boxed(skin(), shape_.border);

    const int aside = shape_.shade ? 2 : 0;
    const int apart = shape_.shade ? 1 : 0;

    const int wide =
        std::max(inner.wide + side.left + side.right + aside, 2 + measured(title_).wide);

    return {std::min(wide, room.wide), std::min(inner.high + 2 + apart, room.high)};
}

// The highlighted item has two faces, as in a list: `which()` is its number, `current()` the item
// itself, which is pressed, read or compared. Empty means none. Inside, the number is kept as an
// `int`, because `nearest` computes wrapping steps on it and closing clears it with -1.
std::optional<std::size_t> Menu::which() const {
    if (item(which_) == nullptr) return std::nullopt;

    return static_cast<std::size_t>(which_);
}

// The highlighted item itself, or `nullptr` when there is none.
MenuItem* Menu::current() const { return item(which_); }

// Sets the highlighted item. The menu keeps the number, but the highlight itself is the focus, so
// changing the number in an open menu moves the focus, and the previous item goes dark by itself.
// In a closed menu the number is only remembered. A number past the end is cut to the last item,
// as in a list, and a negative one, or any number in a menu without items, means none.
void Menu::aim(int given) {
    const int much = static_cast<int>(children().many());

    which_ = given < 0 || much == 0 ? -1 : std::min(given, much - 1);

    changed();

    if (!opened()) return;

    if (MenuItem* one = item(which_); one != nullptr && one->enabled()) one->focus();
}

// Sets the highlighted item by number; a number too large for an `int` is cut.
void Menu::which(std::size_t given) {
    aim(static_cast<int>(std::min<std::size_t>(given, static_cast<std::size_t>(INT_MAX))));
}

// Highlights the given item. An item that is not in this menu changes nothing.
void Menu::current(MenuItem& one) {
    if (const std::optional<std::size_t> at = children().find(one)) which(*at);
}

// The nearest enabled item in the given direction, wrapping around, or -1. Separators and grayed
// items are skipped by the same condition, because a separator is simply an unavailable item.
int Menu::nearest(int from, int step) const {
    const int much = static_cast<int>(children().many());

    if (much == 0) return -1;

    int at = from >= 0 ? from : (step > 0 ? -1 : much);

    for (int i = 0; i < much; ++i) {
        at = (at + step + much) % much;

        const MenuItem* one = item(at);

        if (one != nullptr && one->enabled()) return at;
    }

    return -1;
}

// Picks the highlighted item. Order is everything here: first the bar is left, only then the
// entrusted action runs. The other way round, the menu would still be on top if the action opened
// a window. A menu without items fires its own `PickEvent`; the title is then the action. A
// submenu is not an action: picking it opens its list, and the bar stays.
void Menu::pick() {
    if (children().empty()) {
        if (bar_ != nullptr) bar_->leave();

        (void)trigger(PickEvent{});

        return;
    }

    MenuItem* one = item(which_);

    if (one == nullptr || !one->enabled()) return;

    if (auto* deeper = dynamic_cast<Submenu*>(one)) {
        deeper->pick();

        return;
    }

    if (bar_ != nullptr) bar_->leave();

    one->pick();
}

// The action of a menu, for a key that arrives here directly rather than through the title, is what
// a click on its title does: a menu without items is picked, any other is opened.
void Menu::activate() {
    if (children().empty()) {
        pick();

        return;
    }

    open();
}

// Lays the items out one under another inside the border. The rectangle starts with the border, so
// the items start at row one.
void Menu::layout(Size) {
    const Size inner = fold();
    const Sides side = boxed(skin(), shape_.border);

    int y = 1;

    for (const std::shared_ptr<Node>& one : children()) one->arrange({{side.left, y++}, {inner.wide, 1}});
}

// Paints the box alone: the border, the items, the shadow. The title is not here; `MenuTitle`
// paints it in its own row.
void Menu::paint(Canvas& onto) const {
    const Skin& how = skin();

    const Style edge = how.style("talcite.menu.frame");

    const Size inner = fold();
    const Sides side = boxed(how, shape_.border);

    const int right_x = side.left + inner.wide;
    const int last_x = right_x + side.right - 1;
    const int last_y = inner.high + 1;

    const Size box{last_x + 1, last_y + 1};

    if (shape_.border != Border::none)
        bordered(onto, box, Rim{1, 1, side.left, side.right}, how.lines(shape_.border), edge);

    int y = 1;

    for (const std::shared_ptr<Node>& one : children()) {
        one->render(onto);

        // A separator reaches the border, as in Midnight Commander. The separator itself does
        // not know the joints, because its rectangle ends at the inside, so the menu draws them.
        if (shape_.border != Border::none &&
            dynamic_cast<const MenuSeparator*>(one.get()) != nullptr) {
            const Lines lines = how.lines(shape_.border);

            onto.put({0, y}, lines.left_joint, edge);
            onto.put({right_x, y}, lines.right_joint, edge);
        }

        ++y;
    }

    if (shape_.shade) shadowed(onto, {{0, 0}, box}, how.style("talcite.shade"));
}

// Three keys belong to the menu; the rest belong to the items or to someone higher up. Only an
// open menu gets here; a closed one is outside the tree, and its keys are handled by the title.
//
// A hotkey is handed out, not decided by asking about types: the same rule and the same mechanism
// as in a dialog. The menu does not guess which item answers to what; it passes the key to its
// children and waits for one to take it. The key is handed out from the menu itself, so it reaches
// only this menu's items and not the content under the bar.
//
// When nobody takes it, we return false and the key goes up to the bar, because the menu is its
// child.
//
// The list of a submenu has two keys more: the left arrow and Esc close it alone and give the focus
// back to its item. Up in the bar they would switch menus or leave the bar altogether.
bool Menu::handle(KeyEvent& what) {
    // With Ctrl or Alt nothing is caught, not even the arrows and Enter. Ctrl+Enter and Alt+arrow
    // are different shortcuts by design, and filtering them only after recognizing the key would
    // take them from the application whenever the menu is open. The button, the text field and
    // the key bar leave them alone the same way.
    const Mods mods = what.mods();

    if (mods.ctrl || mods.alt) return false;

    if (owner_ != nullptr && (what.key().code() == Code::left || what.key().code() == Code::escape)) {
        close();

        return true;
    }

    switch (what.key().code()) {
        case Code::up: aim(nearest(which_, -1)); return true;
        case Code::down: aim(nearest(which_, 1)); return true;
        case Code::enter: pick(); return true;
        default: break;
    }

    if (what.key().empty()) return false;

    return broadcast(what);
}

// The number of the highlighted item arrives by bubbling: a focus placed on an item, by an arrow,
// a click, hovering or from the program, passes here on its way up, so all these paths meet in one
// place instead of each setting the number its own way.
bool Menu::handle(FocusEvent& what) {
    const Node* one = dynamic_cast<const Node*>(what.target());

    if (one == nullptr) return false;

    // A focus on the menu itself means there is nothing to stand on: only disabled items. The
    // number must then be cleared, or it would speak of a highlight that is not visible.
    if (one == this) {
        which_ = -1;

        changed();

        return false;
    }

    for (int at = 0; at < static_cast<int>(children().many()); ++at)
        if (children()[static_cast<std::size_t>(at)].get() == one) {
            which_ = at;

            changed();

            break;
        }

    // Another item takes the highlight, so a list opened from a different item goes away, as in a
    // cascading menu of a desktop. The list of the item that now has the focus stays: the focus
    // comes back to it when its list closes.
    shut(one);

    return false;
}

// A closed menu starts at the top next time, not where it stopped. Moving from item to item is not
// closing: the focus still sits in the menu, so the number is not cleared; otherwise every arrow
// would erase what it had just set.
bool Menu::handle(UnfocusEvent& what) {
    if (const Node* other = what.other(); other != nullptr && other->within(*this)) return false;

    which_ = -1;

    changed();

    return false;
}

}
