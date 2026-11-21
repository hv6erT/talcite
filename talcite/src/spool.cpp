#include "spool.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>

// A spool: a byte source presented as lines. It scrolls forward and back through a document that
// cannot be held whole, and never indexes it: it remembers only a window of bytes, the place where
// it stands (`here_`), a handful of anchors (line number and byte where that line starts), and the
// extents of the lines it walked last.
//
// It hands out bytes and line bounds, never the text of a whole line. A line may be a gigabyte
// long (a file without a single \n); whoever draws shows a few dozen characters of it, so it reads
// those (`read`) and nothing more. To know where a line ends, the spool walks it to its \n, but it
// does not keep it: the bytes pass through a window that is trimmed as it goes. A walk interrupted
// by a window that has not arrived yet (`Walk`, and `Behind` for a walk backward) is kept, and the
// next question goes on from where the last one stopped, so no walk ever starts again from the
// start of its line.
//
// It knows nothing of the window width, the skin or the tree. It does not expand tabs and does not
// wrap. Everything about drawing stays in `Page`.
//
// It does not wait for bytes. A `Feed` returns a promise: a window that is not at hand is ordered,
// and the spool answers with what it has (nothing yet), while `stalled()` says that the answer is
// incomplete because it waits for the disk. The window arrives on the main thread, and then the
// `awoken` handler is called; whoever asked asks again. A document given as bytes never stalls.
// `whole()` is the document's length in bytes, and `counted()` the number of lines once it is
// known.
//
// `Peek` is an internal seam, not part of the library's API: a look into bytes that already exist.
// The editor reads through its quilt, which lies in memory and answers at once, and the editor's
// spool must not stall on the cursor. No public entry accepts `Peek`: a source outside memory is
// always a promise in talcite (`Feed`).
//
// `Want` is the byte range a stalled request needs, kept until a window arrives.
namespace talcite {

namespace {

// The window of bytes kept at hand. One call to the source covers a few hundred lines of ordinary
// text, so scrolling with an arrow almost never goes back to it. It is also the most `read` hands
// out at once.
constexpr std::size_t window = 64 * 1024;

// How many bytes of windows may be kept at once when they arrive one after another: further ones
// push the oldest out at the front. Without a limit, walking the document to its end would pull it
// in whole. Nothing the spool does needs more than the piece it is looking at, so the limit is a
// matter of memory only.
constexpr std::size_t stock = 8 * window;

// Anchors cannot keep multiplying, because in a large document they alone would take more than the
// window. When full, every other one is dropped and the stride doubles: memory stays flat, and the
// walk from an anchor grows only where there is a long jump to a place never visited anyway.
constexpr std::size_t plenty = 4096;

// The floor of the stride. With a window three rows high, an anchor every three lines would be pure
// overhead.
constexpr std::size_t least = 64;

// How many line extents are remembered around the last one walked. A screen shows a few dozen
// lines, and each frame asks for their starts and ends; these are answered from here instead of
// walking a long line again on every frame.
constexpr std::size_t remembered = 1024;

// The "no limit" for a walk forward: walk to the end of the line, wherever it is.
constexpr std::size_t endless = SIZE_MAX;

}

// A spool over an asynchronous source, possibly warmed up. `head` is the bytes from the start of
// the document that the caller already has (the header window, read to learn the file type), so
// window zero is there at once and the first request answers immediately, without an order and
// without a turn of the loop. Everything after that works as usual; only where the first window
// came from differs. An empty head gives a cold spool. Cutting the head to the document and to
// `stock` is a guard on memory, not a rule.
Spool::Spool(Feed feed, std::size_t whole, Bytes head) : feed_(std::move(feed)), whole_(whole) {
    anchors_.push_back({0, 0});

    if (whole_ == 0) counted_ = 0;

    if (head.empty()) return;

    head.resize(std::min({head.size(), whole_, stock}));

    chunk_ = std::move(head);
    chunked_ = 0;
}

// A spool over the internal synchronous seam (`Peek`), which answers at once and never stalls.
Spool::Spool(Peek peek, std::size_t whole) : peek_(std::move(peek)), whole_(whole) {
    anchors_.push_back({0, 0});

    if (whole_ == 0) counted_ = 0;
}

// A document given whole has its own entry, because then the window is the document itself: there
// is nothing to fetch and nothing to order. Lines are counted at once: for a license text this
// takes microseconds, and `Viewer` gets all the values as soon as the document is given.
Spool::Spool(Bytes given) : held_(std::move(given)), whole_(held_.size()) {
    anchors_.push_back({0, 0});

    chunk_ = held_;
    chunked_ = 0;

    std::size_t at = 0;
    std::size_t which = 0;

    while (at < whole_) {
        at = forth({which, at}, endless).extent.next;

        ++which;
    }

    counted_ = which;
}

// Whether the bytes [at, upto) are all in the current window.
bool Spool::cached(std::size_t at, std::size_t upto) const {
    return at >= chunked_ && upto <= chunked_ + chunk_.size();
}

// Makes sure the whole range of `much` bytes from `at` lies in the window; returns whether it does.
//
// The internal seam (`Peek`, the editor's quilt) may return less than asked, so we fetch in a loop
// until the range is full or the source stops giving anything. Taking one short answer as the end
// of the document would make the line count random and cut the document off halfway.
//
// A source (`Feed`) gives nothing at once: we order the window, record that the answer is
// incomplete, and return false. Whoever asked gets nothing and asks again when the window arrives.
bool Spool::ranged(std::size_t at, std::size_t much) {
    if (at >= whole_) return false;

    const std::size_t upto = std::min(at + much, whole_);

    if (cached(at, upto)) return true;

    if (peek_) {
        chunk_.clear();
        chunked_ = at;

        while (chunked_ + chunk_.size() < upto) {
            const std::size_t from = chunked_ + chunk_.size();

            const Bytes more = peek_(from, std::max(upto - from, window));

            if (more.empty()) break;

            chunk_.insert(chunk_.end(), more.begin(), more.end());
        }

        return chunked_ + chunk_.size() >= upto;
    }

    if (!feed_) return false;

    stalled_ = true;
    wanted_ = Want{at, upto};

    ask(at, upto);

    return false;
}

// Orders a window from the place that is missing; when the start of the range is already here,
// from the end of what we have, so as to append rather than start over. One order is in flight at
// a time; the next one waits in `wanted_` and goes out when the running one arrives.
void Spool::ask(std::size_t at, std::size_t upto) {
    if (busy_ || !feed_) return;

    const std::size_t end = chunked_ + chunk_.size();
    const std::size_t from = !chunk_.empty() && at >= chunked_ && at <= end ? end : at;

    if (from >= whole_) return;

    const std::size_t most = std::max(upto > from ? upto - from : 0, window);

    busy_ = true;

    const std::weak_ptr<Spool> weak = weak_from_this();

    ordered_.emplace(feed_(from, most)
                       .here()
                       .then([weak, from](Bytes got) {
                           if (const std::shared_ptr<Spool> mine = weak.lock()) mine->arrived(from, std::move(got));
                       })
                       .recover([weak, from](const std::exception&) {
                           // The source refused: the document ends where we got to. Otherwise
                           // every paint would order the same window again, forever.
                           if (const std::shared_ptr<Spool> mine = weak.lock()) mine->arrived(from, {});
                       }));
}

// A window has arrived. It is appended to what we have when adjacent, otherwise it starts a new
// window. The excess falls off the front: whoever scrolls forward does not need what was left far
// behind. Then the second round: the order that was waiting, a count in progress, and whoever
// wanted to know that something arrived.
//
// We trim only at two `stock`s, and then down to one. Trimming at every window above `stock` would
// shift seven eighths of the stock forward to add one eighth, seven bytes rewritten for every byte
// read, so walking the document to its end (End, counting lines) would cost seven walks. This way
// the stock is rewritten once per as many bytes as it holds: a constant cost per byte, and at most
// two stocks of memory.
void Spool::arrived(std::size_t from, Bytes got) {
    busy_ = false;

    const bool nothing = got.empty();

    if (!nothing) {
        if (!chunk_.empty() && from == chunked_ + chunk_.size()) {
            const std::size_t was = chunk_.size();

            chunk_.resize(was + got.size());

            std::copy(got.begin(), got.end(), chunk_.begin() + static_cast<std::ptrdiff_t>(was));

            if (chunk_.size() > 2 * stock) {
                const std::size_t drop = chunk_.size() - stock;

                chunk_.erase(chunk_.begin(), chunk_.begin() + static_cast<std::ptrdiff_t>(drop));
                chunked_ += drop;
            }
        } else {
            chunk_ = std::move(got);
            chunked_ = from;
        }
    }

    // The source returned nothing, although the document promised more: the file shrank under us
    // or stopped being readable. There is nothing further; the line count settles at the next walk.
    if (nothing && from < whole_) whole_ = from;

    if (wanted_.has_value()) {
        const Want want = *wanted_;

        wanted_.reset();

        if (!cached(want.at, std::min(want.upto, whole_)) && want.at < whole_) {
            stalled_ = true;
            wanted_ = want;

            ask(want.at, want.upto);
        }
    }

    if (counting_ && !counted_.has_value()) (void)at(SIZE_MAX);

    if (awoken_) awoken_();
}

// Orders the next window before anyone asks for it, when what was just used lies closer to the end
// of the window than half a window. Scrolling down then goes without stops: the window is on its
// way when the user presses the arrow.
void Spool::ahead(std::size_t from) {
    if (!feed_ || busy_ || stalled_) return;

    const std::size_t end = chunked_ + chunk_.size();

    if (end < whole_ && from <= end && end - from < window / 2) ask(end, end + window);
}

// The byte at document offset `at`, which must lie in the window.
char Spool::sign(std::size_t at) const { return static_cast<char>(chunk_[at - chunked_]); }

// Walks line `from` forward to its \n, but not past byte `limit`: `line` when the line's end is
// found (its extent is then complete and remembered), `short_of` when byte `limit` lies in the
// line and the walk stopped there, `waiting` when a window has been ordered. The walk goes from
// where an interrupted walk of the same line stopped, not from the start of the line: a line far
// longer than the window is walked once, window after window, however many times it is asked
// about. The bytes are scanned and let go; the line is never held.
//
// The content ends before its \n and before a \r right in front of it (Windows text), and the last
// line of a document ends at its end, likewise without a final \r.
Spool::Step Spool::forth(Anchor from, std::size_t limit) {
    if (const Known* got = known(from.line); got != nullptr && got->extent.at == from.at)
        return Step{Ended::line, got->extent};

    std::size_t go = from.at;
    bool carriage = false;

    if (walk_.has_value() && walk_->from.line == from.line && walk_->from.at == from.at && walk_->reached > go) {
        go = walk_->reached;
        carriage = walk_->carriage;
    }

    const std::size_t stop = limit == endless || limit >= whole_ ? whole_ : limit + 1;

    while (go < stop) {
        if (!ranged(go, 1)) {
            walk_ = Walk{from, go, carriage};

            return Step{Ended::waiting, {}};
        }

        const std::size_t upto = std::min(chunked_ + chunk_.size(), stop);

        for (std::size_t which = go; which < upto; ++which) {
            const char one = sign(which);

            if (one == '\n') {
                const Line made{from.at, carriage ? which - 1 : which, which + 1};

                remember(from.line, made, false);

                walk_ = Walk{{from.line + 1, which + 1}, which + 1, false};

                return Step{Ended::line, made};
            }

            carriage = one == '\r';
        }

        go = upto;
    }

    if (go < whole_) {
        walk_ = Walk{from, go, carriage};

        return Step{Ended::short_of, {}};
    }

    const Line made{from.at, carriage ? whole_ - 1 : whole_, whole_};

    remember(from.line, made, true);

    walk_.reset();

    return Step{Ended::line, made};
}

// The start of the line before the one starting at `at`, or nothing while a window is on its way.
// Backward one cannot count, only search: we look for the last \n before the one that ends the
// preceding line, scanning in pieces from the end toward the start. A search interrupted by a
// window that has not arrived goes on from where it stopped, so the line before a very long one is
// found too, window after window.
std::optional<std::size_t> Spool::back(std::size_t at) {
    if (at == 0) return 0;

    if (const Known* got = ending(at); got != nullptr) return got->extent.at;

    // `at` points at the start of a line, so the byte just before it is the \n ending the previous
    // one. That byte itself is not examined.
    std::size_t end = at - 1;

    if (behind_.has_value() && behind_->from == at) end = std::min(end, behind_->reached);

    while (end > 0) {
        const std::size_t step = std::min(window, end);
        const std::size_t from = end - step;

        if (!ranged(from, step)) {
            behind_ = Behind{at, end};

            return {};
        }

        for (std::size_t which = end; which > from; --which)
            if (sign(which - 1) == '\n') {
                behind_.reset();

                return which;
            }

        end = from;
    }

    behind_.reset();

    return 0;
}

// The last anchor not past line `which`. Anchors are ordered by line, so binary search finds it.
// Anchor zero always exists, so there is always a result.
Spool::Anchor Spool::nearest(std::size_t which) const {
    const auto past = std::upper_bound(anchors_.begin(), anchors_.end(), which,
                                       [](std::size_t want, const Anchor& one) { return want < one.line; });

    return *std::prev(past);
}

// The best known start for walking forward to line `which`: the nearest anchor, the place where the
// spool stands, the line after the last remembered extent before `which`, or the place an
// interrupted walk reached, whichever is the latest line not past `which`.
Spool::Anchor Spool::start_for(std::size_t which) const {
    Anchor made = nearest(which);

    const auto better = [&made, which](Anchor one) {
        if (one.line <= which && one.line > made.line) made = one;
    };

    better(here_);

    if (walk_.has_value()) better(walk_->from);

    const auto past = std::lower_bound(known_.begin(), known_.end(), which,
                                       [](const Known& one, std::size_t want) { return one.line < want; });

    if (past != known_.begin()) {
        const Known& one = *std::prev(past);

        better({one.line + 1, one.extent.next});
    }

    return made;
}

// The best known start for walking forward to the line holding byte `byte`: the latest line start
// not past it among the anchors, the place where the spool stands, the remembered extents and an
// interrupted walk.
Spool::Anchor Spool::start_at(std::size_t byte) const {
    const auto past = std::upper_bound(anchors_.begin(), anchors_.end(), byte,
                                       [](std::size_t want, const Anchor& one) { return want < one.at; });

    Anchor made = *std::prev(past);

    const auto better = [&made, byte](Anchor one) {
        if (one.at <= byte && one.at > made.at) made = one;
    };

    better(here_);

    if (walk_.has_value()) better(walk_->from);

    const auto after = std::upper_bound(known_.begin(), known_.end(), byte,
                                        [](std::size_t want, const Known& one) { return want < one.extent.next; });

    if (after != known_.begin()) {
        const Known& one = *std::prev(after);

        better({one.line + 1, one.extent.next});
    }

    return made;
}

// Records that line `which` starts at byte `at`, when `which` falls on the stride and lies past the
// last anchor; thins the anchors out when there are too many.
void Spool::anchored(std::size_t which, std::size_t at) {
    if (which == 0 || which % stride_ != 0) return;

    if (anchors_.back().line >= which) return;

    anchors_.push_back({which, at});

    if (anchors_.size() <= plenty) return;

    std::vector<Anchor> kept;
    kept.reserve(anchors_.size() / 2 + 1);

    for (std::size_t one = 0; one < anchors_.size(); one += 2) kept.push_back(anchors_[one]);

    anchors_ = std::move(kept);
    stride_ *= 2;
}

// Remembers the whole extent of line `which`, found by a walk: the start of the next line is then
// known too, so it goes to the anchors, and the line count when this was the last line. `open`
// says that the line ends at the end of the document rather than with a \n, so that text added
// after it would lengthen it. The remembered extents are kept in line order; past twice their
// limit, only those nearest the line just walked stay, because the next questions come from around
// it.
void Spool::remember(std::size_t which, Line extent, bool open) {
    anchored(which + 1, extent.next);

    if (extent.next >= whole_) counted_ = which + 1;

    const auto at = std::lower_bound(known_.begin(), known_.end(), which,
                                     [](const Known& one, std::size_t want) { return one.line < want; });

    if (at != known_.end() && at->line == which) {
        at->extent = extent;
        at->open = open;

        return;
    }

    known_.insert(at, Known{which, extent, open});

    if (known_.size() <= 2 * remembered) return;

    const auto here = std::lower_bound(known_.begin(), known_.end(), which,
                                       [](const Known& one, std::size_t want) { return one.line < want; });

    const std::size_t middle = static_cast<std::size_t>(here - known_.begin());
    const std::size_t from = middle > remembered / 2 ? middle - remembered / 2 : 0;
    const std::size_t upto = std::min(from + remembered, known_.size());

    known_.erase(known_.begin() + static_cast<std::ptrdiff_t>(upto), known_.end());
    known_.erase(known_.begin(), known_.begin() + static_cast<std::ptrdiff_t>(from));
}

// The remembered extent of line `which`, or null.
const Spool::Known* Spool::known(std::size_t which) const {
    const auto at = std::lower_bound(known_.begin(), known_.end(), which,
                                     [](const Known& one, std::size_t want) { return one.line < want; });

    return at != known_.end() && at->line == which ? &*at : nullptr;
}

// The remembered line holding byte `byte` (from its start up to and including its \n), or null.
const Spool::Known* Spool::holding(std::size_t byte) const {
    const auto after = std::upper_bound(known_.begin(), known_.end(), byte,
                                        [](std::size_t want, const Known& one) { return want < one.extent.at; });

    if (after == known_.begin()) return nullptr;

    const Known& one = *std::prev(after);

    return byte < one.extent.next ? &one : nullptr;
}

// The remembered line whose next line starts at `byte`, or null.
const Spool::Known* Spool::ending(std::size_t byte) const {
    const Known* one = holding(byte > 0 ? byte - 1 : 0);

    return one != nullptr && one->extent.next == byte ? one : nullptr;
}

// Sets the anchor stride from the screen height, because a jump is almost always a page, Home, End
// or a drag of the scrollbar, and then each lands right next to a known point. It never shrinks:
// anchors once thinned have no way to grow dense again, and going back and forth would cost the
// thinning itself.
void Spool::screen(int high) {
    const std::size_t want = high > 0 ? static_cast<std::size_t>(high) : 0;

    stride_ = std::max(stride_, std::max(least, want));
}

// A change in one place: `gone` bytes from `at` replaced by `came`. Anchors past the changed
// stretch move by the difference in bytes and lines: a line that started there still starts, just
// elsewhere. Dropping every anchor past the change would make each key pressed at a block far into
// the file recount the lines from the change to the block, megabytes per key in a large file. Only
// anchors inside the deleted stretch die (after its start, up to and including its end): their line
// may have vanished or may start elsewhere. An anchor exactly at `at` stays: the byte before it did
// not change, so the line still starts there. The same goes for the place where the spool stands.
//
// A remembered extent stays when its line ends with a \n before the change, moves when the line
// starts past the changed stretch, and dies when the change touches it; a line that ends at the
// end of the document is touched by anything added after it. Interrupted walks are dropped: they
// say what lies in bytes that may have changed. The byte window is discarded whole, since it holds
// the bytes from before the change.
//
// `lines` is the difference in line ends; the caller knows it locally, because it knows what it
// inserted and what it deleted. The line count, if known, is corrected by it; if unknown, it stays
// unknown.
void Spool::changed(std::size_t at, std::size_t gone, std::size_t came, std::ptrdiff_t lines) {
    whole_ = whole_ - gone + came;

    chunk_.clear();
    chunked_ = 0;

    walk_.reset();
    behind_.reset();

    const std::size_t end = at + gone;

    const auto shifted = [&](std::size_t one) { return one - gone + came; };

    const auto lined = [&](std::size_t one) {
        return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(one) + lines);
    };

