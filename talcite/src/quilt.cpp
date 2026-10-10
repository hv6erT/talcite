#include "quilt.hpp"

#include <algorithm>
#include <utility>

namespace talcite {

namespace {

// Whether piece `after` starts in its source exactly where `before` ends; then the two are one.
bool follows(const Quilt::Piece& before, const Quilt::Piece& after) {
    return before.added == after.added && before.at + before.much == after.at;
}

}

// A quilt sewn from pieces: a document that can be changed without rewriting it. The document is a
// chain of pieces (`Piece`), each of which says "this many bytes from the original, starting at X"
// or "this many bytes from the additions, starting at Y". Typing a character cuts one piece into
// three, deleting cuts and throws out. Nobody ever touches the original: it lies in memory as it
// came, and the changes live beside it. The memory for changes grows with their number, not with
// the size of the file, and undo is cheap, because a piece goes back into place. Word and VS Code
// do the same, and not by accident: it is the only structure that gives both at once.
//
// The original is in memory by choice: the editor reads bytes at every cursor move and cannot wait
// for a window from disk. A source outside memory is only a promise in talcite, and a quilt over a
// promise (with a window and a "busy" state in the editor) is a separate step for a document
// larger than memory.
//
// The whole thing is also a plain byte source: it hands out bytes from a given offset, which is
// exactly what `Spool` wants. Thanks to that, lines, wrapping, marks and the scrollbar do not know
// that the document can change. The quilt starts as one piece covering the whole original.
Quilt::Quilt(Bytes given) : held_(std::move(given)), whole_(held_.size()) {
    if (whole_ > 0) pieces_.push_back(Piece{false, 0, whole_});
}

// The index of the piece in which byte `at` lies (the last one starting no further than `at`),
// found by binary search over the piece starts. The starts are computed lazily: a change marks them
// stale and the first question after it computes them again. A change shifts the vector of pieces
// anyway, so recomputing at the first read after it does not change its cost, and there are many
// reads per change.
std::size_t Quilt::located(std::size_t at) {
    if (stale_) {
        starts_.resize(pieces_.size());

        std::size_t gone = 0;

        for (std::size_t which = 0; which < pieces_.size(); ++which) {
            starts_[which] = gone;
            gone += pieces_[which].much;
        }

        stale_ = false;
    }

    const auto after = std::upper_bound(starts_.begin(), starts_.end(), at);

    return after == starts_.begin() ? 0 : static_cast<std::size_t>(after - starts_.begin()) - 1;
}

// Returns the index of the piece that starts exactly at `at`, cutting the piece into whose middle
// it falls. For the end of the document it returns the index past the last one.
std::size_t Quilt::split(std::size_t at) {
    if (at >= whole_) return pieces_.size();

    const std::size_t which = located(at);
    const std::size_t gone = starts_[which];

    if (gone == at) return which;

    const Piece one = pieces_[which];
    const std::size_t took = at - gone;

    pieces_[which] = Piece{one.added, one.at, took};

    pieces_.insert(pieces_.begin() + static_cast<std::ptrdiff_t>(which) + 1,
                   Piece{one.added, one.at + took, one.much - took});

    stale_ = true;

    return which + 1;
}

// At most `most` bytes from `at`: the first piece is found by binary search, then the following
// ones, each with a single copy.
Bytes Quilt::bytes(std::size_t at, std::size_t most) {
    Bytes made;

    if (at >= whole_ || most == 0) return made;

    const std::size_t upto = std::min(at + most, whole_);

    made.reserve(upto - at);

    const std::size_t first = located(at);

    std::size_t gone = starts_[first];

    for (std::size_t which = first; which < pieces_.size() && gone < upto; ++which) {
        const Piece& one = pieces_[which];
        const std::size_t ends = gone + one.much;

        const std::size_t from = at > gone ? at - gone : 0;
        const std::size_t want = std::min(one.much, upto - gone) - from;

        if (one.added) {
            const auto* said = reinterpret_cast<const std::byte*>(added_.data()) + one.at + from;

            made.insert(made.end(), said, said + want);
        } else {
            const auto begin = held_.begin() + static_cast<std::ptrdiff_t>(one.at + from);

            made.insert(made.end(), begin, begin + static_cast<std::ptrdiff_t>(want));
        }

        gone = ends;
    }

    return made;
}

// Inserts `what` at byte `at`: the text goes to the additions and its piece into the document.
void Quilt::insert(std::size_t at, std::string_view what) { put(at, stored(what)); }

// Deletes `much` bytes from `at`, clamped to the end of the document. Only pieces go away; the
// bytes stay where they lie.
void Quilt::erase(std::size_t at, std::size_t much) {
    if (much == 0 || at >= whole_) return;

    const std::size_t took = std::min(much, whole_ - at);

    // We cut the earlier place first, then the later one: the second cut happens past the first,
    // so it does not invalidate the first index.
    const std::size_t from = split(at);
    const std::size_t upto = split(at + took);

    pieces_.erase(pieces_.begin() + static_cast<std::ptrdiff_t>(from),
                  pieces_.begin() + static_cast<std::ptrdiff_t>(upto));

    whole_ -= took;

    stale_ = true;
}

// The pieces under a range, trimmed to its edges, as a scrap: a part of the document given by
// pieces, without bytes. The original and the additions never change, so a reference to them is
// always valid; a scrap taken today can be put back a week later and will be the same text. The
// editor's journal rests on this: a deleted gigabyte is one piece, not a gigabyte of copies. The
// document is not touched, not even cut, because the scrap carries its own copies of the piece
// descriptions, not indices.
Quilt::Scrap Quilt::taken(std::size_t at, std::size_t much) {
    Scrap made;

    if (at >= whole_ || much == 0) return made;

    const std::size_t upto = std::min(at + much, whole_);
    const std::size_t first = located(at);

    std::size_t gone = starts_[first];

    for (std::size_t which = first; which < pieces_.size() && gone < upto; ++which) {
        const Piece& one = pieces_[which];
        const std::size_t from = at > gone ? at - gone : 0;
        const std::size_t want = std::min(one.much, upto - gone) - from;

        made.pieces.push_back(Piece{one.added, one.at + from, want});

        gone += one.much;
    }

    made.much = upto - at;

    return made;
}

// Stores text in the additions as a scrap, but not yet in the document; it will stand wherever
// `put` inserts it. The additions only grow and never shrink. Deleted text stays in them as
// garbage, and that is not an oversight: if they shrank, every piece pointing further would have
// to be corrected, and then the cost of a deletion would depend on the number of changes, not on
// one. `appended()` reports their size, only for tests: they check that undo and moving do not
// copy text, which cannot be seen otherwise.
Quilt::Scrap Quilt::stored(std::string_view what) {
    Scrap made;

    if (what.empty()) return made;

    made.pieces.push_back(Piece{true, added_.size(), what.size()});
    made.much = what.size();

    added_.append(what);

    return made;
}

// Inserts the pieces of a scrap at `at`, without bytes. On both edges a piece merges with its
// neighbor when they lie one after the other in their source: continuous typing extends the last
// piece (without that a paragraph would give five hundred pieces instead of one, and the cost of
// everything above depends on their number), and undoing a deletion sews the document back into
// the same pieces it had before.
void Quilt::put(std::size_t at, const Scrap& what) {
    if (what.much == 0) return;

    // The cut at the insertion point is computed on the document before the insertion; the length
    // and the starts change only afterward.
    const std::size_t which = split(std::min(at, whole_));

    std::size_t first = 0;
    std::size_t last = what.pieces.size();

    if (which > 0 && follows(pieces_[which - 1], what.pieces.front())) {
        pieces_[which - 1].much += what.pieces.front().much;
        ++first;
    }

    if (first < last && which < pieces_.size() && follows(what.pieces[last - 1], pieces_[which])) {
        pieces_[which].at = what.pieces[last - 1].at;
        pieces_[which].much += what.pieces[last - 1].much;
        --last;
    }

    pieces_.insert(pieces_.begin() + static_cast<std::ptrdiff_t>(which),
                   what.pieces.begin() + static_cast<std::ptrdiff_t>(first),
                   what.pieces.begin() + static_cast<std::ptrdiff_t>(last));

    // Everything went into the left neighbor, and that one may now touch the right one.
    if (first == last && which > 0 && which < pieces_.size() && follows(pieces_[which - 1], pieces_[which])) {
        pieces_[which - 1].much += pieces_[which].much;
        pieces_.erase(pieces_.begin() + static_cast<std::ptrdiff_t>(which));
    }

    whole_ += what.much;

    stale_ = true;
}

// The number of line ends in a scrap, counted in the sources, without copying.
std::size_t Quilt::lines(const Scrap& what) const {
    std::size_t made = 0;

    for (const Piece& one : what.pieces) {
        if (one.added) {
            const auto begin = added_.begin() + static_cast<std::ptrdiff_t>(one.at);

            made += static_cast<std::size_t>(std::count(begin, begin + static_cast<std::ptrdiff_t>(one.much), '\n'));
        } else {
            const auto begin = held_.begin() + static_cast<std::ptrdiff_t>(one.at);

            made += static_cast<std::size_t>(
                std::count(begin, begin + static_cast<std::ptrdiff_t>(one.much), std::byte{'\n'}));
        }
    }

    return made;
}

// The bytes of a scrap as a string.
std::string Quilt::text(const Scrap& what) const {
    std::string made;

    made.reserve(what.much);

    for (const Piece& one : what.pieces) {
        if (one.added) {
            made.append(added_, one.at, one.much);
        } else {
            const auto* said = reinterpret_cast<const char*>(held_.data()) + one.at;

            made.append(said, one.much);
        }
    }

    return made;
}

// The last byte of a scrap; zero for an empty one.
char Quilt::last(const Scrap& what) const {
    if (what.pieces.empty()) return '\0';

    const Piece& one = what.pieces.back();

    return one.added ? added_[one.at + one.much - 1] : static_cast<char>(held_[one.at + one.much - 1]);
}

// Adds scrap `more` at the front or the back of `into`, merging the pieces where they meet.
// Deleting backward (a held Backspace) adds at the front one character at a time, and each
// character lies in the source just before the previous one, so the piece grows longer and
// nothing shifts. The vector shifts only at a boundary between document pieces, not at every
// character.
void Quilt::joined(Scrap& into, const Scrap& more, bool front) {
    if (more.much == 0) return;

    std::vector<Piece>& to = into.pieces;
    const std::vector<Piece>& from = more.pieces;

    into.much += more.much;

    if (front) {
        std::size_t upto = from.size();

        if (!to.empty() && follows(from.back(), to.front())) {
            to.front().at = from.back().at;
            to.front().much += from.back().much;
            --upto;
        }

        to.insert(to.begin(), from.begin(), from.begin() + static_cast<std::ptrdiff_t>(upto));

        return;
    }

    std::size_t begin = 0;

    if (!to.empty() && follows(to.back(), from.front())) {
        to.back().much += from.front().much;
        ++begin;
    }

    to.insert(to.end(), from.begin() + static_cast<std::ptrdiff_t>(begin), from.end());
}

// Applies many cuts at once, in one pass over the pieces, for "replace all" and its undo. Each cut
// (`Cut`) replaces `much` bytes from `at` with `text`. The cuts are ascending, disjoint, and at
// positions in the document from before the whole change. The same text of several cuts in a row
// goes to the additions once, and the pieces share it.
//
// The new list of pieces is built in one pass: old pieces before a cut pass through (cut at its
// edges), those under the cut drop out, and a piece of the additions takes its place. The cost is
// the number of pieces plus the number of cuts, not their product, as it would be if every cut
// went separately through `insert` and `erase`, each over the whole list.
void Quilt::spliced(const std::vector<Cut>& cuts) {
    std::vector<Piece> made;
    made.reserve(pieces_.size() + 2 * cuts.size());

    std::size_t which = 0;
    std::size_t gone = 0;
    std::size_t taken = 0;

    // The part of the current piece that has not passed yet runs from `taken` to its end.
    const auto copied = [&](std::size_t upto) {
        while (which < pieces_.size() && gone < upto) {
            const Piece& one = pieces_[which];
            const std::size_t ends = gone + one.much;
            const std::size_t stop = std::min(ends, upto);

            if (stop > gone + taken) made.push_back(Piece{one.added, one.at + taken, stop - gone - taken});

            if (stop == ends) {
                gone = ends;
                taken = 0;
                ++which;
            } else {
                taken = stop - gone;

                return;
            }
        }
    };

    const auto skipped = [&](std::size_t upto) {
        while (which < pieces_.size() && gone + taken < upto) {
            const std::size_t ends = gone + pieces_[which].much;

            if (ends <= upto) {
                gone = ends;
                taken = 0;
                ++which;
            } else {
                taken = upto - gone;

                return;
            }
        }
    };

    std::size_t last_at = 0;
    std::size_t last_much = 0;
    bool had = false;

    std::size_t grown = 0;
    std::size_t shrunk = 0;

    for (const Cut& one : cuts) {
        const std::size_t at = std::min(one.at, whole_);
        const std::size_t much = std::min(one.much, whole_ - at);

        copied(at);
        skipped(at + much);

        shrunk += much;

        if (one.text.empty()) continue;

        if (!had || one.text != std::string_view{added_}.substr(last_at, last_much)) {
            last_at = added_.size();
            last_much = one.text.size();
            added_.append(one.text);
            had = true;
        }

        made.push_back(Piece{true, last_at, one.text.size()});

        grown += one.text.size();
    }

    copied(whole_);

    pieces_ = std::move(made);
    whole_ = whole_ + grown - shrunk;
    stale_ = true;
}

}
