#include "spool.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>

// A spool: a byte source presented as lines. It scrolls forward and back through a document that
// cannot be held whole, and never indexes it: it remembers only a window of bytes, the place where
// it stands (`here_`), and a handful of anchors (line number and byte where that line starts).
//
// It knows nothing of the window width, the skin or the tree. It does not expand tabs and does not
// wrap: it returns the raw line without the \n and \r at its end. Everything about drawing stays in
// `Viewer`.
//
// It does not wait for bytes. A `Feed` returns a promise: a window that is not at hand is ordered,
// and the spool answers with what it has (fewer lines, or nothing), while `stalled()` says that the
// answer is incomplete because it waits for the disk. The window arrives on the main thread, and
// then the `awoken` handler is called; whoever asked asks again. A document given as bytes never
// stalls. `whole()` is the document's length in bytes, and `counted()` the number of lines once it
// is known.
//
// `Peek` is an internal seam, not part of the library's API: a look into bytes that already exist.
// The editor reads through its quilt, which lies in memory and answers at once, and the editor's
// spool must not stall on the cursor. It is not a source but a value in pieces that cannot be given
// as one pointer. No public entry accepts `Peek`: a source outside memory is always a promise in
// talcite (`Feed`), and the public `Fetch` is a pixel source, also a promise.
//
// `Want` is the byte range a stalled request needs, kept until a window arrives.
namespace talcite {

namespace {

// The window of bytes kept at hand. One call to the source covers a few hundred lines of ordinary
// text, so scrolling with an arrow almost never goes back to it.
constexpr std::size_t window = 64 * 1024;

// How many bytes of windows may be kept at once when they arrive one after another: further ones
// push the oldest out at the front. Without a limit, walking the document to its end would pull it
// in whole.
constexpr std::size_t stock = 8 * window;

// The upper limit of a line's length. Without it, a file without a single \n (and every binary file
// is like that) would be sucked whole into one string at the first opening. Past the limit we treat
// the line as ending there: a document without line ends falls into equal pieces and nothing
// hangs.
constexpr std::size_t brim = 1024 * 1024;

// Anchors cannot keep multiplying, because in a large document they alone would take more than the
// window. When full, every other one is dropped and the stride doubles: memory stays flat, and the
// walk from an anchor grows only where there is a long jump to a place never visited anyway.
constexpr std::size_t plenty = 4096;

// The floor of the stride. With a window three rows high, an anchor every three lines would be pure
// overhead.
constexpr std::size_t least = 64;

}

// A spool over an asynchronous source, possibly warmed up. `head` is the bytes from the start of
// the document that the caller already has (the header window, read to learn the file type), so
// window zero is there at once and the first request for lines answers immediately, without an
// order and without a turn of the loop. Everything after that (fetching more, counting lines,
// anchors, `stalled()`) works as usual; only where the first window came from differs. An empty
// head gives a cold spool. Cutting the head to the document and to `stock` is a guard on memory, not
// a rule: `whole_` limits every read anyway, but there is no point keeping bytes nobody will read.
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
        at = forth(at, nullptr);

        ++which;

        anchored(which, at);
    }

    counted_ = which;
}

// Whether the bytes [at, upto) are all in the current window.
bool Spool::cached(std::size_t at, std::size_t upto) const {
    return at >= chunked_ && upto <= chunked_ + chunk_.size();
}

// Makes sure the whole range of `much` bytes from `at` lies in the window; returns whether it does.
// Reading backward scans from the end toward the start, so hitting just the start of the range is
// not enough: we would skip a tail left outside the previous window and find the wrong line end.
//
// The internal seam (`Peek`, the editor's quilt) may return less than asked, so we fetch in a loop
// until the range is full or the source stops giving anything. Taking one short answer as the end
// of the document would make the line count random and cut the document off halfway.
//
// A source (`Feed`) gives nothing at once: we order the window, record that the answer is
// incomplete, and return false. Whoever asked gets less and asks again when the window arrives.
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
                       .fail([weak, from](const std::exception&) {
                           // The source refused: the document ends where we got to. Otherwise
                           // every paint would order the same window again, forever.
                           if (const std::shared_ptr<Spool> mine = weak.lock()) mine->arrived(from, {});
                       }));

    ordered_->start();
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
            chunk_.insert(chunk_.end(), got.begin(), got.end());

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

// The byte at document offset `at`, which must lie in the window.
char Spool::sign(std::size_t at) const { return static_cast<char>(chunk_[at - chunked_]); }

