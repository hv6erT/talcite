#include "talcite/page.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "eliding.hpp"
#include "scrollbar.hpp"
#include "spool.hpp"

namespace talcite::ui {

namespace {

// How many document lines we take from the roll at a time. With wrapping one document line gives
// several screen lines, so asking for exactly as many as the window is high would almost always
// ask for too many.
constexpr std::size_t batch = 32;

// How many lines one wheel step scrolls. The same number as in the scrollers.
constexpr int stepping = 3;

// The piece of a document line that occupies one screen row: the finished text and a column to
// byte map, along which the marks are overlaid.
struct Cut {
    std::string text;
    std::vector<std::size_t> bytes;
    std::size_t line = 0;

    // The byte just past the last character of the piece. The column map says where each
    // character starts but not where it ends, and a click past the end of a line has to land
    // exactly there.
    std::size_t upto = 0;
};

// Cuts a document line into screen rows, without deciding what is made of the pieces. The walker
// tells the taker only three things: a character occupies this many columns, a tab expands into
// this many spaces, a piece ends before this byte. From the same calls `parted` builds pieces for
// drawing, while `counted` only counts them, because scrolling counts pieces at every step and
// building the text and the column map just to take their number would be wasted work.
//
// Without wrapping there is one piece, whatever fits, and the rest simply does not exist for the
// view.
//
// We cut by columns, not by bytes or characters: a Chinese character occupies two cells and a
// combining character zero. Otherwise a line with one such character would run off the page or
// break off one cell too early.
template <typename Taker>
void walked(const std::string& what, int wide, bool wrap, Taker& taker) {
    int much = 0;

    // Whether anything stands in the current piece, including a zero-width character that takes no
    // column.
    bool any = false;
    bool cut = false;

    // How far the cutting got. Without wrapping the line breaks off at the edge and the remaining
    // bytes are simply not visible, so the tail must not claim the whole length of the line.
    std::size_t ended = what.size();

    const auto flushed = [&taker, &much, &any, &cut](std::size_t upto) {
        taker.flushed(upto);

        much = 0;
        any = false;
        cut = true;
    };

    for (std::size_t at = 0; at < what.size();) {
        const std::size_t was = at;
        const char32_t sign = utf8::decode(what, at);

        if (sign == U'\t') {
            const int upto = std::min(((much / tabbing) + 1) * tabbing, wide);

            taker.spaced(was, upto - much);

            much = upto;
            any = true;
        } else {
            const int room = std::max(width(sign), 0);

            if (much + room > wide) {
                if (!wrap) {
                    ended = was;

                    break;
                }

                // The piece ends before this character, because the character did not fit in it.
                flushed(was);
            }

            taker.lettered(sign, was, room);

            much += room;
            any = true;
        }

        if (much < wide) continue;

        if (!wrap) {
            ended = at;

            break;
        }

        flushed(at);
    }

    // The tail is added only when something is in it. A line whose length is an exact multiple of
    // the width ends on a piece boundary, and an empty tail after it would be an empty screen row
    // that the document does not have. An empty document line is a different matter: it must give
    // one piece, hence the second condition.
    if (any || !cut) taker.flushed(ended);
}

struct Builder {
    std::vector<Cut> made;
    Cut one;

    // Adds `much` spaces of an expanded tab, each column pointing at the byte of the tab.
    void spaced(std::size_t was, int much) {
        for (int step = 0; step < much; ++step) {
            one.text.push_back(' ');
            one.bytes.push_back(was);
        }
    }

    // Adds a character. Every occupied column points at the byte of the character that occupies
    // it, including the second column of a double-width character and every column of an expanded
    // tab. Without that there would be no way to tell whether a cell falls within a mark's range.
    void lettered(char32_t sign, std::size_t was, int room) {
        utf8::encode(sign, one.text);

        for (int step = 0; step < room; ++step) one.bytes.push_back(was);
    }

    // Closes the current piece at byte `upto` and starts a new one.
    void flushed(std::size_t upto) {
        one.upto = upto;

        made.push_back(std::move(one));
        one = Cut{};
    }
};

struct Counter {
    int made = 0;

    void spaced(std::size_t, int) {}

    void lettered(char32_t, std::size_t, int) {}

