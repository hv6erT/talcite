#include "talcite/viewer.hpp"

#include <memory>
#include <utility>

#include "spool.hpp"

namespace talcite::ui {

// The viewer is a page plus two ways for a document to come in. All the drawing, scrolling,
// marks, questions about bytes and intents waiting for a window live in `Page`; what stays here is
// only how the document arrives: bytes in memory or a byte source read in windows.
//
// The spool does not wait for bytes, so the viewer does not wait either: the two things that need
// a pass over the document, `End` (the line count) and a jump to a byte, leave an intent behind,
// and the spool wakes the page after every window, so the intent completes by itself.
Viewer::Viewer() = default;

Viewer::Viewer(ScrollShape shape) : Page(shape) {}

Viewer::~Viewer() = default;

// A document in memory: the spool counts the lines at once, so every value is known as soon as
// it is given. This is how the license built into the program comes in.
void Viewer::hold(Bytes given) { Page::hold(std::make_shared<Spool>(std::move(given))); }

// A byte source: the spool counts nothing in advance. The line count is unknown until we reach
// the end, and that is the moment the + disappears in Midnight Commander. `head` holds the bytes
// from the start that the caller has already read (the header window): with them the first frame
// has lines at once, without a blank until the first window from the pool, since a network file
// takes a while to load.
void Viewer::hold(Feed given, std::size_t whole, Bytes head) {
    Page::hold(std::make_shared<Spool>(std::move(given), whole, std::move(head)));
}

}
