#include "talcite/scroll.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

#include "talcite/skin.hpp"

#include "followed.hpp"
#include "scrollbar.hpp"

namespace talcite::ui {

namespace {

// Three rows per wheel notch: what most terminals do and what the eye can follow.
constexpr int stepping = 3;

// Which scrollbars to show, as {column on the right, row at the bottom}: always, or only when
// needed (`needed`). A bar "when needed" appears on an axis where the content does not fit in the
// window. The content is first asked without bars; once one bar appears, the other axis is checked
// again, because a vertical bar takes a column, so wrapped content may grow and only then fail to fit
// horizontally (and the other way around). Two passes are enough: there are two bars and neither
// disappears once it has appeared.
std::pair<int, int> bars(const ScrollShape& shape, bool across, bool down, const Node& child, Size room) {
    if (!shape.scrollbar) return {0, 0};

    int aside = down && room.wide > 0 ? 1 : 0;
    int below = across && room.high > 0 ? 1 : 0;

    if (!shape.needed) return {aside, below};

    const bool vertical = aside != 0;
    const bool horizontal = below != 0;

    aside = 0;
    below = 0;

    for (int pass = 0; pass < 2; ++pass) {
        const Size inner{room.wide - aside, room.high - below};
        const Size wants = child.wants({across ? plenty : inner.wide, down ? plenty : inner.high});

        if (vertical && wants.high > inner.high) aside = 1;
        if (horizontal && wants.wide > inner.wide) below = 1;
    }

    return {aside, below};
}


// The bar along the right edge (`upright`) or the bottom one of a scroller of size `given`, in its
// coordinates: the window without bars, the whole content and the offset say where the thumb is.
Scrollbar barred(Size given, bool upright, Size window, Size whole, Spot shift, bool focused, const Skin& skin,
                 const std::string& role) {
    return Scrollbar{.at = Scrollbar::edge(given, {{0, 0}, given}, upright),
                     .upright = upright,
                     .much = upright ? window.high : window.wide,
                     .shift = static_cast<std::size_t>(upright ? shift.y : shift.x),
                     .whole = static_cast<std::size_t>(upright ? whole.high : whole.wide),
                     .window = static_cast<std::size_t>(upright ? window.high : window.wide),
                     .focused = focused,
                     .skin = skin,
                     .role = role};
}
}

// One scroller for three axis choices. The axes differ only in which dimension the child gets
// fitted and which one free. An open axis (`across` horizontally, `down` vertically) asks the child
// for `plenty`, a closed one for what is there; on a closed axis the content is exactly as large as
// the window, so the scroll offset along it always comes out zero and needs no special handling.

Scroll::Scroll(Axis axis, ScrollShape shape)
    : axis_(axis), shape_(shape), upright_(std::make_shared<Grip>()), sideways_(std::make_shared<Grip>()) {}

// Sets the scroll offset (negative parts become zero) and sends a `ScrollEvent` only on a change,
// and only from here. Clamping to the current window happens in `paint`, so notifying on every
// change of `shift_` would mean replacing the tree while a frame is being composed. The known and
// accepted consequence: narrowing the window, which moves the content by itself, notifies nobody.
// The component of a closed axis is ignored. The event says how the move came: the wheel and the
// bars pass `Moved::by_hand`, a call from the program leaves the default.
void Scroll::shift(Spot given, Moved how) {
    const Spot want{across() ? std::max(given.x, 0) : 0, down() ? std::max(given.y, 0) : 0};

    if (want == shift_) return;

    shift_ = want;

    changed();

    (void)trigger(ScrollEvent{how});
}

// The size the scroller wants: the child's size plus the bars, but at most `room`. We ask for
// `plenty` minus the bar: a child that returns what it was given (a panel, a veil, a window) would
// otherwise return `plenty`, and adding one to that overflows `int`, so the size would come out
// negative and the scroller would vanish from the screen. We return at most what we were given:
// the parent has nowhere to take more from. A label that would wrap in a narrow window stays one
// long row in a horizontally open scroller, which is exactly what is expected of it.
Size Scroll::wants(Size room) const {
    if (children().empty()) return {};

    const auto [aside, below] = bars(shape_, across(), down(), *children().front(), room);

    const Size wants = children().front()->wants(
        {across() ? plenty - aside : room.wide - aside, down() ? plenty - below : room.high - below});

    return {std::min(wants.wide + aside, room.wide), std::min(wants.high + below, room.high)};
}



// Lays the child out in a rectangle shifted by the scroll offset and as large as it wanted on the
// open axes. Only its intersection with the window without bars can be seen, which is what it is
// told, so that it lays out and paints only that part.
//
// Following the focus happens here, after the child is laid out, because only then is the
// rectangle of the node that took the focus known. When it lies outside the window, we move the
// offset and lay the content out again: once per focus change, not per frame, and with nothing
// painted yet. The wheel moves the view and leaves the focus alone, so the two do not fight over
// `shift_`. No notification, as with clamping: a frame in progress is no time to replace the tree.
// On a closed axis `followed` clamps to zero.
void Scroll::layout(Size given) {
    if (children().empty()) return;

    Node& one = *children().front();

    // The vertical bar takes the rightmost column, the horizontal one the bottom row. The corner
    // between them stays empty, because it belongs to neither.
    const auto [aside, below] = bars(shape_, across(), down(), one, given);

    window_ = {given.wide - aside, given.high - below};

    const Size wants = one.wants({across() ? plenty : window_.wide, down() ? plenty : window_.high});

    whole_ = {across() ? std::max(wants.wide, 0) : window_.wide,
              down() ? std::max(wants.high, 0) : window_.high};

    // The content may have shrunk between frames; the offset would then point past its end.
    shift_ = {std::clamp(shift_.x, 0, std::max(whole_.wide - window_.wide, 0)),
              std::clamp(shift_.y, 0, std::max(whole_.high - window_.high, 0))};

    const Area room{{0, 0}, window_};

    one.arrange({{-shift_.x, -shift_.y}, whole_}, room);

    if (follow_) {
        follow_ = false;

        if (const Node* held = beheld(*this); held != nullptr)
            for (int pass = 0; pass < passes; ++pass) {
                const std::optional<Area> box = held->area_in(one);

                if (!box.has_value()) break;

                const Spot want{followed(shift_.x, box->at.x, box->size.wide, window_.wide, whole_.wide),
                                followed(shift_.y, box->at.y, box->size.high, window_.high, whole_.high)};

                if (want == shift_) break;

                shift_ = want;

                one.arrange({{-shift_.x, -shift_.y}, whole_}, room);
            }
    }

    upright_->laid(std::nullopt);
    sideways_->laid(std::nullopt);

    const bool focused = Scrollbar::focused_in(*this);

    if (aside != 0)
        upright_->laid(barred(given, true, window_, whole_, shift_, focused, skin(), scrollbar_role_).track());

    if (below != 0)
        sideways_->laid(barred(given, false, window_, whole_, shift_, focused, skin(), scrollbar_role_).track());
}

// Paints the visible part of the child in the window without bars, and the bars.
void Scroll::paint(Canvas& onto) const {
    if (children().empty()) return;

    Canvas window = onto.clipped({{0, 0}, window_});

    children().front()->render(window);

    const Size given = onto.size();

    const bool focused = Scrollbar::focused_in(*this);

    if (given.wide > window_.wide)
        barred(given, true, window_, whole_, shift_, focused, skin(), scrollbar_role_).draw(onto);

    if (given.high > window_.high)
        barred(given, false, window_, whole_, shift_, focused, skin(), scrollbar_role_).draw(onto);
}

// One of the descendants took the focus (the event bubbles up), so the next frame has to follow it.
// We keep no pointer: the node may die before the frame, and `Root` knows who has the focus anyway.
// The event is not consumed, so that parents see it too (a scroller inside a scroller).
bool Scroll::handle(FocusEvent&) {
    follow_ = true;

    return false;
}

// The wheel scrolls vertically, and horizontally with Shift: most programs do this, and it is the
// only way for a mouse with a single wheel. A horizontal-only scroller scrolls horizontally with
// the wheel, without Shift. The tilted wheel (a touchpad's sideways swipe) scrolls horizontally
// wherever there is a horizontal axis, and a vertical-only scroller passes it on. At the end of the
// range we return false, so the event goes up: a scroller inside a scroller passes the motion on
// instead of swallowing it.
bool Scroll::handle(WheelEvent& what) {
    const Spot was = shift_;

    const bool tilted = what.axis() == Axis::horizontal;

    if (tilted && axis_ == Axis::vertical) return false;

    const bool sideways = tilted || axis_ == Axis::horizontal || (axis_ == Axis::both && what.mods().shift);

    if (sideways)
        shift_.x = std::clamp(shift_.x - what.steps() * stepping, 0,
                              std::max(whole_.wide - window_.wide, 0));
    else
        shift_.y = std::clamp(shift_.y - what.steps() * stepping, 0,
                              std::max(whole_.high - window_.high, 0));

    if (shift_ == was) return false;

    changed();

    (void)trigger(ScrollEvent{Moved::by_hand});

    return true;
}

// The mouse on a scrollbar, with a separate grip for each axis: the track moves by one window, the
// thumb is dragged. While a grip is held, motion goes to the bar that was grabbed, wherever the
// pointer is.
bool Scroll::handle(MouseEvent& what) {
    const Spot here = local(what.where());

    const bool sideways = sideways_->held() || (!upright_->held() && sideways_->over(here));

    Grip& grip = sideways ? *sideways_ : *upright_;

    const Asked asked = grip.mouse(what, here, *this);

    const int window = sideways ? window_.wide : window_.high;
    const int now = sideways ? shift_.x : shift_.y;

    const auto moved = [this, sideways](int to) {
        const int most = sideways ? std::max(whole_.wide - window_.wide, 0) : std::max(whole_.high - window_.high, 0);
        const int fitted = std::clamp(to, 0, most);

        shift(sideways ? Spot{fitted, shift_.y} : Spot{shift_.x, fitted}, Moved::by_hand);
    };

    switch (asked.part) {
        case Part::none: return false;
        case Part::before: moved(now - std::max(window, 1)); break;
        case Part::after: moved(now + std::max(window, 1)); break;
        case Part::thumb:
            if (asked.to.has_value()) moved(static_cast<int>(*asked.to));
            break;
    }

    return true;
}

}