    // Counts one finished piece.
    void flushed(std::size_t) { ++made; }
};

// The screen pieces of one document line, ready for drawing.
std::vector<Cut> parted(const std::string& what, int wide, bool wrap) {
    Builder builder;

    walked(what, wide, wrap, builder);

    return std::move(builder.made);
}

// How many screen rows one document line takes when wrapped at `wide` columns.
int counted(const std::string& what, int wide) {
    Counter counter;

    walked(what, wide, true, counter);

    return counter.made;
}

}

struct Page::Kept {
    Roll roll;
    std::optional<std::size_t> told;

    // The byte document. Empty with a line source, because then the bytes are not known at all.
    std::shared_ptr<Spool> spool;

    // Intentions waiting for a spool window: "go to the end" and "stand on the line with this
    // byte". They live until they are carried out or until a new document arrives.
    bool ending = false;
    std::optional<std::size_t> sought;

    std::size_t top = 0;
    int part = 0;

    std::vector<Cut> shown;
    std::size_t walked = 0;

    // Whether there is still content past the last shown piece: the rest of the line or the next
    // line. A line the source does not have yet counts as content.
    bool more = false;

    std::optional<std::size_t> which;

    // A line that has to become visible, while it is not known where yet, because the page does not
    // know its height.
    std::optional<std::size_t> wish;

    Size sized;
    bool wrapped = true;
    bool fresh = true;

    ScrollShape shape;

    std::string scrollbar_role = "talcite.scroll";
    std::string chosen = "talcite.viewer.mark.focus";