    const auto moved = [&](Anchor& one) {
        if (one.at <= at || one.line == 0) return true;

        if (one.at <= end) return false;

        one.at = shifted(one.at);
        one.line = lined(one.line);

        return true;
    };

    const auto kept = std::remove_if(anchors_.begin(), anchors_.end(), [&moved](Anchor& one) { return !moved(one); });

    anchors_.erase(kept, anchors_.end());

    if (!moved(here_)) here_ = {0, 0};

    const auto lost = std::remove_if(known_.begin(), known_.end(), [&](Known& one) {
        if (one.extent.next <= at && !one.open) return false;

        if (one.extent.at <= end) return true;

        one.line = lined(one.line);
        one.extent = {shifted(one.extent.at), shifted(one.extent.upto), shifted(one.extent.next)};

        return false;
    });

    known_.erase(lost, known_.end());

    if (counted_.has_value()) counted_ = lined(*counted_);
}

// A change in many places at once ("replace all"), the first of them at `at`, with `bytes` and
// `lines` the total differences: the cuts move different stretches by different amounts, so
// everything past the first one is discarded. Nothing before the first cut has moved.
void Spool::rewritten(std::size_t at, std::ptrdiff_t bytes, std::ptrdiff_t lines) {
    whole_ = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(whole_) + bytes);

    chunk_.clear();
    chunked_ = 0;

    walk_.reset();
    behind_.reset();

    const auto kept = std::remove_if(anchors_.begin(), anchors_.end(),
                                     [at](const Anchor& one) { return one.at > at && one.line > 0; });

    anchors_.erase(kept, anchors_.end());

    const auto lost = std::remove_if(known_.begin(), known_.end(),
                                     [at](const Known& one) { return one.extent.next > at || one.open; });

    known_.erase(lost, known_.end());

    if (here_.at > at) here_ = {0, 0};

    if (counted_.has_value())
        counted_ = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(*counted_) + lines);
}

