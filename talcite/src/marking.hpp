#pragma once

#include <string>
#include <string_view>

#include "talcite/area.hpp"
#include "talcite/encoding.hpp"
#include "talcite/shape.hpp"
#include "talcite/view.hpp"

namespace talcite::ui {

// Repaints the shortcut letter in a label that is already in the view. The free function
// `shortcut_column` gives the column; what remains here is finding the character that sits in
// that column and repainting it with a different style.
//
// The split into two parts is deliberate: the column must be known before drawing, because it may
// be needed to lay out the label, while repainting can only happen after. The widget knows anyway
// by how many cells it shifted the label from the left, so it passes the offset in `at`.
//
// It is shared by the button, the menu item, the checkbox, the key bar and the menu bar, which
// would otherwise each repeat the same loop over the label's characters.
//
// We walk by clusters and cells, not bytes, because the label is UTF-8, wide characters take two
// cells and a letter with its accent is one character to repaint. The
// label must be the same one that went into the view, that is, already trimmed: a column counted
// over the whole label would land on the ellipsis.
//
// The shortcut letter changes the foreground and attributes, never the background. The .key role
// is merged into the label's style without its `background`: skins ported from Midnight Commander
// have a foreground-background pair in .key (there menuhot and dhotnormal are fg;bg), while MC has
// a separate role for the letter in a highlighted item (menuhotsel, dhotfocus) that we do not
// have. With the background from .key, the letter in a highlighted title would stand on the bar's
// background like a hole in the highlight, since in each of the 29 skins the .key background
// equals the background of a normal item. So the background is whatever the letter stands on;
// the foreground and bold or underline come from .key.
inline void marking(View& into, Spot at, const std::string& shown, int which, Style look,
                    Style key) {
    if (which < 0) return;

    key.background = Color{};

    const Style how = Style::merge(look, key);

    std::size_t where = 0;
    int wide = 0;

    while (where < shown.size()) {
        const std::size_t next = cluster_after(shown, where);
        const std::string_view one = std::string_view{shown}.substr(where, next - where);

        if (wide == which) {
            into.put({at.x + which, at.y}, one, how);

            return;
        }

        wide += width(one);
        where = next;
    }
}

}