    // The scrollbar from the last frame and its unit: lines or bytes (see `gauged`).
    Grip grip;
    bool bytes = false;
};

// A page with the default scroll shape and the "talcite.viewer" role.
Page::Page() : kept_(std::make_unique<Kept>()) { role("talcite.viewer"); }

// A page with the given scroll shape and the "talcite.viewer" role.
Page::Page(ScrollShape shape) : kept_(std::make_unique<Kept>()) {
    role("talcite.viewer");

    kept_->shape = shape;
}

Page::~Page() = default;

// Shows a byte document read through a spool. The page holds the document, not the subclass.
// There are two kinds: a byte document, a spool from which the page knows lines, bytes and length,
// and a pure line document, which does not know bytes at all; then `at()` and `whole()` are empty,
// meaning "there is nothing to say", not "broken". A subclass only supplies the document: `Viewer`
// bytes or a byte source, `Editor` a spool over a quilt. A new document replaces the previous one
// entirely, because there is only one place where the document lives.
//
// The spool wakes the page after every window: the frame is invalidated, because the page
// remembers what it showed, and the intentions waiting for a window (the end and the jump to a
// byte) complete by themselves. The callback holds the page by a raw pointer, because the page
// holds the spool; for the editor's spool it is never called, because that spool reads from memory
// and waits for nothing.
void Page::hold(std::shared_ptr<Spool> given) {
    Kept& kept = *kept_;

    hold([given](std::size_t which, std::size_t most) { return given->lines(which, most); }, 0);

    kept.spool = std::move(given);

    kept.spool->awoken([this] {
        Kept& now = *kept_;

        invalidate();

        if (now.sought.has_value()) {
            const std::size_t want = *now.sought;

            now.sought.reset();

            at(want);
        }

        if (now.ending && now.spool->counted().has_value()) {
            now.ending = false;

            end();
        }
    });
}

// Shows a document given as a roll of `lines` lines. A roll is all the page needs for drawing;
// where it comes from does not matter. Given directly it is a pure line document, so the previous
// document's spool and its intentions are dropped; `hold` with a spool calls this same function
// and only then puts its own spool in place.
void Page::hold(Roll given, std::size_t lines) {
    Kept& kept = *kept_;

    kept.spool.reset();
    kept.ending = false;
    kept.sought.reset();

    kept.roll = std::move(given);
    kept.told = lines;

    kept.top = 0;
    kept.part = 0;
    kept.fresh = true;

    (void)trigger(ScrollEvent{});
}

// The spool of a byte document, or an empty pointer for a line document.
const std::shared_ptr<Spool>& Page::spool() const { return kept_->spool; }

// Whether the page holds any document.
bool Page::held() const { return static_cast<bool>(kept_->roll); }

// The document line shown at the top, or nothing without a document.
std::optional<std::size_t> Page::first() const {
    if (!held()) return {};

    return kept_->top;
}

// The number of lines: a spool knows it only after reaching the end, a roll tells it at once.
std::optional<std::size_t> Page::lines() const {
    const Kept& kept = *kept_;

    if (kept.spool) return kept.spool->counted();

    return kept.told;
}

// The byte at which the first visible line starts, which is what Midnight Commander shows in its
// viewer as the position in the file. Empty for a line document.
std::optional<std::size_t> Page::at() const {
    const Kept& kept = *kept_;

    const std::optional<std::size_t> which = first();

    if (!kept.spool || !which.has_value()) return {};

    return kept.spool->at(*which);
}

// The size of a byte document in bytes; empty for a line document.
std::optional<std::size_t> Page::whole() const {
    const Kept& kept = *kept_;

    if (!kept.spool) return {};

    return kept.spool->whole();
}

// Jumps to the line containing byte `given`. Without a spool there is nothing to say, because a
// roll does not know bytes. When the spool has not reached the byte because it waits for a window,
// the jump stays an intention and completes when the window arrives.
void Page::at(std::size_t given) {
    Kept& kept = *kept_;

    if (!kept.spool) return;

    const std::size_t which = kept.spool->found(given);

    if (kept.spool->stalled()) {
        kept.sought = given;

        return;
    }

    first(which);
}

// Puts document line `given` at the top, clamped to the last line when the count is known, and
// reports a `ScrollEvent` when the position changes.
void Page::first(std::size_t given) {
    Kept& kept = *kept_;

    if (!held()) return;

    const std::optional<std::size_t> many = lines();
    const std::size_t want = many.has_value() && *many > 0 && given >= *many ? *many - 1 : given;

    if (want == kept.top && kept.part == 0) return;

    kept.top = want;
    kept.part = 0;
    kept.fresh = true;

    (void)move_by(0);

    (void)trigger(ScrollEvent{});
}

// Scrolls so that the last line stands at the bottom of the screen. Without an index the end can
// be found only one way: by walking the document. The spool counts the lines along the way, and
// from then on `lines()` is no longer empty. With an asynchronous source the walk goes window by
// window and the jump to the end completes by itself once the count is known.
void Page::end() {
    Kept& kept = *kept_;

    if (!held()) return;

    if (kept.spool && !kept.spool->counted().has_value()) {
        kept.spool->counting();

        if (!kept.spool->counted().has_value()) kept.ending = true;
    }

    const std::optional<std::size_t> many = lines();

    if (!many.has_value() || *many == 0) return;

    kept.top = *many - 1;
    kept.part = std::max(pieces(kept.top) - 1, 0);
    kept.fresh = true;

    slide(-std::max(kept.sized.high - 1, 0));

    (void)trigger(ScrollEvent{});
}

// How many screen rows this document line occupies. Without wrapping always one; with wrapping as
// many as cutting by columns gives.
int Page::pieces(std::size_t which) const {
    const Kept& kept = *kept_;

    if (!kept.wrapped || kept.sized.wide <= 0 || !kept.roll) return 1;

    const std::optional<std::vector<std::string>> got = kept.roll(which, 1);

    if (!got.has_value() || got->empty()) return 1;

    return counted(got->front(), kept.sized.wide);
}

// Moves the view by `lines` screen rows without notifying anyone; returns whether the position
// changed. A step counts in screen rows, not document lines, because that is how far the picture
// moves; with wrapping one document line is several steps. It does not notify, because a jump to
// a mark scrolls and changes the current mark in one move, so it must send one event, not two.
//
// The screen does not go past the end. A step down stops when the last document line stands at
// the bottom of the screen, not at the top with emptiness below it. The clamping goes by screen
// rows (with wrapping a document line is several), counts forward only as far as the screen is
// high, and works for zero steps too; that is how a `first()` jump to the last page is brought
// back into order. Before layout there is no height and nothing to clamp.
//
// A roll that does not have a line yet is not the end. Emptiness instead of a vector means "waiting
// for a window": a step down stops at what there is, and the clamping to the end does nothing,
// because it is not known where the end is. When the window arrives, the page repaints and the
// next step goes through.
//
// Every document line is looked at once per move. `looked` remembers what is known about a line:
// `-1` means it waits for a window, `0` that it is past the end, more is the number of pieces.
// Without that, paging down near the end of the document would fetch and cut height squared lines;
// now stepping back by one screen row, once the walk has reached the end of the document, adds
// exactly one line.
bool Page::move_by(int lines) {
    Kept& kept = *kept_;

    if (!held()) return false;

    const std::size_t was = kept.top;
    const int part = kept.part;

    std::unordered_map<std::size_t, int> seen;

    const auto looked = [&kept, &seen](std::size_t which) {
        if (const auto known = seen.find(which); known != seen.end()) return known->second;

        const std::optional<std::vector<std::string>> got = kept.roll(which, 1);

        int made = 1;

        if (!got.has_value())
            made = -1;
        else if (got->empty())
            made = 0;
        else if (kept.wrapped && kept.sized.wide > 0)
            made = counted(got->front(), kept.sized.wide);

        seen.emplace(which, made);

        return made;
    };

    const auto pieced = [&looked](std::size_t which) { return std::max(looked(which), 1); };

    for (int one = 0; one < lines; ++one) {
        if (kept.part + 1 < pieced(kept.top)) {
            ++kept.part;

            continue;
        }

        if (looked(kept.top + 1) <= 0) break;

        ++kept.top;
        kept.part = 0;
    }

    for (int one = 0; one > lines; --one) {
        if (kept.part > 0) {
            --kept.part;

            continue;
        }

        if (kept.top == 0) break;

        --kept.top;
        kept.part = pieced(kept.top) - 1;
    }

    if (const int high = kept.sized.high; high > 0) {
        int rows = -kept.part;

        for (std::size_t which = kept.top; rows <= high; ++which) {
            const int got = looked(which);

            if (got < 0) rows = high;

            if (got <= 0) break;

            rows += got;
        }

        while ((kept.top > 0 || kept.part > 0) && rows < high) {
            if (kept.part > 0) {
                --kept.part;
            } else {
                --kept.top;

                if (looked(kept.top) < 0) break;

                kept.part = pieced(kept.top) - 1;
            }

            ++rows;
        }
    }

    if (kept.top == was && kept.part == part) return false;

    kept.fresh = true;

    return true;
}

// Scrolls by `lines` screen rows and reports a `ScrollEvent` when the view moved; returns whether
// it moved. The notification travels the ordinary event path, so it bubbles up to the ancestors.
// The top bar is a sibling, not an ancestor, so the listener has to be hung higher up, on a
// container or the root.
bool Page::slide(int lines) {
    if (!move_by(lines)) return false;

    (void)trigger(ScrollEvent{});

    return true;
}

// Scrolls by `pages` screen heights.
void Page::page(int pages) { slide(pages * std::max(kept_->sized.high, 1)); }

// Whether long lines are wrapped.
bool Page::wrapped() const { return kept_->wrapped; }

// Turns wrapping on or off. The same byte means something else in the two modes, so the view goes
// back to the start of the line it stands on. Without that, switching would throw the screen to a
// place nobody asked for.
void Page::wrapped(bool given) {
    Kept& kept = *kept_;

    if (kept.wrapped == given) return;

    kept.wrapped = given;
    kept.part = 0;
    kept.fresh = true;

    (void)trigger(ScrollEvent{});
}

// Whether the page draws a scrollbar.
bool Page::scrollbar() const { return kept_->shape.scrollbar; }

// Turns the scrollbar on or off.
void Page::scrollbar(bool given) {
    if (kept_->shape.scrollbar == given) return;

    kept_->shape.scrollbar = given;
    kept_->fresh = true;
}

namespace {

// The order of marks: by line, and by start for equal lines. A mark is a range and a role, nothing
// more, exactly like Decoration.mark in CodeMirror. It is not a bookmark, a selection or a search
// result; each of them may use it, but none of them is one. The range counts in bytes of the line,
// because a document byte exists only with a byte source. The set is kept sorted in this order,
// otherwise `next` would have no way to tell which mark comes next.
bool ahead(const Mark& one, const Mark& two) {
    return one.line != two.line ? one.line < two.line : one.from < two.from;
}

}


// How many document lines the last frame showed.
std::size_t Page::shown() const { return kept_->walked; }

// Makes mark `which` the current one and scrolls to it as little as possible: a mark already
// visible does not move the picture, one above lands at the top, one below at the bottom. One event
// is sent, even when the move consists of two steps.
bool Page::reach(std::size_t which) {
    MarkedNode::which(which);

    (void)reveal(marks()[which].line);

    (void)trigger(ScrollEvent{});

    return true;
}

// Scrolls as little as possible to make document line `line` visible: a line already visible does
// not move the picture, one above lands at the top, one below at the bottom. Returns whether the
// view changed. It does not notify; the caller does, so that a compound move gives one event.
bool Page::reveal(std::size_t line) {
    Kept& kept = *kept_;

    // Before the page gets its allotment there is no way to tell whether the line is visible
    // anyway: the height is zero, so every line except the first would look hidden and the picture
    // would jump for no reason. The question is postponed until the picture is assembled, when the
    // height is known.
    if (kept.sized.high <= 0) {
        kept.wish = line;
        kept.fresh = true;

        return true;
    }

    kept.wish.reset();

    const std::size_t last = kept.walked > 0 ? kept.top + kept.walked - 1 : kept.top;

    if (line >= kept.top && line <= last) return false;

    const bool above = line < kept.top;

    kept.top = line;
    kept.part = 0;
    kept.fresh = true;

    if (!above) (void)move_by(-std::max(kept.sized.high - 1, 0));

    return true;
}

// Forces the next frame to lay the document out again.
void Page::invalidate() { kept_->fresh = true; }

// The document line at the top of the view.
std::size_t Page::topmost() const { return kept_->top; }

// The cell on which this byte of this line stands, or nothing when the line is off screen or the
// byte fell outside what fit. The editor takes the cursor position from here.
std::optional<Spot> Page::spot_of(Place where) const {
    const Kept& kept = *kept_;
    const std::size_t line = where.line;
    const std::size_t byte = where.byte;

    for (std::size_t one = 0; one < kept.shown.size(); ++one) {
        const Cut& piece = kept.shown[one];

        if (piece.line != line) continue;

        for (std::size_t col = 0; col < piece.bytes.size(); ++col)
            if (piece.bytes[col] == byte)
                return Spot{static_cast<int>(col), static_cast<int>(one)};

        // End of the line: the cursor stands past the last character, so no column carries it.
        if (!piece.bytes.empty() && byte > piece.bytes.back())
            return Spot{static_cast<int>(piece.bytes.size()), static_cast<int>(one)};

        if (piece.bytes.empty()) return Spot{0, static_cast<int>(one)};
    }

    return {};
}

// The inverse of `spot_of`: from a cell to a document line and a byte in it. A screen row is not a
// line number, because with wrapping one line occupies several, and a column is not a byte,
// because a tab occupies eight cells and a Chinese character two. Cutting into pieces has already
// computed both maps, so all that is left here is reading them.
std::optional<Place> Page::place_of(Spot where) const {
    const Kept& kept = *kept_;

    if (where.y < 0 || static_cast<std::size_t>(where.y) >= kept.shown.size()) return {};

    const Cut& piece = kept.shown[static_cast<std::size_t>(where.y)];

    const std::size_t col = static_cast<std::size_t>(std::max(where.x, 0));

    // Past the last character of the piece: the cursor stands just past it, that is, where the
    // next piece starts or where the line ends.
    if (col >= piece.bytes.size()) return Place{piece.line, piece.upto};

    return Place{piece.line, piece.bytes[col]};
}

// The role in which the current mark is painted. The current mark is painted in a different role
// than the others. The viewer knows which one it stands on, so it substitutes the role itself; the
// application does not have to pass the marks again just so that one of them looks different. The
// role name can be replaced, like the widget's own role.
const std::string& Page::chosen() const { return kept_->chosen; }

// Replaces the role in which the current mark is painted.
void Page::chosen(std::string given) { kept_->chosen = std::move(given); }

// The role in which the scrollbar is painted.
const std::string& Page::scrollbar_role() const { return kept_->scrollbar_role; }

// Replaces the role in which the scrollbar is painted.
void Page::scrollbar_role(std::string given) { kept_->scrollbar_role = std::move(given); }

// A page always takes the focus, so that the keys can scroll it.
bool Page::focusable() const { return true; }

// The viewer takes no children; it is the content itself.
int Page::capacity() const { return 0; }

// A page takes the whole room it is offered.
Size Page::wants(Size room) const { return room; }

// Lays out the visible pieces for an area of size `given`. It recomputes only when the allotment
// or the position changed. Without that, every frame refresh would walk the document again, and
// with an animated neighbor that happens ten times a second.
void Page::spread(Size given) {
    Kept& kept = *kept_;

    if (!kept.fresh && kept.sized == given) return;

    kept.sized = given;
    kept.fresh = false;
    kept.shown.clear();
    kept.more = false;

    if (!kept.roll || given.wide <= 0 || given.high <= 0) return;

    if (kept.wish.has_value()) {
        (void)reveal(*kept.wish);

        kept.fresh = false;
    }

    // The spacing of the spool's anchors comes from the window height; the spool knows nothing
    // about the view.
    if (kept.spool) kept.spool->screen(given.high);

    const std::size_t want = static_cast<std::size_t>(given.high);

    std::size_t which = kept.top;
    int skip = kept.part;

    kept.walked = 0;

    while (kept.shown.size() < want) {
        const std::optional<std::vector<std::string>> got = kept.roll(which, batch);

        if (!got.has_value() || got->empty()) break;

        for (const std::string& one : *got) {
            for (Cut& piece : parted(one, given.wide, kept.wrapped)) {
                if (skip > 0) {
                    --skip;

                    continue;
                }

                if (kept.shown.size() >= want) {
                    kept.more = true;

                    break;
                }

                piece.line = which;

                kept.shown.push_back(std::move(piece));
            }

            ++which;

            if (kept.shown.size() >= want) break;
        }
    }

    kept.walked = which - kept.top;

    if (!kept.more && kept.shown.size() >= want) {
        const std::optional<std::vector<std::string>> next = kept.roll(which, 1);

        kept.more = !next.has_value() || !next->empty();
    }
}

// Fills in the scrollbar's position, whole and window; returns false when there is nothing to
// measure. The scrollbar is measured in lines when their number is known, and in bytes when it is
// not. With a document in memory and with a line source the lines are known at once and they come
// out more naturally. With a stream the number of lines is unknown until the first End, so the
// scrollbar then moves by bytes, which are exact from the moment the file is opened.
bool Page::gauged(std::size_t& shift, std::size_t& whole, std::size_t& window) const {
    const Kept& kept = *kept_;

    const std::optional<std::size_t> many = lines();

    if (many.has_value()) {
        if (*many == 0) return false;

        shift = kept.top;
        whole = *many;
        window = static_cast<std::size_t>(std::max(kept.sized.high, 1));

        return true;
    }

    const std::optional<std::size_t> from = at();
    const std::optional<std::size_t> much = this->whole();

    if (!from.has_value() || !much.has_value() || *much == 0) return false;

    // The window in bytes is the span of what currently stands on the screen: from the start of the
    // first visible line to the start of the line past the last one. `spread` has walked these
    // lines anyway. When the spool does not know that second line yet, the rest of the document is
    // taken.
    const std::optional<std::size_t> upto = kept.spool->at(kept.top + shown());

    shift = *from;
    whole = *much;
    window = upto.has_value() ? *upto - *from : *much - *from;

    return true;
}

namespace {

// Overlays the marks on one drawn piece. Marks are overlaid after the content is drawn, cell by
// cell: the letter already there stays, and the style is merged through `Style::merge`. Thanks to
// that, a role that sets only the background keeps the color of the letters, and a role that sets
// only the letters keeps the background.
//
// A cell with a zero letter is the continuation of a double-width character; it is skipped,
// because the first half has already repainted both.
void painted(View& into, const Cut& piece, Spot at, const Style& look,
             const std::vector<Mark>& marks, std::optional<std::size_t> which,
             const std::string& chosen, const Skin& how) {
    for (std::size_t nth = 0; nth < marks.size(); ++nth) {
        const Mark& one = marks[nth];

        if (one.line != piece.line) continue;

        if (one.upto <= one.from) continue;

        const bool here = which.has_value() && *which == nth;

        const Style over = Style::merge(look, how.style(here ? chosen : one.role));

        for (std::size_t col = 0; col < piece.bytes.size(); ++col) {
            const std::size_t byte = piece.bytes[col];

            if (byte < one.from || byte >= one.upto) continue;

            const Spot spot{at.x + static_cast<int>(col), at.y};
            const Cell said = into.at(spot);

            if (said.letter == 0) continue;

            into.put(spot, said.letter, over);
        }
    }
}

}

// Draws the visible pieces, the marks over them and, when it is on, the scrollbar.
View Page::paint(Size given, Area seen) {
    settle();

    View made{seen.size};

    made.clear({{0, 0}, seen.size});

    // The scrollbar takes one column on the right, so the content gets that much less room, just
    // as in the scrollers.
    int aside = kept_->shape.scrollbar && given.wide > 0 ? 1 : 0;

    spread({given.wide - aside, given.high});

    // The "when needed" scrollbar (`needed`): a page that fits entirely from the first line gives
    // its column back to the content and lays itself out once more. Text wrapped wider then takes
    // the same number of rows or fewer, so it still fits.
    if (aside != 0 && kept_->shape.needed && kept_->top == 0 && kept_->part == 0 && !kept_->more) {
        aside = 0;

        spread({given.wide, given.high});
    }

    if (kept_->shown.empty() && aside == 0) return made;

    const Style look = skin().style(role());

    made.fill({{0, 0}, seen.size}, look);

    for (std::size_t one = 0; one < kept_->shown.size(); ++one) {
        const Cut& piece = kept_->shown[one];
        const int y = static_cast<int>(one) - seen.at.y;

        made.put({-seen.at.x, y}, piece.text, look);

        if (!marks().empty())
            painted(made, piece, {-seen.at.x, y}, look, marks().all(), which(),
                    kept_->chosen, skin());
    }

    std::size_t shift = 0;
    std::size_t whole = 0;
    std::size_t window = 0;

    if (aside != 0 && gauged(shift, whole, window)) {
        const Scrollbar bar{.at = Scrollbar::edge(given, seen, true),
                            .much = given.high,
                            .shift = shift,
                            .whole = whole,
                            .window = window,
                            .skin = skin(),
                            .role = kept_->scrollbar_role};

        bar.draw(made);

        Track mine = bar.track();

        mine.at = Scrollbar::edge(given, {{0, 0}, given}, true);

        kept_->grip.laid(mine);
        kept_->bytes = !lines().has_value();
    } else {
        kept_->grip.laid(std::nullopt);
    }

    return made;
}

// Scrolls with the arrows, PageUp, PageDown, Home and End. It returns false for everything it did
// not use: the F-keys have to reach the legend, which is an ancestor, and the journey ends at the
// first true.
bool Page::handle(KeyEvent& what) {
    // Nothing with a modifier is taken: Ctrl+Home and Alt+PageDown are by design different
    // shortcuts than Home and PageDown, so the combination is passed on to the application.
    if (what.mods().ctrl || what.mods().alt) return false;

    switch (what.key().code()) {
        case Code::up: slide(-1); return true;
        case Code::down: slide(1); return true;
        case Code::pageup: page(-1); return true;
        case Code::pagedown: page(1); return true;
        case Code::home: first(0); return true;
        case Code::end: end(); return true;
        default: return false;
    }
}

// Scrolls `stepping` rows per wheel step; returns whether the view moved.
bool Page::handle(WheelEvent& what) {
    const std::size_t was = kept_->top;
    const int part = kept_->part;

    slide(-what.steps() * stepping);

    return kept_->top != was || kept_->part != part;
}

// Takes the focus on a click. The guard on the root is there because a widget is sometimes
// rendered in a test without a root.
bool Page::handle(ClickEvent&) {
    if (root() != nullptr) focus();

    return true;
}

// The mouse on the scrollbar: an arrow scrolls a line, the track a page, and the thumb jumps to
// where it was put, to a line when the scrollbar counts lines and to a byte when it counts bytes.
bool Page::handle(MouseEvent& what) {
    const Asked asked = kept_->grip.mouse(what, local(what.where()), *this);

    switch (asked.part) {
        case Part::none: return false;
        case Part::back: slide(-1); break;
        case Part::forth: slide(1); break;
        case Part::before: page(-1); break;
        case Part::after: page(1); break;
        case Part::thumb:
            if (asked.to.has_value()) {
                if (kept_->bytes) at(*asked.to);
                else first(*asked.to);
            }
            break;
    }

    return true;
}

}