// The byte where line `which` starts; nothing means "past the end", or "not known yet" when
// `stalled()`. Under an ordered window nothing is lost: the walk records how far it got, and the
// next call goes on from there.
std::optional<std::size_t> Spool::at(std::size_t which) {
    stalled_ = false;

    if (counted_.has_value() && which >= *counted_) return {};

    if (which == here_.line) return here_.at;

    if (const Known* got = known(which); got != nullptr) {
        here_ = {which, got->extent.at};

        return here_.at;
    }

    Anchor from = start_for(which);

    // Of the two ways we take the shorter: forward from the best known start, or back from where we
    // stand. For the up arrow this is always one step back. Each step back is recorded at once, so
    // a step interrupted by a window does not repeat the ones before it.
    if (which < here_.line && here_.line - which <= which - from.line) {
        while (here_.line > which) {
            const std::optional<std::size_t> went = back(here_.at);

            if (!went.has_value()) return {};

            here_ = {here_.line - 1, *went};
        }

        return here_.at;
    }

    std::size_t go = from.at;

    for (std::size_t one = from.line; one < which; ++one) {
        if (go >= whole_) {
            counted_ = one;

            return {};
        }

        const Step step = forth({one, go}, endless);

        if (step.how == Ended::waiting) return {};

        go = step.extent.next;
    }

    if (go >= whole_) {
        counted_ = which;

        return {};
    }

    here_ = {which, go};

    ahead(go);

    return go;
}