// Returns the byte where the next line starts. When `into` is not null, it collects the current
// line's content on the way, without the \n and \r at its end. When a window has been ordered but
// is not here, it returns the place it got to; the caller checks `stalled_` and does not trust the
// result.
std::size_t Spool::forth(std::size_t at, std::string* into) {
    if (into != nullptr) into->clear();

    const auto trimmed = [into] {
        if (into != nullptr && !into->empty() && into->back() == '\r') into->pop_back();
    };

    std::size_t go = at;
    std::size_t took = 0;

    while (go < whole_) {
        if (!ranged(go, 1)) break;

        const std::size_t upto = std::min(chunked_ + chunk_.size(), whole_);

        for (std::size_t which = go; which < upto; ++which) {
            if (sign(which) == '\n') {
                trimmed();

                return which + 1;
            }

            if (took >= brim) {
                trimmed();

                return which;
            }

            if (into != nullptr) into->push_back(sign(which));

            ++took;
        }

        go = upto;
    }

    trimmed();

    return stalled_ ? go : whole_;
}

// The start of the line before the one starting at `at`. Backward one cannot count, only search: we
// look for the last \n before the one that ends the preceding line, scanning in pieces from the
// end toward the start. The order matters: we want the last one, so the region is walked from the
// back, not to the first hit.
std::size_t Spool::back(std::size_t at) {
    if (at == 0) return 0;

    // `at` points at the start of a line, so the byte just before it is the \n ending the previous
    // one. That byte itself is not examined.
    const std::size_t upto = at - 1;

    std::size_t end = upto;

    while (end > 0) {
        const std::size_t step = std::min(window, end);
        const std::size_t from = end - step;

        if (!ranged(from, step)) break;

        for (std::size_t which = end; which > from; --which)
            if (sign(which - 1) == '\n') return which;

        if (upto - from >= brim) return upto - brim;

        end = from;
    }

    return 0;
}

// The last anchor not past line `which`. Anchors are ordered by line, so binary search finds it.
// Anchor zero always exists, so there is always a result.
Spool::Anchor Spool::nearest(std::size_t which) const {
    const auto past = std::upper_bound(anchors_.begin(), anchors_.end(), which,
                                       [](std::size_t want, const Anchor& one) { return want < one.line; });

    return *std::prev(past);
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

// Sets the anchor stride from the screen height, because a jump is almost always a page, Home, End
// or a drag of the scrollbar, and then each lands right next to a known point. It never shrinks:
// anchors once thinned have no way to grow dense again, and going back and forth would cost the
// thinning itself.
void Spool::screen(int high) {
    const std::size_t want = high > 0 ? static_cast<std::size_t>(high) : 0;

    stride_ = std::max(stride_, std::max(least, want));
}

// A change in one place: `gone` bytes from `at` replaced by `came`. Anchors past the changed stretch
// move by the difference in bytes and lines: a line that started there still starts, just
// elsewhere. Dropping every anchor past the change would make each key pressed at a block far into
// the file recount the lines from the change to the block, megabytes per key in a large file. Only
// anchors inside the deleted stretch die (after its start, up to and including its end): their line
// may have vanished or may start elsewhere. An anchor exactly at `at` stays: the byte before it did
// not change, so the line still starts there. The same goes for the place where the spool stands.
// The byte window is discarded whole: it holds the bytes from before the change.
//
// `lines` is the difference in line ends; the caller knows it locally, because it knows what it
// inserted and what it deleted. The line count, if known, is corrected by it; if unknown, it stays
// unknown. The difference may be adjusted for whether the last line exists, but that concerns only
// changes at the very end of the document, and there are no anchors past the end.
void Spool::changed(std::size_t at, std::size_t gone, std::size_t came, std::ptrdiff_t lines) {
    whole_ = whole_ - gone + came;

    chunk_.clear();
    chunked_ = 0;

    const std::size_t end = at + gone;

    const auto moved = [&](Anchor& one) {
        if (one.at <= at || one.line == 0) return true;

        if (one.at <= end) return false;

        one.at = one.at - gone + came;
        one.line = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(one.line) + lines);

        return true;
    };

    const auto kept = std::remove_if(anchors_.begin(), anchors_.end(), [&moved](Anchor& one) { return !moved(one); });

    anchors_.erase(kept, anchors_.end());

    if (!moved(here_)) here_ = {0, 0};

    if (counted_.has_value())
        counted_ = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(*counted_) + lines);
}

// A change in many places at once ("replace all"), the first of them at `at`, with `bytes` and
// `lines` the total differences: the cuts move different stretches by different amounts, so
// everything past the first one is discarded. Nothing before the first cut has moved.
void Spool::rewritten(std::size_t at, std::ptrdiff_t bytes, std::ptrdiff_t lines) {
    whole_ = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(whole_) + bytes);

    chunk_.clear();
    chunked_ = 0;

    const auto kept = std::remove_if(anchors_.begin(), anchors_.end(),
                                     [at](const Anchor& one) { return one.at > at && one.line > 0; });

    anchors_.erase(kept, anchors_.end());

    if (here_.at > at) here_ = {0, 0};

    if (counted_.has_value())
        counted_ = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(*counted_) + lines);
}

