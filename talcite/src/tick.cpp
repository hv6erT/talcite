#include "talcite/tick.hpp"

#include <algorithm>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "eliding.hpp"

#include "marking.hpp"

namespace talcite::ui {

namespace {

// The bracket with the mark and the space after it: `[x] `. Always four cells.
constexpr int around = 4;

}

// A check box with the given label and initial state, styled under the role `talcite.tick`.
Tick::Tick(std::string text, bool on) : text_(std::move(text)), on_(on) { role("talcite.tick"); }

// Setting the state from the program does not notify. Otherwise filling a window with its initial
// values would fire everything connected to those values, and that is filling in, not pressing.
void Tick::ticked(bool given) { on_ = given; }

// Flips the state and notifies. Space, a click and the shortcut key all go through here.
void Tick::pick() {
    if (!enabled()) return;

    on_ = !on_;

    (void)trigger(PickEvent{});
}

// A check box holds no children.
int Tick::capacity() const { return 0; }

// One row, not as many as offered, just like a button. A layout row gives its children its own
// height, and without this the focus highlight would spread over all of it.
Size Tick::wants(Size room) const {
    return {std::min(measured(text_).wide + around, room.wide), std::min(1, room.high)};
}

// Draws the bracket with the mark, then the label elided to the room left, with the shortcut
// letter underlined.
View Tick::paint(Size given, Area seen) {
    View made{seen.size};

    // Transparent wherever nothing is drawn: the same rule and the same reason as in the button.
    made.clear({{0, 0}, seen.size});

    const Skin& how = skin();

    // The role for the state is built from a stem and a suffix, the same contract as in the
    // button, so that a skin author describes the family `<stem>`, `<stem>.focus`, `<stem>.off`
    // and has nothing to guess.
    const Style look = how.style(!enabled() ? role() + ".disabled" : focused() ? role() + ".focus" : role());

    made.fill({{-seen.at.x, -seen.at.y}, {given.wide, 1}}, look);

    const std::string first = on_ ? "[x] " : "[ ] ";

    // A field that is too narrow cuts the label, not the bracket. The bracket is what makes this a
    // check box; cut off, it would leave behind something that looks like plain text.
    const int room = std::max(given.wide - around, 0);

    made.put({-seen.at.x, -seen.at.y}, first, look);

    if (room <= 0) return made;

    const std::string shown =
        elided(text_, room, Elide::right, how.glyph("talcite.elide"));

    made.put({around - seen.at.x, -seen.at.y}, shown, look);

    // The marking is computed on the elided label: a letter that is cut off has nothing to
    // underline. A shortcut key without an underline would be a key nobody learns about, and in
    // Midnight Commander a check box does show its letter.
    talcite::ui::marking(made, {around - seen.at.x, -seen.at.y}, shown, shortcut_column(shown),
                      look, how.style(role() + ".key"));

    return made;
}

// Space flips the box, Enter goes up. In a dialog Enter belongs to the default button, so a check
// box that swallowed it would break confirming the dialog.
bool Tick::handle(KeyEvent& what) {
    if (!enabled()) return false;

    if (what.mods().ctrl || what.mods().alt) return false;

    if (what.key().letter() != U' ') return false;

    pick();

    return true;
}

// A click takes the focus (when the box is in a tree) and flips the box. A disabled box still
// consumes the click, so it does not fall through to what lies below.
bool Tick::handle(ClickEvent&) {
    if (!enabled()) return true;

    if (root() != nullptr) focus();

    pick();

    return true;
}

// The shortcut key comes from the base class, which has checked that it is ours. The box does the
// same as for space and a click: it flips and notifies.
void Tick::activate() { pick(); }

}
