#include "talcite/gauge.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>

#include "talcite/loop.hpp"
#include "talcite/promise.hpp"
#include "talcite/skin.hpp"
#include "talcite/time.hpp"

namespace talcite::ui {

namespace {

// One tick per tenth of a second: the eye sees motion, and an idle program wakes ten times a
// second, and only while something is really spinning.
constexpr std::chrono::milliseconds pacing{100};

// The percentage takes four cells for "100%" and a fifth for the gap before the bar.
constexpr int telling = 5;

}

// A progress bar `wide` cells wide, drawn in the `talcite.gauge` role.
Gauge::Gauge(int wide, GaugeShape shape) : shape_(shape), wide_(wide) { role("talcite.gauge"); }

// A gauge holds no children.
int Gauge::capacity() const { return 0; }

// The caller gives the width, as with a field: a bar in a window is as wide as whoever builds the
// window decides. The height is always one row.
Size Gauge::wants(Size room) const {
    return {std::min(wide_, room.wide), std::min(1, room.high)};
}

// The fraction is clamped here, because this is the only way it enters the widget. The inverted
// comparison also catches NaN: it is neither greater than nor equal to zero, so it becomes zero.
void Gauge::part(double given) {
    if (!(given >= 0.0)) given = 0.0;

    given = std::min(given, 1.0);

    if (part_ == given) return;

    part_ = given;

    changed();
}

// Moves the indeterminate block one position on.
void Gauge::step() {
    ++at_;

    changed();
}

// Switches to indeterminate mode and back. The turn number grows with every change, so a tick sent
// before it does not revive a bar that has already stopped, and two switches on in a row do not
// start two chains at once.
void Gauge::busy(bool given) {
    if (busy_ == given) return;

    busy_ = given;
    at_ = 0;

    ++turn_;

    if (busy_) beating();
    else beat_.reset();

    changed();
}

// The bar spins by itself, on the library's clock. `later` measures the interval, `here()`
// moves the continuation to the loop's thread, and `Loop::post` inside the loop wakes the terminal
// and forces a frame.
//
// Two conditions must hold, otherwise nothing starts. Without a loop there is nowhere to move the
// continuation, and touching the node tree from a pool thread is a data race. Without an owner,
// that is when nobody holds the bar through a shared pointer, the weak pointer is expired at once
// and a tick would have nothing to wake. In a tree that does not happen, because `Children::add`
// takes a shared pointer, but a bar on the stack in a test looks exactly like that and may simply
// not move.
void Gauge::beating() {
    if (Loop::current() == nullptr) return;

    const std::weak_ptr<Node> weak = weak_from_this();

    if (weak.expired()) return;

    const int mine = turn_;

    // The promise has to be kept: the handle is its owner, and a dropped handle stops the beat. The
    // type is hidden behind shared_ptr<void> so that the widget's header does not pull in the whole
    // promise machinery.
    //
    // Replacing it inside the callback is safe: the running callback belongs to the promise's node,
    // not to the handle, so letting go of the handle only cancels what has not run yet.
    beat_ = std::make_shared<Promise<void>>(later(pacing).here().then([weak, mine] {
        const std::shared_ptr<Node> one = weak.lock();

        if (!one) return;

        Gauge& gauge = static_cast<Gauge&>(*one);

        if (!gauge.busy_ || gauge.turn_ != mine) return;

        gauge.step();
        gauge.beating();
    }));
}

// Draws the brackets, the bar (filled part or the bouncing block) and, when there is room, the
// percentage.
void Gauge::paint(Canvas& onto) const {
    const Size given = onto.size();

    if (given.wide <= 0 || given.high <= 0) return;

    const Skin& how = skin();

    const Style look = how.style(role());
    const Style full = how.style(role() + ".done");

    const char32_t block = how.glyph(role() + ".done");
    const char32_t empty = how.glyph(role() + ".rest");

    // When space is tight the percentage goes first and the brackets second: a one-cell bar still
    // says something, a bracket alone says nothing.
    const bool told = shape_.percent && given.wide >= telling + 3;
    const int room = told ? given.wide - telling : given.wide;

    const bool boxed = room >= 3;
    const int bar = boxed ? room - 2 : room;
    const int from = boxed ? 1 : 0;

    if (boxed) {
        onto.put({0, 0}, how.glyph(role() + ".left"), look);
        onto.put({room - 1, 0}, how.glyph(role() + ".right"),
                    look);
    }

    if (bar <= 0) return;

    // Indeterminate: a block travels back and forth. Bouncing instead of wrapping, because a jump
    // from the right edge to the left reads as a blink, not as motion.
    if (busy_) {
        const int much = std::max(bar / 5, 1);
        const int travel = bar - much;
        const int span = 2 * travel;
        const int which = span > 0 ? at_ % span : 0;
        const int at = which <= travel ? which : span - which;

        for (int x = 0; x < bar; ++x)
            onto.put({from + x, 0},
                        x >= at && x < at + much ? block : empty,
                        x >= at && x < at + much ? full : look);

        return;
    }

    // Full only when finished. We round, but neither 100 nor a full bar is shown until the fraction
    // is exactly one; since both numbers follow the same rule, the bar and the percentage never
    // disagree.
    int done = static_cast<int>(std::lround(part_ * bar));

    if (done >= bar && part_ < 1.0) done = bar - 1;

    for (int x = 0; x < bar; ++x)
        onto.put({from + x, 0}, x < done ? block : empty,
                    x < done ? full : look);

    if (!told) return;

    int shown = static_cast<int>(std::lround(part_ * 100.0));

    if (shown >= 100 && part_ < 1.0) shown = 99;

    const std::string said = std::to_string(shown) + "%";
    const Style text = how.style(role() + ".text");

    // The percentage stands on whatever the gauge stands on (a hint line, a window, a panel), and
    // only what lies below knows its background: without one of its own in the skin it takes that
    // one (`View::foreground`), as the scroll bar's track does; a background the skin gives wins.
    // Drawn as plain text, it would show the terminal's own background there instead.
    const int start = given.wide - static_cast<int>(said.size());

    for (std::size_t at = 0; at < said.size(); ++at)
        onto.foreground({start + static_cast<int>(at), 0}, static_cast<char32_t>(said[at]), text);
}

}
