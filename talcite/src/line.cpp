#include "talcite/line.hpp"

#include <algorithm>
#include <utility>

#include "talcite/skin.hpp"
#include "lining.hpp"
#include "titling.hpp"

namespace talcite::ui {

// A separator line with an optional title. Its role works as for a frame, because a separator is
// a piece of a frame: the line takes the style of `role()`, the text that of `role() + ".title"`.
// The default is "talcite.frame", so without a role it looks like any frame; with a role it looks
// like its own frame, for example the line above the status row in an active and an inactive
// panel.
Line::Line(std::string text, Border border) : text_(std::move(text)), border_(border) {
    role("talcite.frame");
}

// A separator holds no children.
int Line::capacity() const { return 0; }

// A separator does not push its parent wider. It asks for the width of its text alone, and for
// zero without a text, just like the line in a menu and for the same reason: it is not the line
// that decides how wide its window is. The column stretches it, because it gives every child its
// whole width, not only what the child asked for. The height is always one row: a separator that
// wraps is not a separator.
Size Line::wants(Size room) const {
    const int wide = text_.empty() ? 0 : 2 + measured(text_).wide;

    return {std::min(wide, room.wide), std::min(1, room.high)};
}

// Paints the line across the whole width, with the title placed in it.
View Line::paint(Size given, Area seen) {
    View made{seen.size};

    // Transparent where nothing is drawn, the rule described at `Node::paint`.

    made.clear({{0, 0}, seen.size});

    const Skin& how = skin();

    const Style look = how.style(role());

    if (border_ != Border::none) {
        const Lines glyphs = how.lines(border_);

        across(made, {-seen.at.x, -seen.at.y}, given.wide, glyphs.across, look);
    }

    // The text is drawn by the same function that puts a frame's title into its edge, because it
    // is the same thing.
    along(made, {-seen.at.x, -seen.at.y}, given.wide, text_, justify_,
          how.style(role() + ".title"), how.glyph("talcite.elide"));

    return made;
}

}