// The byte where line `which` starts; nothing means "past the end", or "not known yet" when
// `stalled()`. Under an ordered window nothing is recorded: `here_` and the line count stay as they
// were, because a walk over absent bytes means nothing. Anchors set before the stall stay: they
// were computed from real bytes.
std::optional<std::size_t> Spool::at(std::size_t which) {
    stalled_ = false;

    if (counted_.has_value() && which >= *counted_) return {};

    if (which == here_.line) return here_.at;

    // Of the two ways we take the shorter: forward from an anchor, or back from where we stand.
    // For the up arrow this is always one step back.
    if (which < here_.line && here_.line - which <= which - nearest(which).line) {
        std::size_t go = here_.at;

        for (std::size_t one = here_.line; one > which; --one) {
            go = back(go);

            if (stalled_) return {};
        }

        here_ = {which, go};

        return go;
    }

    Anchor from = nearest(which);

    if (which > here_.line && here_.line > from.line) from = here_;

    std::size_t go = from.at;

    for (std::size_t one = from.line; one < which; ++one) {
        if (go >= whole_) {
            counted_ = one;

            return {};
        }

        go = forth(go, nullptr);

        if (stalled_) return {};

        anchored(one + 1, go);
    }

    if (go >= whole_) {
        counted_ = which;

        return {};
    }

    here_ = {which, go};

    return go;
}

// The inverse of `at`: which line contains this byte. Anchors are ordered both by line number and by
// byte, so we start from the last one not past the place sought, found by binary search as in
// `nearest`. Under an ordered window it returns the last line it reached; the caller asks again
// after `awoken`.
std::size_t Spool::found(std::size_t at) {
    stalled_ = false;

    if (whole_ == 0) return 0;

    const std::size_t want = at < whole_ ? at : whole_ - 1;

    const auto past = std::upper_bound(anchors_.begin(), anchors_.end(), want,
                                       [](std::size_t byte, const Anchor& one) { return byte < one.at; });

    Anchor from = *std::prev(past);

    if (here_.at <= want && here_.at > from.at) from = here_;

    std::size_t go = from.at;
    std::size_t which = from.line;

    while (go < whole_) {
        const std::size_t next = forth(go, nullptr);

        if (stalled_) return which;

        if (next > want) break;

        go = next;

        ++which;

        anchored(which, go);
    }

    here_ = {which, go};

    return which;
}

// Up to `most` lines from line `which`: as many as asked, or fewer because the document ended or a
// window has not arrived yet (`stalled()`); nothing instead of a vector when not even the first one
// is there. A line interrupted by a missing window is left out: it would come in cut off and be
// different at the next request.
//
// When the window ends just past what we return, the next one is ordered right away, without
// waiting to be asked: scrolling down then goes without stops, because the window is already on
// its way when the user presses the arrow.
std::optional<std::vector<std::string>> Spool::lines(std::size_t which, std::size_t most) {
    std::vector<std::string> made;

    const std::optional<std::size_t> from = at(which);

    if (!from.has_value()) {
        if (stalled_) return {};

        return made;
    }

    made.reserve(most);

    std::size_t go = *from;
    std::size_t began = *from;
    std::size_t one = which;
    std::string said;

    while (made.size() < most && go < whole_) {
        const std::size_t next = forth(go, &said);

        if (stalled_) break;

        began = go;
        go = next;

        made.push_back(said);

        ++one;

        anchored(one, go);
    }

    if (!stalled_ && go >= whole_) counted_ = one;

    if (!made.empty()) here_ = {one - 1, began};

    if (made.empty() && stalled_) return {};

    if (feed_ && !stalled_ && !busy_ && go < whole_) {
        const std::size_t end = chunked_ + chunk_.size();

        if (end < whole_ && end - go < window / 2) ask(end, end + window);
    }

    return made;
}

// Walks to the end to learn the line count: at once for a synchronous source, window by window for
// an asynchronous one, each arriving window pushing the walk further until `counted()` has a value.
// Anchors set on the way stay, so End after counting lands near a known point.
void Spool::counting() {
    if (counted_.has_value()) return;

    counting_ = true;

    (void)at(SIZE_MAX);
}

// Sets the handler called on the main thread whenever a window arrives.
void Spool::awoken(std::function<void()> given) { awoken_ = std::move(given); }

}
