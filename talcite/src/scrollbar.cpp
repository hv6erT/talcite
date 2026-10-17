#include "scrollbar.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

#include "talcite/node.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

namespace talcite::ui {

// A scrollbar is one cell wide: a thin line for the track and a solid thumb on it, with no arrows
// at the ends. The thumb says two things at once, where we are and how much of the whole fits in
// the window, because its length is the same fraction of the track. The bar of a widget with the
// focus (or, for a scroller, with the focus inside it) takes the skin's `.focus` styles, so the
// eye sees at once which of two bars the keys move.
//
// This is an internal header: the library's API does not draw bars, the widgets do. The name of the
// bar's role stays public in the widgets (`scrollbar_role`), because that is what reaches the skin.
//
// `Scrollbar` is one bar as a single object. The caller fills the fields by name and calls `draw`:
//
//     Scrollbar{.at = Scrollbar::edge(given, seen, true), .much = given.high, .shift = shift_,
//               .whole = rows, .window = given.high, .focused = focused(), .skin = skin(),
//               .role = role}.draw(made);
//
// `at` is the bar's first cell in the coordinates of the view being painted (the top one for a
// vertical bar, the left one for a horizontal bar); `much` is the bar's length in cells; `shift`,
// `whole` and `window` are the scroll offset, the length of the whole and the length of the
// window, in any units as long as they are the same: rows, columns or bytes. Both references
// (`skin`, `role`) live shorter than the expression that creates the bar: the object is not for
// keeping, only for one drawing. `Scrollbar::track` (inline in the header) copies the position and
// numbers into a `Track`.
//
// `Track` holds the position and numbers of one bar, all that is needed to draw it and to hit it
// with the mouse. It has no skin and no role, so it can be kept until the next frame.
//
// `Part` is the part of the bar under a cell: the track before and after the thumb (one page) or
// the thumb (dragging); a row at a time is the wheel's. `Asked` says what to do after a mouse event
// on the bar: the part the press hit and, while the thumb is dragged, the new offset in the bar's
// units.
//
// `Grip` is the mouse handling on a bar, shared by every scrolling widget: the table, the page, the
// list and the scroller each draw the bar their own way, but hitting, grabbing and dragging work
// the same everywhere, so they are computed once, here. After each paint the widget says where the
// bar ended up (in its own coordinates) and asks on each event what to do; it does the scrolling
// itself, in its own units. `Grip::over` (inline) tells whether a cell lies on the bar, and
// `Grip::held` whether the thumb is being dragged.
namespace {

// The bar computes a proportion, not counts. So it may divide all three numbers by a common
// divisor, as long as the whole fits in `int`; that is the only way a document of two billion
// rows or bytes can be drawn at all.
struct Scaled {
    int shift = 0;
    int whole = 0;
    int window = 0;
};

// The three numbers divided by the smallest power of two that brings `whole` within `int`.
Scaled scaled(std::size_t shift, std::size_t whole, std::size_t window) {
    constexpr auto brim = static_cast<std::size_t>(std::numeric_limits<int>::max());

    std::size_t by = 1;

    while (whole / by > brim) by *= 2;

    return {static_cast<int>(shift / by), static_cast<int>(whole / by),
            static_cast<int>(window / by)};
}

// The divisor `scaled` used to fit the numbers in `int`, needed in the other direction, when the
// thumb is dragged.
std::size_t divisor(std::size_t whole) {
    constexpr auto brim = static_cast<std::size_t>(std::numeric_limits<int>::max());

    std::size_t by = 1;

    while (whole / by > brim) by *= 2;

    return by;
}

// The bar's layout, computed in one place: drawing and the mouse must see the thumb in the same
// cells, otherwise a click on the drawn thumb would hit the track next to it.
struct Laid {
    int track = 0;
    int thumb = 0;
    int from = 0;
    int most = 0;
};

// The track length (the whole bar), the thumb length, the thumb's first cell on the track and the
// largest scaled offset.
Laid laid_out(const Track& one) {
    Laid made;

    made.track = one.much;

    if (made.track <= 0) return made;

    const Scaled small = scaled(one.shift, one.whole, one.window);

    made.most = std::max(small.whole - small.window, 0);

    // When the content fits in the window, the thumb fills the whole track. Otherwise we would
    // have to draw a bar that shows nothing and still takes space.
    //
    // Products are computed in 64 bits. Scaling fits each number in `int` on its own, but not its
    // product with the track length: for a three-billion-byte file scrolled halfway, `room * shift`
    // is several billion, and in `int` the thumb would land anywhere.
    const auto widened = [](int value) { return static_cast<std::int64_t>(value); };

    made.thumb = made.most == 0 ? made.track
                                : static_cast<int>(std::clamp<std::int64_t>(
                                      widened(made.track) * small.window / std::max(small.whole, 1), 1, made.track));

    const int room = made.track - made.thumb;

    made.from = made.most == 0 ? 0 : static_cast<int>((widened(room) * small.shift + made.most / 2) / made.most);

    return made;
}

}

// The bar's first cell at the edge, in the coordinates of the visible area `seen` of a widget of
// size `given`: on the right for a vertical bar, at the bottom for a horizontal one. Every scroller
// needs this, and it is exactly the kind of arithmetic that comes out one cell off in each copy.
Spot Scrollbar::edge(Size given, Area seen, bool upright) {
    return upright ? Spot{given.wide - 1 - seen.at.x, -seen.at.y}
                   : Spot{-seen.at.x, given.high - 1 - seen.at.y};
}

// Whether the focus of the owner's root rests on the owner or on a node inside it: a list hands
// the focus to its rows and a scroller to what it holds, and their bar is still the one the keys
// move.
bool Scrollbar::focused_in(const Node& owner) {
    const Root* top = owner.root();

    if (top == nullptr) return false;

    for (const Node* at = top->focused_node(); at != nullptr; at = at->parent())
        if (at == &owner) return true;

    return false;
}

// Draws the track and the thumb into `into`, with the glyphs of `role` and its .track.upright or
// .track.across and .thumb variants, and the styles of `role` and `role`.thumb, or of
// `role`.focus and `role`.thumb.focus when the bar's widget has the focus.
void Scrollbar::draw(View& into) const {
    if (much <= 0) return;

    // The bar asks the skin itself, because it is not a node and has no ancestor through which
    // to reach it. The caller passes it from `Node::skin()`: one reference instead of two styles in
    // the fields.
    const std::string state = focused ? ".focus" : "";

    const Style look = skin.style(role + state);
    const Style full = skin.style(role + ".thumb" + state);

    const char32_t empty = skin.glyph(role + (upright ? ".track.upright" : ".track.across"));
    const char32_t block = skin.glyph(role + ".thumb");

    const Laid laid = laid_out(track());

    for (int which = 0; which < laid.track; ++which) {
        const bool kept = which >= laid.from && which < laid.from + laid.thumb;
        const Spot spot = upright ? Spot{at.x, at.y + which} : Spot{at.x + which, at.y};

        // The bar lies on its widget's ground: a style without a background takes the one below.
        into.foreground(spot, kept ? block : empty, kept ? full : look);
    }
}

// Records the bar from the last frame, or its absence when the content fits without it. A missing
// bar does not release the grip: motion without a bar drags nothing anyway, and releasing the
// button removes the grip.
void Grip::laid(std::optional<Track> given) { track_ = given; }

// The part of the bar under the cell `here` (in the widget's coordinates); `Part::none` off the bar
// or when there is no bar.
Part Grip::part(Spot here) const {
    if (!track_.has_value() || track_->much <= 0) return Part::none;

    const Track& one = *track_;

    const int across = one.upright ? here.x - one.at.x : here.y - one.at.y;
    const int along = one.upright ? here.y - one.at.y : here.x - one.at.x;

    if (across != 0 || along < 0 || along >= one.much) return Part::none;

    const Laid laid = laid_out(one);

    if (along < laid.from) return Part::before;

    if (along >= laid.from + laid.thumb) return Part::after;

    return Part::thumb;
}

// A left press on the bar says what it hit; on the thumb it remembers which of the thumb's cells
// was grabbed and asks the root to capture the mouse, so that motion keeps arriving even when the
// pointer leaves the bar, as when dragging a window. Motion while grabbed yields the offset at
// which the same cell of the thumb stays under the pointer; the track is computed in the same
// proportions as for drawing, and the result returns to the bar's units (rows, bytes) through the
// scaling divisor. Releasing the button ends the grip.
Asked Grip::mouse(const MouseEvent& what, Spot here, Node& owner) {
    if (what.press() == Press::up) {
        const bool was = grip_.has_value();

        grip_.reset();

        return Asked{was ? Part::thumb : Part::none, std::nullopt};
    }

    if (what.press() == Press::move) {
        if (!grip_.has_value() || !track_.has_value()) return {};

        const Track& one = *track_;
        const Laid laid = laid_out(one);
        const int room = laid.track - laid.thumb;

        if (room <= 0 || laid.most == 0) return Asked{Part::thumb, std::nullopt};

        const int along = one.upright ? here.y - one.at.y : here.x - one.at.x;
        const int cell = std::clamp(along - *grip_, 0, room);

        const auto scaled_to = static_cast<std::size_t>(
            (static_cast<std::int64_t>(cell) * laid.most + room / 2) / room);

        // The cell does not leave the track, so the result does not go past the last offset, also
        // after multiplying by the scaling divisor it was divided by before.
        return Asked{Part::thumb, scaled_to * divisor(one.whole)};
    }

    grip_.reset();

    if (what.button() != Mouse::left) return {};

    const Part hit = part(here);

    if (hit == Part::thumb && track_.has_value()) {
        const int along = track_->upright ? here.y - track_->at.y : here.x - track_->at.x;

        grip_ = along - laid_out(*track_).from;

        if (Root* top = owner.root(); top != nullptr) top->capture(owner);
    }

    return Asked{hit, std::nullopt};
}

}
