#include "talcite/status.hpp"

#include <algorithm>
#include <utility>

namespace talcite::ui {

// A status line: a text that a window changes as things happen (what is missing, what a row under
// the cursor holds, how many entries pass) and that keeps its row when it has nothing to say. An
// empty `Span` takes no room, which is right for a label, but a status line that vanished with its
// text would make the window jump each time the text came back; windows used to hold the row with a
// space for text. The line is its own class rather than a switch on `Span`, so a sketch of the
// window says what it is: `<Status>`.
Status::Status(std::string text, SpanShape shape) : Span(std::move(text), shape) {}

// What the text wants, and at least one row while the room has one, also with no text at all.
Size Status::wants(Size room) const {
    const Size made = Span::wants(room);

    return {made.wide, std::max(made.high, std::min(1, std::max(room.high, 0)))};
}

}
