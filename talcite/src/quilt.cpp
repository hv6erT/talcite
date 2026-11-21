#include "quilt.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>
#include <vector>

namespace talcite {

namespace {

// Whether piece `after` starts in its source exactly where `before` ends; then the two are one.
bool follows(const Quilt::Piece& before, const Quilt::Piece& after) {
    return before.added == after.added && before.at + before.much == after.at;
}

// The sizes of a patch, in pieces: one that grows past `fullest` is divided into patches of `half`,
// and one that falls below `thinnest` joins its neighbor when the two fit in one.
constexpr std::size_t fullest = 1024;
constexpr std::size_t half = 512;
constexpr std::size_t thinnest = 256;

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
//
// The chain of pieces lies in patches (`Patch`) of at most `fullest` pieces, each knowing how many
// bytes it holds, and the starts are kept for the patches, not for the pieces. A change touches one
// patch (an insertion into a vector of at most a thousand pieces) and the starts of the patches,
// which are a five-hundredth of the pieces, so a key after "replace all" with a hundred thousand
// hits costs what it costs in a fresh document, not a pass over two hundred thousand pieces.
// Finding a byte is a binary search over the patches and a walk in one. A patch is never empty
// between two calls: `mended` divides, joins and drops them after every change.
Quilt::Quilt(Bytes given) : held_(std::move(given)), whole_(held_.size()) {
    if (whole_ > 0) patches_.push_back(Patch{{Piece{false, 0, whole_}}, whole_});
}

// The piece in which byte `at` lies, before the end of the document: the patch by binary search
// over the patch starts, then the piece by a walk in it, with the byte where the piece starts. The
// starts are computed lazily: a change marks them stale and the first question after it computes
// them again, once for many reads.
Quilt::Spot Quilt::located(std::size_t at) {
    if (stale_) {
        starts_.resize(patches_.size());

        std::size_t gone = 0;

        for (std::size_t which = 0; which < patches_.size(); ++which) {
            starts_[which] = gone;
            gone += patches_[which].much;
        }

        stale_ = false;
    }

    const auto after = std::upper_bound(starts_.begin(), starts_.end(), at);

    Spot made;

    made.patch = after == starts_.begin() ? 0 : static_cast<std::size_t>(after - starts_.begin()) - 1;
    made.gone = starts_[made.patch];

    const std::vector<Piece>& pieces = patches_[made.patch].pieces;

    while (made.piece + 1 < pieces.size() && made.gone + pieces[made.piece].much <= at) {
        made.gone += pieces[made.piece].much;
        ++made.piece;
    }

    return made;
}

// The place of the piece that starts exactly at `at`, cutting the piece into whose middle it falls.
// For the end of the document it is the place past the last piece of the last patch. A cut keeps
// the bytes of its patch, so the starts stay as they are.
Quilt::Spot Quilt::split(std::size_t at) {
    if (at >= whole_) return patches_.empty() ? Spot{} : Spot{patches_.size() - 1, patches_.back().pieces.size(), whole_};

    const Spot made = located(at);

    if (made.gone == at) return made;

    std::vector<Piece>& pieces = patches_[made.patch].pieces;

    const Piece one = pieces[made.piece];
    const std::size_t took = at - made.gone;

    pieces[made.piece] = Piece{one.added, one.at, took};

    pieces.insert(pieces.begin() + static_cast<std::ptrdiff_t>(made.piece) + 1, Piece{one.added, one.at + took, one.much - took});

    return Spot{made.patch, made.piece + 1, at};
}

// Sews the piece at `left` to the one after it, in its patch or at the front of a later one, when
// the two lie one after the other in their source, and says whether it did. The piece after it
// goes; a patch it leaves empty stays for `mended`, so that no place the caller holds moves.
bool Quilt::sewn(Spot left) {
    std::size_t patch = left.patch;
    std::size_t piece = left.piece + 1;

    while (patch < patches_.size() && piece >= patches_[patch].pieces.size()) {
        ++patch;
        piece = 0;
    }

    if (patch >= patches_.size()) return false;

    Piece& before = patches_[left.patch].pieces[left.piece];
    const Piece after = patches_[patch].pieces[piece];

    if (!follows(before, after)) return false;

    before.much += after.much;
    patches_[left.patch].much += after.much;
    patches_[patch].much -= after.much;

    patches_[patch].pieces.erase(patches_[patch].pieces.begin() + static_cast<std::ptrdiff_t>(piece));

    stale_ = true;

    return true;
}

// Mends the patches around `patch` after a change there: from the one after it down to the one
// before it, so that what happens to a later patch does not move an earlier one. An empty patch
// goes, one grown past `fullest` is divided into patches of at most `half`, and one below
// `thinnest` takes in the next one when both fit in a patch.
void Quilt::mended(std::size_t patch) {
    const std::size_t lowest = patch > 0 ? patch - 1 : 0;

    for (std::size_t one = patch + 2; one-- > lowest;) {
        if (one >= patches_.size()) continue;

        std::vector<Piece>& pieces = patches_[one].pieces;

        if (pieces.empty()) {
            patches_.erase(patches_.begin() + static_cast<std::ptrdiff_t>(one));
            stale_ = true;

            continue;
        }

        if (pieces.size() > fullest) {
            // Into parts of nearly the same size, none above `half`, so that no tiny patch is left.
            const std::size_t parts = (pieces.size() + half - 1) / half;
            const std::size_t each = (pieces.size() + parts - 1) / parts;

            std::vector<Patch> divided;

            for (std::size_t from = 0; from < pieces.size(); from += each) {
                Patch made;

                made.pieces.assign(pieces.begin() + static_cast<std::ptrdiff_t>(from),
                                   pieces.begin() + static_cast<std::ptrdiff_t>(std::min(from + each, pieces.size())));

                for (const Piece& each : made.pieces) made.much += each.much;

                divided.push_back(std::move(made));
            }

            patches_.erase(patches_.begin() + static_cast<std::ptrdiff_t>(one));
            patches_.insert(patches_.begin() + static_cast<std::ptrdiff_t>(one), std::make_move_iterator(divided.begin()),
                            std::make_move_iterator(divided.end()));
            stale_ = true;

            continue;
        }

        if (pieces.size() < thinnest && one + 1 < patches_.size() && pieces.size() + patches_[one + 1].pieces.size() <= fullest) {
            Patch& next = patches_[one + 1];

            pieces.insert(pieces.end(), next.pieces.begin(), next.pieces.end());
            patches_[one].much += next.much;

            patches_.erase(patches_.begin() + static_cast<std::ptrdiff_t>(one) + 1);
            stale_ = true;
        }
    }
}

// At most `most` bytes from `at`: the first piece is found by binary search, then the following
// ones, patch after patch, each with a single copy.
Bytes Quilt::bytes(std::size_t at, std::size_t most) {
    Bytes made;

    if (at >= whole_ || most == 0) return made;

    const std::size_t upto = std::min(at + most, whole_);

    made.reserve(upto - at);

    const Spot first = located(at);

    std::size_t gone = first.gone;

    for (std::size_t patch = first.patch, piece = first.piece; patch < patches_.size() && gone < upto;) {
        const Piece& one = patches_[patch].pieces[piece];

        const std::size_t from = at > gone ? at - gone : 0;
        const std::size_t want = std::min(one.much, upto - gone) - from;

        if (one.added) {
            const auto* said = reinterpret_cast<const std::byte*>(added_.data()) + one.at + from;

            made.insert(made.end(), said, said + want);
        } else {
            const auto begin = held_.begin() + static_cast<std::ptrdiff_t>(one.at + from);

            made.insert(made.end(), begin, begin + static_cast<std::ptrdiff_t>(want));
        }

        gone += one.much;

        if (++piece == patches_[patch].pieces.size()) {
            ++patch;
            piece = 0;
        }
    }

    return made;
}

// Inserts `what` at byte `at`: the text goes to the additions and its piece into the document.
void Quilt::insert(std::size_t at, std::string_view what) { put(at, stored(what)); }

// Deletes `much` bytes from `at`, clamped to the end of the document. Only pieces go away; the
// bytes stay where they lie. The range may span patches: the first loses its tail, the last its
// head, and those between go whole.
void Quilt::erase(std::size_t at, std::size_t much) {
    if (much == 0 || at >= whole_) return;

    const std::size_t took = std::min(much, whole_ - at);

    // We cut the earlier place first, then the later one: the second cut happens past the first,
    // so it does not move the first place.
    const Spot from = split(at);
    const Spot upto = split(at + took);

    std::vector<Piece>& first = patches_[from.patch].pieces;

    if (from.patch == upto.patch) {
        first.erase(first.begin() + static_cast<std::ptrdiff_t>(from.piece), first.begin() + static_cast<std::ptrdiff_t>(upto.piece));

        patches_[from.patch].much -= took;
    } else {
        std::vector<Piece>& last = patches_[upto.patch].pieces;

        last.erase(last.begin(), last.begin() + static_cast<std::ptrdiff_t>(upto.piece));
        first.erase(first.begin() + static_cast<std::ptrdiff_t>(from.piece), first.end());

        for (const std::size_t one : {from.patch, upto.patch}) {
            patches_[one].much = 0;

            for (const Piece& each : patches_[one].pieces) patches_[one].much += each.much;
        }

        patches_.erase(patches_.begin() + static_cast<std::ptrdiff_t>(from.patch) + 1,
                       patches_.begin() + static_cast<std::ptrdiff_t>(upto.patch));
    }

    whole_ -= took;

    stale_ = true;

    mended(from.patch);
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
    const Spot first = located(at);

    std::size_t gone = first.gone;

    for (std::size_t patch = first.patch, piece = first.piece; patch < patches_.size() && gone < upto;) {
        const Piece& one = patches_[patch].pieces[piece];
        const std::size_t from = at > gone ? at - gone : 0;
        const std::size_t want = std::min(one.much, upto - gone) - from;

        made.pieces.push_back(Piece{one.added, one.at + from, want});

        gone += one.much;

        if (++piece == patches_[patch].pieces.size()) {
            ++patch;
            piece = 0;
        }
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
    Spot where = split(std::min(at, whole_));

    if (patches_.empty()) {
        patches_.emplace_back();
        where = Spot{};
    }

    Patch& patch = patches_[where.patch];

    patch.pieces.insert(patch.pieces.begin() + static_cast<std::ptrdiff_t>(where.piece), what.pieces.begin(), what.pieces.end());
    patch.much += what.much;

    whole_ += what.much;

    stale_ = true;

    // The later seam first, the last piece of the scrap with the one after it, so that the earlier
    // seam, the piece before the scrap with its first, stays where it is. A scrap of one piece that
    // went into both neighbors leaves them sewn together that way; a scrap of two pieces that went
    // one into each leaves its neighbors side by side, and they are sewn once more.
    const bool later = sewn(Spot{where.patch, where.piece + what.pieces.size() - 1, 0});

    if (where.piece > 0 || where.patch > 0) {
        const Spot before = where.piece > 0 ? Spot{where.patch, where.piece - 1, 0}
                                            : Spot{where.patch - 1, patches_[where.patch - 1].pieces.size() - 1, 0};

        if (sewn(before) && later && what.pieces.size() == 2) (void)sewn(before);
    }

    mended(where.patch);
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
// The new chain of pieces is built in one pass, straight into patches of `half`: old pieces before
// a cut pass through (cut at its edges), those under the cut drop out, and a piece of the additions
// takes its place. The cost is the number of pieces plus the number of cuts, not their product, as
// it would be if every cut went separately through `insert` and `erase`.
void Quilt::spliced(const std::vector<Cut>& cuts) {
    std::vector<Patch> made;

    // Lays one piece at the end of the new chain.
    const auto laid = [&made](Piece one) {
        if (made.empty() || made.back().pieces.size() == half) {
            made.emplace_back();
            made.back().pieces.reserve(half);
        }

        made.back().pieces.push_back(one);
        made.back().much += one.much;
    };

    std::size_t patch = 0;
    std::size_t which = 0;
    std::size_t gone = 0;
    std::size_t taken = 0;

    // Goes on to the next piece of the old chain, patch after patch.
    const auto stepped = [&] {
        if (++which == patches_[patch].pieces.size()) {
            ++patch;
            which = 0;
        }
    };

    // Lays the old chain up to byte `upto` into the new one. The part of the current piece that has
    // not passed yet runs from `taken` to its end.
    const auto copied = [&](std::size_t upto) {
        while (patch < patches_.size() && gone < upto) {
            const Piece& one = patches_[patch].pieces[which];
            const std::size_t ends = gone + one.much;
            const std::size_t stop = std::min(ends, upto);

            if (stop > gone + taken) laid(Piece{one.added, one.at + taken, stop - gone - taken});

            if (stop == ends) {
                gone = ends;
                taken = 0;
                stepped();
            } else {
                taken = stop - gone;

                return;
            }
        }
    };

    // Passes over the old chain up to byte `upto` without laying it: what a cut takes away.
    const auto skipped = [&](std::size_t upto) {
        while (patch < patches_.size() && gone + taken < upto) {
            const std::size_t ends = gone + patches_[patch].pieces[which].much;

            if (ends <= upto) {
                gone = ends;
                taken = 0;
                stepped();
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

        laid(Piece{true, last_at, one.text.size()});

        grown += one.text.size();
    }

    copied(whole_);

    patches_ = std::move(made);
    whole_ = whole_ + grown - shrunk;
    stale_ = true;
}

}