// The whole extent of line `which`: where it starts, where its content ends and where the next one
// starts; nothing past the end or while a window is on its way. The line is walked to its end once
// and remembered, so a screen asking about the same lines on every frame walks nothing.
std::optional<Spool::Line> Spool::line(std::size_t which) {
    stalled_ = false;

    if (const Known* got = known(which); got != nullptr) return got->extent;

    const std::optional<std::size_t> from = at(which);

    if (!from.has_value()) return {};

    const Step step = forth({which, *from}, endless);

    if (step.how == Ended::waiting) return {};

    ahead(step.extent.next);

    return step.extent;
}

// The inverse of `at`: which line contains this byte. The walk starts from the latest known line
// start not past the byte and looks for line ends only up to the byte: the line holding it is known
// as soon as there is no \n between its start and the byte, whatever lies after. Under an ordered
// window it returns the last line it reached; the caller asks again after `awoken`.
std::size_t Spool::found(std::size_t byte) {
    stalled_ = false;

    if (whole_ == 0) return 0;

    const std::size_t want = byte < whole_ ? byte : whole_ - 1;

    if (const Known* got = holding(want); got != nullptr) {
        here_ = {got->line, got->extent.at};

        return got->line;
    }

    Anchor from = start_at(want);

    while (true) {
        const Step step = forth(from, want);

        if (step.how == Ended::waiting) return from.line;

        if (step.how == Ended::short_of || step.extent.next > want) break;

        from = {from.line + 1, step.extent.next};
    }

    here_ = from;

    return from.line;
}

