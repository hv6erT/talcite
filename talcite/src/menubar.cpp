#include "talcite/menubar.hpp"

#include <algorithm>
#include <memory>
#include <utility>

#include "talcite/menu.hpp"
#include "talcite/root.hpp"

#include "menutitle.hpp"
#include "slot.hpp"

namespace talcite::ui {

// The menu bar is a stack with two floors. At the bottom a column: the row of titles above the
// content row. On top, only while a menu is open, the menu itself, which tells the stack where to
// stand: under its title. Painting and mouse hits are done by `Stack`, laying out the titles by
// `Row`; the bar computes no widths or positions and paints nothing of its own.
//
//     Menubar (Stack)
//     ├── Column
//     │   ├── Row  ← MenuTitle × N
//     │   └── Row  ← slot for the application's content
//     └── [Menu]  ← attached only while open; placed under its title
//
// An open menu is a child of the bar, so a key from an item bubbles item → menu → bar, and the
// side arrows and Esc arrive by that path. The menu is not a curtain, so a click on a neighboring
// title hits the title, and the bar stands under the legend, so the list does not cover the key
// bar. The three costs a curtain menu would have do not arise here at all.
Menubar::Menubar()
    : frame_(std::make_shared<Column>()),
      bar_(std::make_shared<Row>()),
      body_(std::make_shared<Row>()) {
    frame_->children().add(bar_);
    frame_->children().add(body_);

    children().add(frame_);
}

// The content underneath, or `nullptr` when there is none. The slot is described in `slot.hpp`.
Node* Menubar::content() const { return held(*body_); }

// Puts the given node in the content slot, replacing what was there.
void Menubar::content(std::shared_ptr<Node> given) { hold(*body_, std::move(given)); }

// Two places: the column and at most one open menu.
int Menubar::capacity() const { return 2; }

// Only our own children are accepted: the column and our own menus. A foreign child cannot come
// in, because the stack would give it a place and the bar would not know what to do with it.
bool Menubar::accepts(const Node& one) const {
    if (&one == frame_.get()) return true;

    for (const std::shared_ptr<Menu>& mine : menus_)
        if (mine.get() == &one) return true;

    return false;
}

// Creates a menu with the given title and returns it. The bar makes the menu, not the caller. The
// title goes into the row of titles at once; the menu stays outside the tree until someone opens
// it. The two know each other through the bar: the title holds the menu, the menu holds the title
// and the bar, by raw pointers, because all three live and die together here.
std::shared_ptr<Menu> Menubar::add(std::string title, MenuShape shape) {
    auto one = std::make_shared<Menu>(std::move(title), shape);
    auto head = std::make_shared<MenuTitle>(*one);

    one->bar_ = this;
    one->head_ = head.get();

    menus_.push_back(one);

    try {
        bar_->children().add(head);
    } catch (...) {
        menus_.pop_back();

        throw;
    }

    return one;
}

// Closes any open menu and removes all menus and their titles.
void Menubar::clear() {
    fold();

    expanded_ = false;

    bar_->children().clear();
    menus_.clear();
}

// The menu the bar stands on: the open one, or the one whose title has the focus (active but
// folded), or `nullptr`. Public, because the title is an internal node that cannot be asked about
// the focus from outside, and "which menu is current" is a question both the application and the
// tests ask.
Menu* Menubar::current() const {
    for (const std::shared_ptr<Menu>& one : menus_)
        if (one->opened()) return one.get();

    for (const std::shared_ptr<Menu>& one : menus_)
        if (one->head_ != nullptr && one->head_->focused()) return one.get();

    return nullptr;
}

// The position of the given menu in the bar, or -1.
int Menubar::find(const Menu* one) const {
    for (int at = 0; at < static_cast<int>(menus_.size()); ++at)
        if (menus_[static_cast<std::size_t>(at)].get() == one) return at;

    return -1;
}

// Whether the node belongs to the bar: everything in the row of titles or in an open menu is
// ours. The whole branch upward is checked, because the focus sits on an item or a title, not on
// the bar.
bool Menubar::ours(const Node* one) const {
    for (const Node* at = one; at != nullptr; at = at->parent()) {
        if (at == bar_.get()) return true;

        for (const std::shared_ptr<Menu>& mine : menus_)
            if (mine.get() == at) return true;
    }

    return false;
}

// Opening is attaching. At most one menu is open at a time, so whatever stood open is closed
// first, and then the new menu is attached as the last child of the stack, that is, on top.
void Menubar::unfold(Menu& one) {
    if (one.parent() == this) return;

    fold();

    for (const std::shared_ptr<Menu>& mine : menus_)
        if (mine.get() == &one) {
            children().add(mine);

            expanded_ = true;

            return;
        }
}

// Closes the current menu. The menu closes itself (`Menu::close`); the bar does not touch its item
// number. A current menu that is only a highlighted title is left alone by the close.
void Menubar::fold() {
    if (Menu* one = current()) one->close();
}

// Entering the bar puts the focus on the first title with nothing open, as in Midnight Commander
// with drop_menus off; only the down arrow, Enter, a letter or a click opens a menu. With
// `dropdown()` entering opens the first menu at once, as MC does with "Drop down menus", and sets
// the open bit, so the side arrows keep menus open; it is the same path as the down arrow after
// F9, only without waiting for it. A first menu without items stays folded, as with the arrow.
// Public, paired with `leave()`, because a key bar field has to enter by the same path as the key;
// otherwise a click on the field would do something other than F9. A bar already active stays as
// it is.
void Menubar::enter() {
    if (menus_.empty() || current() != nullptr) return;

    expanded_ = false;

    if (dropdown_) {
        menus_.front()->open();
        expanded_ = true;

        return;
    }

    if (Node* head = menus_.front()->head_) head->focus();
}

// Leaves the bar: closes the menu and gives the focus back to whoever had it before entering.
void Menubar::leave() {
    fold();

    expanded_ = false;

    Root* top = root();

    if (top == nullptr) return;

    std::shared_ptr<Node> was = was_.lock();

    was_.reset();

    if (was && was->root() != top) was.reset();

    top->move_focus(was.get());
}

// Toggles the bar: enters when it is inactive, leaves when it is active. The opening key is given
// by the caller, not by the library: adding `Code::f9` to `keys()` gives the Midnight Commander
// behavior, and an empty set means "not from the keyboard".
void Menubar::activate() {
    if (current() == nullptr) enter();
    else leave();
}

// The bar's own keys: the opening key, Esc, the side arrows, and the title letters while the bar
// is active.
bool Menubar::handle(KeyEvent& what) {
    if (menus_.empty()) return false;

    Menu* one = current();

    const int much = static_cast<int>(menus_.size());

    if (keys().contains(what.key())) {
        activate();

        return true;
    }

    switch (what.key().code()) {
        case Code::escape:
            if (one == nullptr) return false;

            leave();

            return true;

        case Code::left:
        case Code::right: {
            if (one == nullptr) return false;

            const int step = what.key().code() == Code::left ? -1 : 1;

            // The neighbor takes over the state of the current menu: with a menu open the arrow
            // opens the next one, with it folded it only moves the highlight. That is how the
            // Midnight Commander bar moves. The bar is asked, not the current menu: a menu without
            // items is never open, so passing through such a title would lose the state and the
            // neighbor would come out folded.
            const std::shared_ptr<Menu>& next =
                menus_[static_cast<std::size_t>((find(one) + step + much) % much)];

            if (expanded_) {
                // Close explicitly, because a menu without items does not open and so would not
                // close its predecessor itself, and the bar's state has to carry on anyway.
                fold();
                next->open();
                expanded_ = true;
            } else if (next->head_ != nullptr) {
                next->head_->focus();
            }

            return true;
        }

        // A title letter works only while the bar is active, that is, after F9, as in Midnight
        // Commander. Without this condition an ordinary letter pressed over a panel would open a
        // menu.
        //
        // The items of an open menu get the letter earlier, because `Menu::handle` lies closer to
        // the focus. Only what nobody inside took arrives here, and then the titles are asked.
        // The loop runs over a snapshot, because a handler may rebuild the bar in response.
        //
        // An active bar swallows everything it did not handle itself. In Midnight Commander an
        // open menu is modal: F5 does not copy while it is open. The legend stands higher up and
        // would catch the key, performing an action nobody asked for.
        default: {
            if (one == nullptr) return false;

            const std::vector<std::shared_ptr<Menu>> these = menus_;

            for (const std::shared_ptr<Menu>& each : these)
                if (each->head_ != nullptr && offer(what, *each->head_)) return true;

            return true;
        }
    }
}

// The focus from before entering is remembered here, because every entry passes this way, by a
// key, a click or a side arrow. Moving from title to title or from a title to an item does not
// count: if a title were remembered, Esc would open the previous menu instead of returning to the
// application.
bool Menubar::handle(FocusEvent& what) {
    if (!ours(dynamic_cast<const Node*>(what.target()))) return false;

    Node* other = what.other();

    if (ours(other)) return false;

    was_ = other != nullptr ? other->weak_from_this() : std::weak_ptr<Node>{};

    return false;
}

// When the focus leaves the bar, the bar folds. Opening is attaching, so it has to be undone
// explicitly. The event goes up from the old focus, so it always arrives here, wherever the focus
// went, including a curtain that stands outside the bar.
bool Menubar::handle(UnfocusEvent& what) {
    if (ours(what.other())) return false;

    fold();

    expanded_ = false;

    return false;
}

}
