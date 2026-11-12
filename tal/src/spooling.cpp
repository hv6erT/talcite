#include "spooling.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

// A spool: a byte document presented as lines. It goes forward and back through a document that
// cannot be held whole, and never indexes it: it remembers a window of bytes, the place where it
// stands (`here_`), a handful of anchors (a line and the byte where it starts) and the extents of
// the lines it walked last.
//
// It hands out bytes and the bounds of lines, never the text of a whole line. A line may be a
// gigabyte long; whoever draws shows a few dozen characters of it, so it reads those (`read`) and
// nothing more. To know where a line ends the spool walks it to its \n without keeping it. A walk
// stopped by a window that has not come yet (`Walk`, and `Behind` backwards) is kept, and the next
// question goes on from where the last one stopped.
//
// It waits for nothing. A document read from elsewhere tells the bytes it lacks as an order
// (`ordered`), answers with what it has, and `stalled` says that the answer is not whole; the owner
// reads the window and gives it back (`arrived`) and asks again. A document given whole never
// stalls.
namespace tal::spooling {

namespace {

// The window of bytes read at once: a few hundred lines of ordinary text, so that stepping with an
// arrow almost never goes back to the source. It is also the most `read` hands out at once.
constexpr std::size_t window = 64 * 1024;

// How many bytes of windows coming one after another are kept; past twice as many the oldest fall
// off the front, so that walking the document to its end does not pull it in whole.
constexpr std::size_t stock = 8 * window;

// The most anchors kept: past them every other one goes and the stride doubles, so that their
// memory stays flat in a document of any size.
constexpr std::size_t plenty = 4096;

// The least stride of the anchors.
constexpr std::size_t least = 64;

// How many extents of lines are kept around the last one walked: a screen asks for the same lines
// at every frame.
constexpr std::size_t remembered = 1024;

// A walk forward with no limit: to the end of the line, wherever it is.
constexpr Offset endless = UINT64_MAX;

// The bytes `got` as characters.
std::string_view viewed(const Bytes& got, std::size_t from, std::size_t much) {
    return std::string_view{reinterpret_cast<const char*>(got.data()) + from, much};
}

}

// A document given whole: the window is the document, nothing is read and nothing ordered, and
// the lines are counted at once.
Spool::Spool(Bytes whole) : held_(true), whole_(whole.size()), chunk_(std::move(whole)) {
    anchors_.push_back({0, 0});

    Offset at = 0;
    std::size_t which = 0;

    while (at < whole_) {
        at = forth({which, at}, endless).extent.next;

        ++which;
    }

    counted_ = which;
}

// A document of `whole` bytes read in windows, the first bytes of it already at hand in `head`, so
// that the first question has an answer without an order.
Spool::Spool(Offset whole, Bytes head) : whole_(whole), chunk_(std::move(head)) {
    anchors_.push_back({0, 0});

    if (whole_ == 0) counted_ = 0;
}

// Whether the bytes from `at` up to `upto` are all in the window.
bool Spool::cached(Offset at, Offset upto) const { return at >= chunked_ && upto <= chunked_ + chunk_.size(); }

// Whether the `much` bytes from `at`, a place in the document, are in the window, as many as the
// document has; when not, the spool stalls and orders them.
bool Spool::ranged(Offset at, std::size_t much) {
    const Offset upto = std::min<Offset>(at + much, whole_);

    if (cached(at, upto)) return true;

    stalled_ = true;

    ask(at, upto);

    return false;
}

// Orders a window from where the bytes up to `upto` are missing: when the start is in the window
// already, from its end, so that the window grows instead of starting over. One order goes at a
// time; a question that finds one on its way asks again when it came.
void Spool::ask(Offset at, Offset upto) {
    if (held_ || busy_ || order_) return;

    const Offset end = chunked_ + chunk_.size();
    const Offset from = at >= chunked_ && at <= end ? end : at;

    order_ = Order{from, std::max<std::size_t>(upto > from ? static_cast<std::size_t>(upto - from) : 0, window)};
}

// The window to read, when there is one: from then on it is on its way, and nothing more is ordered
// until it comes.
std::optional<Spool::Order> Spool::ordered() {
    const std::optional<Order> made = order_;

    if (made) busy_ = true;

    order_.reset();

    return made;
}

// The window read from `from` has come. It joins the window when it goes on from its end, otherwise
// it takes its place; past two stocks the window is cut to one, so that its bytes are moved once
// per stock read, not at every window. An empty window before the end means the document is shorter
// than it said (it shrank, or reading failed): it ends there. A walk counting the lines goes on.
void Spool::arrived(Offset from, Bytes got) {
    busy_ = false;

    if (got.empty()) {
        whole_ = std::min(whole_, from);
    } else if (from == chunked_ + chunk_.size()) {
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

    if (counting_ && !counted_) (void)at(SIZE_MAX);
}

// Orders the next window before anyone asks for it, when what was just used, at `from` in the
// window, lies less than half a window before its end: reading on then goes without a stop.
void Spool::ahead(Offset from) {
    if (held_ || busy_ || order_) return;

    const Offset end = chunked_ + chunk_.size();

    if (end < whole_ && end - from < window / 2) ask(end, end + window);
}

// The byte at `at` of the document, which lies in the window.
char Spool::sign(Offset at) const { return static_cast<char>(chunk_[static_cast<std::size_t>(at - chunked_)]); }

// Walks the line starting at `from` forward to its \n, looking at the bytes before `limit` only:
// `line` when its end was found (the extent is then kept), `short_of` when no line end lies before
// `limit`, `waiting` when a window was ordered. A walk of the same line goes on from where the last
// one stopped. The content ends before the \n and before a \r right in front of it, and the last
// line ends at the end of the document, without a \r there too.
Spool::Step Spool::forth(Anchor from, Offset limit) {
    if (const Known* got = known(from.line)) return Step{Ended::line, got->extent};

    Offset go = from.at;
    bool carriage = false;

    if (walk_ && walk_->from.line == from.line && walk_->reached > go) {
        go = walk_->reached;
        carriage = walk_->carriage;
    }

    const Offset stop = std::min(limit, whole_);

    while (go < stop) {
        if (!ranged(go, 1)) {
            walk_ = Walk{from, go, carriage};

            return Step{Ended::waiting, {}};
        }

        const Offset upto = std::min<Offset>(chunked_ + chunk_.size(), stop);

        for (Offset which = go; which < upto; ++which) {
            const char one = sign(which);

            if (one == '\n') {
                const Line made{from.at, carriage ? which - 1 : which, which + 1};

                remember(from.line, made);

                walk_.reset();

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

    remember(from.line, made);

    walk_.reset();

    return Step{Ended::line, made};
}

// The start of the line before the one starting at `at`, or none while a window is on its way.
// Backwards a line cannot be counted, only sought: the last \n before the one ending the line
// before, looked for in what the window holds and then in the window before it, the search going
// on where it stopped when a window was missing.
std::optional<Offset> Spool::back(Offset at) {
    if (at == 0) return 0;

    if (const Known* got = holding(at - 1)) return got->extent.at;

    Offset end = at - 1;

    if (behind_ && behind_->from == at) end = std::min(end, behind_->reached);

    while (end > 0) {
        if (!cached(end - 1, end)) {
            const Offset from = end - std::min<Offset>(window, end);

            if (!ranged(from, static_cast<std::size_t>(end - from))) {
                behind_ = Behind{at, end};

                return std::nullopt;
            }
        }

        for (; end > chunked_; --end)
            if (sign(end - 1) == '\n') return end;
    }

    return 0;
}

// The best start known for walking forward to the line `which`: of the nearest anchor, the line the
// spool reached last and the line after the last extent kept before it, the latest not past it.
// Anchors may lie far apart, and the lines walked since one may be kept no longer, so that walking
// from the anchor would read them again.
Spool::Anchor Spool::start_for(std::size_t which) const {
    const auto past = std::upper_bound(anchors_.begin(), anchors_.end(), which, [](std::size_t want, const Anchor& one) { return want < one.line; });
    Anchor made = *std::prev(past);

    const auto better = [&made, which](Anchor one) {
        if (one.line <= which && one.line > made.line) made = one;
    };

    better(here_);

    const auto kept = std::lower_bound(known_.begin(), known_.end(), which, [](const Known& one, std::size_t want) { return one.line < want; });

    if (kept != known_.begin()) better({std::prev(kept)->line + 1, std::prev(kept)->extent.next});

    return made;
}

// The best start known for walking forward to the line holding the byte `byte`: the latest start
// of a line not past it among the same.
Spool::Anchor Spool::start_at(Offset byte) const {
    const auto past = std::upper_bound(anchors_.begin(), anchors_.end(), byte, [](Offset want, const Anchor& one) { return want < one.at; });
    Anchor made = *std::prev(past);

    const auto better = [&made, byte](Anchor one) {
        if (one.at <= byte && one.at > made.at) made = one;
    };

    better(here_);

    const auto after = std::upper_bound(known_.begin(), known_.end(), byte, [](Offset want, const Known& one) { return want < one.extent.next; });

    if (after != known_.begin()) better({std::prev(after)->line + 1, std::prev(after)->extent.next});

    return made;
}

// Keeps that the line `which` starts at `at`, when the line falls on the stride past the last
// anchor; with too many anchors every other one goes and the stride doubles.
void Spool::anchored(std::size_t which, Offset at) {
    if (which % stride_ != 0 || anchors_.back().line >= which) return;

    anchors_.push_back({which, at});

    if (anchors_.size() <= plenty) return;

    std::vector<Anchor> kept;

    kept.reserve(anchors_.size() / 2 + 1);

    for (std::size_t one = 0; one < anchors_.size(); one += 2) kept.push_back(anchors_[one]);

    anchors_ = std::move(kept);
    stride_ *= 2;
}

// Keeps the whole extent of the line `which`, found by a walk: the next line's start becomes an
// anchor, and the count of lines is known when this one ends the document. Past twice the extents
// kept, only those nearest this line stay, since the next questions come from around it.
void Spool::remember(std::size_t which, Line extent) {
    anchored(which + 1, extent.next);

    if (extent.next >= whole_) counted_ = which + 1;

    const auto at = std::lower_bound(known_.begin(), known_.end(), which, [](const Known& one, std::size_t want) { return one.line < want; });

    if (at != known_.end() && at->line == which) {
        at->extent = extent;

        return;
    }

    const auto put = known_.insert(at, Known{which, extent});
    const auto middle = static_cast<std::size_t>(put - known_.begin());

    if (known_.size() <= 2 * remembered) return;

    const std::size_t from = middle > remembered / 2 ? middle - remembered / 2 : 0;
    const std::size_t upto = std::min(from + remembered, known_.size());

    known_.erase(known_.begin() + static_cast<std::ptrdiff_t>(upto), known_.end());
    known_.erase(known_.begin(), known_.begin() + static_cast<std::ptrdiff_t>(from));
}

// The extent kept of the line `which`, or none.
const Spool::Known* Spool::known(std::size_t which) const {
    const auto at = std::lower_bound(known_.begin(), known_.end(), which, [](const Known& one, std::size_t want) { return one.line < want; });

    return at != known_.end() && at->line == which ? &*at : nullptr;
}

// The line kept that holds the byte `byte`, from its start up to its \n, or none.
const Spool::Known* Spool::holding(Offset byte) const {
    const auto after = std::upper_bound(known_.begin(), known_.end(), byte, [](Offset want, const Known& one) { return want < one.extent.at; });

    if (after == known_.begin()) return nullptr;

    const Known& one = *std::prev(after);

    return byte < one.extent.next ? &one : nullptr;
}

// Sets the stride of the anchors from the height of the screen, since a jump is mostly a page,
// Home, End or the thumb dragged; it never shrinks, as anchors thinned cannot grow dense again.
void Spool::screen(int high) { stride_ = std::max({stride_, least, static_cast<std::size_t>(std::max(high, 0))}); }

// The byte where the line `which` starts: none past the end, or while a window is on its way
// (`stalled`). Of walking forward from the best start known and stepping back from where the spool
// stands the shorter way is taken: for the up arrow one step back.
std::optional<Offset> Spool::at(std::size_t which) {
    stalled_ = false;

    if (counted_ && which >= *counted_) return std::nullopt;

    if (const Known* got = known(which)) return got->extent.at;

    const Anchor from = start_for(which);

    if (which < here_.line && here_.line - which <= which - from.line) {
        while (here_.line > which) {
            const std::optional<Offset> went = back(here_.at);

            if (!went) return std::nullopt;

            here_ = {here_.line - 1, *went};
        }

        return here_.at;
    }

    Offset go = from.at;

    for (std::size_t one = from.line; one < which; ++one) {
        if (go >= whole_) return std::nullopt;

        const Step step = forth({one, go}, endless);

        if (step.how == Ended::waiting) return std::nullopt;

        go = step.extent.next;
    }

    if (go >= whole_) return std::nullopt;

    here_ = {which, go};

    return go;
}

// The whole extent of the line `which`; none past the end or while a window is on its way. The line
// is walked to its end once and kept.
std::optional<Spool::Line> Spool::line(std::size_t which) {
    stalled_ = false;

    if (const Known* got = known(which); got != nullptr) return got->extent;

    const std::optional<Offset> from = at(which);

    if (!from) return std::nullopt;

    const Step step = forth({which, *from}, endless);

    if (step.how == Ended::waiting) return std::nullopt;

    return step.extent;
}

// The line holding the byte `byte` (the last line for a byte past the end): walked from the latest
// start known not past it, looking for line ends only up to the byte. While a window is on its way,
// the last line reached; the caller asks again.
std::size_t Spool::found(Offset byte) {
    stalled_ = false;

    if (whole_ == 0) return 0;

    const Offset want = std::min(byte, whole_ - 1);

    if (const Known* got = holding(want)) return got->line;

    Anchor from = start_at(want);

    while (true) {
        const Step step = forth(from, want);

        if (step.how == Ended::waiting) return from.line;

        if (step.how == Ended::short_of || step.extent.next > want) break;

        from = {from.line + 1, step.extent.next};
    }

    return from.line;
}

// Up to `much` bytes from `at`, a window at most: fewer at the end, an empty view past it, none
// while the window is on its way. With `least`, fewer are enough when that many are at hand, so
// that the head of a file shows before the rest comes. The view lies in the spool's window and
// holds until the next call to the spool.
std::optional<std::string_view> Spool::read(Offset at, std::size_t much, std::size_t least) {
    stalled_ = false;

    if (at >= whole_) return std::string_view{};

    std::size_t take = static_cast<std::size_t>(std::min<Offset>({much, window, whole_ - at}));

    if (least < take && cached(at, at + least)) take = static_cast<std::size_t>(std::min<Offset>(take, chunked_ + chunk_.size() - at));
    else if (!ranged(at, take)) return std::nullopt;

    ahead(at + take);

    return viewed(chunk_, static_cast<std::size_t>(at - chunked_), take);
}

// Walks to the end to count the lines: at once for a document at hand, window by window otherwise,
// each window that comes taking the walk further.
void Spool::counting() {
    counting_ = true;

    (void)at(SIZE_MAX);
}

// Learns the count of lines from whoever read the document to its end from a known line, as a page
// does when its last row ends the document.
void Spool::counted(std::size_t lines) {
    if (!counted_) counted_ = lines;
}

}
