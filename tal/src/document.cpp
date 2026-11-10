#include "tal/document.hpp"

#include <string>
#include <utility>
#include <vector>

#include "damage.hpp"

namespace tal {

namespace {

constexpr std::size_t most_damaged = 64;

}

// The root of a tree of nodes, with the built-in sheet and a screen of no size until `display`.
// The clock serves the timers that come with events.
Document::Document(Port<Clock> time) : time_(std::move(time)) { document_ = this; }

// Takes another sheet: every node is styled again and the whole screen painted again, since the
// glyphs may differ too.
void Document::sheet(StyleSheet given) {
    changing();

    sheet_ = std::move(given);

    restyle();
    damaged(Area{{0, 0}, screen_});
}

// Says what the document is shown on: the size of the screen in cells, its color level and
// whether it shows Unicode. A new size is a new surface, laid out and painted whole; a new level
// styles every node again; Unicode changes the glyphs, so everything is painted again.
void Document::display(Size screen, Colors colors, bool unicode) {
    if (screen == screen_ && colors == colors_ && unicode == unicode_) return;

    changing();

    if (screen != screen_) {
        screen_ = screen;
        surface_ = Surface{screen};

        relayout();
    }

    if (colors != colors_) {
        colors_ = colors;

        restyle();
    }

    unicode_ = unicode;

    damaged(Area{{0, 0}, screen_});
}

// Whether a frame would change anything: a node to style, measure or arrange, or a place to paint.
bool Document::due() const { return restyling_ || sizing_ || arranging_ || !damage_.empty(); }

// Makes a frame: styles the nodes that wait for it, measures and arranges from the root, skipping
// what has not changed, and paints the damaged places only, each node that meets one of them. The
// surface's damage is what this frame changed.
const Surface& Document::frame() {
    struct Ending {
        Document& one;

        ~Ending() { one.phase_ = Phase::idle; }
    } ending{*this};

    ++frames_;
    tally_ = Tally{};
    surface_.forget();

    phase_ = Phase::styling;

    if (restyling_) {
        restyling_ = false;
        styled(*this);
    }

    phase_ = Phase::laying;

    if (sizing_ || arranging_) {
        (void)measured(Limits{screen_, screen_});
        placed(Area{{0, 0}, screen_});
    }

    phase_ = Phase::painting;

    const std::vector<Area> parts = std::move(damage_);
    const Area screen{{0, 0}, screen_};

    damage_.clear();

    for (const Area& part : parts) painted(*this, Spot{0, 0}, screen, part.common(screen));

    return surface_;
}

// Notes that `part` of the screen must be painted again.
void Document::damaged(Area part) { damage::added(damage_, part, most_damaged); }

// Styles `one` and everything under it that waits for a style. A new box lays the node out again,
// a new look only paints it again.
void Document::styled(Node& one) {
    if (!one.styled_) {
        const std::vector<std::vector<std::string>> classes = one.lineage();
        const Style made = sheet_.style(Selection{classes, one.states_, {}}, colors_);

        ++tally_.styled;

        if (made.border != one.style_.border || made.padding != one.style_.padding || made.margin != one.style_.margin) {
            one.style_ = made;
            one.relayout();
        } else if (made.look != one.style_.look) {
            one.style_ = made;
            one.repaint();
        }

        one.styled_ = true;
    }

    for (Node& child : one.children_) styled(child);
}

// Paints `one`, whose parent stands at `origin` on the screen, where it meets `clip` (what its
// parent shows) and `part` (the damage being painted): its background and border, its content,
// then its children in order, the later on top. A node that does not meet them is skipped with
// everything under it, since children show only inside their parent.
void Document::painted(Node& one, Spot origin, Area clip, Area part) {
    if (!one.placed_) return;

    const Area box{{origin.x + one.area_.at.x, origin.y + one.area_.at.y}, one.area_.size};
    const Area seen = box.common(clip).common(part);

    if (seen.size.wide <= 0 || seen.size.high <= 0) return;

    Canvas whole = surface_.canvas(seen).within(Area{{box.at.x - seen.at.x, box.at.y - seen.at.y}, box.size});
    const Style& style = one.style_;
    const Area all{{0, 0}, box.size};
    const int line = style.border == Border::none ? 0 : 1;

    whole.fill(all, style.look);

    if (line != 0) whole.frame(all, sheet_.lines(style.border, unicode_), style.look);

    Canvas inside = whole.within(all.inner(Sides{style.padding.top + line, style.padding.right + line,
                                                 style.padding.bottom + line, style.padding.left + line}));

    one.paint(inside);

    ++tally_.painted;

    for (Node& child : one.children_) painted(child, box.at, box.common(clip), part);
}

}
