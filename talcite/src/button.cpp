#include "talcite/button.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "eliding.hpp"

#include "marking.hpp"

namespace talcite::ui {

namespace {

// The brackets around the label: a normal button has four cells of them, a primary one six, `[ Yes
// ]` and `[< Yes >]`, exactly as in Midnight Commander.
int around(bool primary) { return primary ? 6 : 4; }

}

// A button with the given label, drawn in the role `talcite.button`.
Button::Button(std::string text) : text_(std::move(text)) { role("talcite.button"); }

// A button has no children.
int Button::capacity() const { return 0; }

// The label's width plus the brackets, one row high, within the room.
Size Button::wants(Size room) const {
    const int wide = measured(text_).wide + around(primary_);

    return {std::min(wide, room.wide), std::min(1, room.high)};
}

// One row: the brackets, the label cut to fit between them, and the hot letter underlined.
View Button::paint(Size given, Area seen) {
    View made{seen.size};

    // Transparent where it draws nothing, the rule described at `Node::paint`. The button paints
    // one row, and a layout row gives it its own height, so without this the rest of the rectangle
    // would go to the parent as opaque spaces and cover its background.
    made.clear({{0, 0}, seen.size});

    const Skin& how = skin();

    // A role per state, not one role with markers glued on: a skin author wants to say what a
    // focused button looks like, not guess what the library will add to it. The role per state is
    // made of a stem and a suffix: whoever builds the button sets the stem, and the button adds the
    // suffix, because only it knows what states it has. So the skin describes the family `<stem>`,
    // `<stem>.focus`, `<stem>.key`, `<stem>.disabled` and does not have to guess anything.
    //
    // Gluing the name together allocates memory once per state and once per frame. With three
    // buttons in a window that is nothing, while keeping four ready strings in every button would
    // cost more and would need refreshing on every change of the stem.
    const Style look = how.style(!enabled()      ? role() + ".disabled"
                                      : focused() ? role() + ".focus"
                                                  : role());

    // One row, not as many as given. The button reports a height of one in `wants()`, but a row
    // gives its children its own height; without this the reverse video would spill over the whole
    // row.
    made.fill({{-seen.at.x, -seen.at.y}, {given.wide, 1}}, look);

    const std::string first = primary_ ? "[< " : "[ ";
    const std::string after = primary_ ? " >]" : " ]";

    const int aside = measured(first).wide;
    const int apart = measured(after).wide;

    // A button that is too narrow cuts the label, not the closing bracket. The bracket is what
    // makes it a button; cut off, it would leave behind something that looks like plain text.
    const int room = std::max(given.wide - aside - apart, 0);

    const std::string shown =
        elided(text_, room, Elide::right, how.glyph("talcite.elide"));

    const int inner = measured(shown).wide;

    made.put({-seen.at.x, -seen.at.y}, first, look);
    made.put({aside - seen.at.x, -seen.at.y}, shown, look);
    made.put({aside + inner - seen.at.x, -seen.at.y}, after, look);

    // The marking is computed on the clipped label: a letter that is not visible after clipping has
    // nothing to underline, and computed on the whole label it would land on the ellipsis or the
    // bracket. The hot letter has its own role, laid over the button's look, so a skin can give it
    // a color, while the built-in skin stays with an underline, which shows on every terminal. A
    // disabled button marks no letter: the key does nothing, and a lit letter would make the dimmed
    // button look half alive.
    if (enabled())
        talcite::ui::marking(made, {aside - seen.at.x, -seen.at.y}, shown, shortcut_column(shown),
                          look, how.style(role() + ".key"));

    return made;
}

// Presses the button, unless it is disabled. A press goes through `trigger`, not straight to a
// callback. This way one event is seen both by the widget itself and by everything above it, and
// `on<PickEvent>` catches the press regardless of what caused it: Enter, the mouse or a hot key. A
// callback would bypass the whole mechanism and be the only place in talcite where the event cannot
// be seen.
void Button::pick() {
    if (!enabled()) return;

    (void)trigger(PickEvent{});
}

// Enter presses the button that has the focus, as in Midnight Commander, regardless of hot keys.
// The dialog takes care of the primary button and of hot keys from anywhere in the window, because
// those are the window's behaviors, not the button's.
//
// Nothing is caught with a modifier: Ctrl+Enter and Alt+space are meant as other shortcuts. The
// key bar filters them the same way.
bool Button::handle(KeyEvent& what) {
    if (!enabled()) return false;

    if (what.mods().ctrl || what.mods().alt) return false;

    // Enter alone, without space. Space is a reflex from a world other than Midnight Commander, and
    // every key a widget takes is one the application cannot use; in a window with a text field,
    // space belongs to typing anyway.
    if (what.key().code() != Code::enter) return false;

    pick();

    return true;
}

// A hot key comes from the base, which has checked that it is ours. What remains is to say what we
// do, and the button does the same as on Enter and on a click.
void Button::activate() { pick(); }

// A click on the button takes the focus, unlike a key bar item, which leaves it alone. There what
// mattered was the selection in the panel; here the clicked button becomes the one you stand on.
bool Button::handle(ClickEvent&) {
    if (!enabled()) return true;

    if (root() != nullptr) focus();

    pick();

    return true;
}

}
