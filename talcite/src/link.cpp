#include "talcite/link.hpp"

#include <algorithm>
#include <utility>

#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "eliding.hpp"

// A link: a line of text that stands for an address. It is drawn in the role `talcite.link`, with
// `.focus` and `.disabled` beside it, and its cells carry a hyperlink (`View::link`), which goes to
// a terminal that takes them as OSC 8 (`Capabilities::links`): such a terminal shows the address
// under the pointer and opens it itself on Ctrl+click (Cmd+click on macOS).
//
// Enter, or a click, follows the link the way every widget of the library follows an address
// (`FollowingMixin`): `FollowEvent` first, which the application may take, then the launcher, and
// `FollowedEvent` with its answer.
//
// A click with Ctrl is not ours. A terminal that takes hyperlinks opens the link itself on
// Ctrl+click and still passes the click on to the program (Windows Terminal and iTerm2 do), so
// following it here too would open the address twice. A terminal without hyperlinks leaves the
// plain click, which follows the link, and Enter.
namespace talcite::ui {

// A link with its text and address, opened through `with`, or through the system's launcher when
// it is not given. Empty text shows the address itself.
Link::Link(std::string text, std::string address, Seam<Launcher> with)
    : text_(std::move(text)), target_(hyperlink(std::move(address))) {
    role("talcite.link");

    if (with.bound()) launcher(std::move(with));
}

// A new address takes its own hyperlink: under the old mark the cells would not change, and the
// terminal would keep the old address (`hyperlink`).
void Link::address(std::string given) { target_ = hyperlink(std::move(given)); }

// A link has no children.
int Link::capacity() const { return 0; }

// What the link shows: its text, or its address when it has no text.
const std::string& Link::shown() const { return text_.empty() ? target_.address : text_; }

// The width of what it shows, one row high, within the room.
Size Link::wants(Size room) const {
    return {std::min(measured(shown()).wide, room.wide), std::min(1, room.high)};
}

// One row: the text cut to the width with the skin's ellipsis, in the role of its state, and over
// exactly the cells of the text the hyperlink. The rest of the rectangle stays transparent, so that
// a link in a row of text does not paint the row. A disabled link carries no hyperlink, otherwise
// the terminal would open what the program says cannot be used.
View Link::paint(Size given, Area seen) {
    View made{seen.size};

    made.clear({{0, 0}, seen.size});

    const Skin& how = skin();

    const Style look = how.style(!enabled() ? role() + ".disabled" : focused() ? role() + ".focus" : role());

    const std::string text = elided(shown(), given.wide, Elide::right, how.glyph("talcite.elide"));

    const Spot at{-seen.at.x, -seen.at.y};

    made.put(at, text, look);

    if (enabled()) made.link({at, {measured(text).wide, 1}}, target_);

    return made;
}

// Follows the link, unless it is disabled (`FollowingMixin::followed`; a link without an address
// follows nowhere).
void Link::follow() {
    if (!enabled()) return;

    followed(target_.address);
}

// Enter follows the link that has the focus, as it presses a button. Nothing is caught with a
// modifier: those are meant as other shortcuts.
bool Link::handle(KeyEvent& what) {
    if (!enabled()) return false;

    if (what.mods().ctrl || what.mods().alt) return false;

    if (what.key().code() != Code::enter) return false;

    follow();

    return true;
}

// A click with the left button takes the focus and follows the link; with Ctrl it is the
// terminal's (see the head of this file), and another button is not a click on a link.
bool Link::handle(ClickEvent& what) {
    if (!enabled()) return true;

    if (what.mods().ctrl || what.button() != Mouse::left) return false;

    if (root() != nullptr) focus();

    follow();

    return true;
}

}