// Up to `much` bytes from `at`, at most a window: fewer at the end of the document, an empty view
// past it, nothing while the window is on its way. With `least`, fewer are enough when at least
// that many are at hand already: whoever reads what has arrived (the head of a file shown before
// the rest comes) does not wait for a whole piece. The view points into the spool's window and is
// valid until the next call to the spool.
std::optional<std::string_view> Spool::read(std::size_t at, std::size_t much, std::size_t least) {
    stalled_ = false;

    if (at >= whole_) return std::string_view{};

    std::size_t take = std::min({much, window, whole_ - at});

    if (take == 0) return std::string_view{};

    const std::size_t end = chunked_ + chunk_.size();

    if (least < take && cached(at, at + least)) take = std::min(take, end - at);
    else if (!ranged(at, take)) return {};

    // The next window is ordered first and the view made after it, so that a window arriving at
    // once cannot leave the view pointing at bytes that moved.
    ahead(at + take);

    if (!cached(at, at + take) && !ranged(at, take)) return {};

    return std::string_view{reinterpret_cast<const char*>(chunk_.data()) + (at - chunked_), take};
}

// Walks to the end to learn the line count: at once for a synchronous source, window by window for
// an asynchronous one, each arriving window pushing the walk further until `counted()` has a value.
// Anchors set on the way stay, so End after counting lands near a known point.
void Spool::counting() {
    if (counted_.has_value()) return;

    counting_ = true;

    (void)at(SIZE_MAX);
}

// Learns the line count from whoever read the document to its end from a known line, as a page
// does when its last row ends the document: then nobody has to walk it again to know.
void Spool::counted(std::size_t lines) {
    if (!counted_.has_value()) counted_ = lines;
}

// Sets the handler called on the main thread whenever a window arrives.
void Spool::awoken(std::function<void()> given) { awoken_ = std::move(given); }

}
