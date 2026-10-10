#include "talcite/box.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace talcite::ui {

// A node that sets its size instead of negotiating it.
//
// The wish is a function, not a number. A number cannot say "at most eighty columns, but leave
// eight on each side", and that is an ordinary way of sizing a window, not an oddity. The function
// gets the room and computes, so a fixed size is a special case of it; the other way round is
// impossible. The cost is measured, not estimated: a `std::function` with the function in a
// separate translation unit costs as much as reading a field (the difference disappears in the
// noise over twenty million calls), and `wants` is called on the order of hundreds of times per
// frame.
//
// There is no second mode, because leaving out the `Box` provides it. "I want as much as the child
// wants" is the default `Node::wants`, so whoever wants that simply does not wrap. That is why
// there is no enumeration of modes here and no asking the child: `Box` does not ask it at all. If
// it did, it would be the same as a `Frame` without a border, and the set size would stop being
// set.
//
// The room is not the screen. The function gets what the parent left for it, and a `Modal`
// subtracts room for the shadow, two columns and a row, before it asks the child for its wish.
// "Leave sixteen" therefore means "sixteen of what I got", not "of the screen"; whoever ports a
// calculation made from the terminal width adds those two columns and that row in their own
// function. This needs no separate kinds of positioning: the difference is constant and known, and
// positioning is a separate question from size, which `Modal::seat` answers.
//
// This constructor takes a constant as a special case of the function: a second entry point, not a
// second state. There is still one field, so `wants` has nothing to branch on, and `Box{{68, 20}}`
// reads better than a closure returning two numbers; this will be the most common case.
//
// `plenty` needs no separate branch here: the wish goes on as it is, and the clipping in `wants`
// turns it into the whole room. Recognizing it in this place would be a second truth about what an
// unbounded size means.
Box::Box(Size given) : wish_([given](Size) { return given; }) {}

// A box whose size the function computes from the room. An empty function would throw
// std::bad_function_call from inside the layout, on the first frame and in a place that says
// nothing, so we reject it where it was made.
Box::Box(Wish wish) : wish_(std::move(wish)) {
    if (!wish_) throw std::logic_error("talcite::ui::Box: the wish is empty");
}

// The wish clipped to the room and to zero from below. The clipping stays here, even though the
// function could do it itself, because otherwise everyone who writes one would have to remember it,
// and forgetting would give the parent a wish larger than the room: a lie to whoever divides the
// room afterwards.
//
// Clipping to zero from below is not excess caution: the room is sometimes negative by itself,
// because a parent subtracts borders and margins from what it has. A negative wish should stop
// where it arose instead of traveling up and multiplying in the layout's calculations.
Size Box::wants(Size room) const {
    const Size wish = wish_(room);

    return {std::max(std::min(wish.wide, room.wide), 0),
            std::max(std::min(wish.high, room.high), 0)};
}

}
