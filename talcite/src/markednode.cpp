#include "talcite/markednode.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace talcite::ui {

namespace {

// The order of marks: by line, and on the same line by start. The set is kept sorted this way;
// otherwise "next" could not say which one is next.
bool ahead(const Mark& one, const Mark& two) {
    return one.line != two.line ? one.line < two.line : one.from < two.from;
}

}

// Inserts a mark in its place in the order, not at the end. That is the whole difference from the
// set of options: a mark has a place in the document, so the order does not belong to the caller
// and cannot be broken.
void Marks::add(Mark given) {
    const auto at = std::upper_bound(inside_.begin(), inside_.end(), given, ahead);

    inside_.insert(at, std::move(given));
}

// Replaces all marks, sorting them stably: the caller hands over a ready list, and the order of
// marks at the same place is the caller's, not ours.
void Marks::all(std::vector<Mark> given) {
    std::stable_sort(given.begin(), given.end(), ahead);

    inside_ = std::move(given);
}

// Removes all marks.
void Marks::clear() { inside_.clear(); }

// The mark at the given position. Reaching past the end is the caller's error, not a state to pass
// over in silence, just as with a node's children, the set of keys and the set of options, so it
// throws std::out_of_range.
const Mark& Marks::operator[](std::size_t which) const {
    if (which >= inside_.size()) throw std::out_of_range("talcite::ui::Marks: no such mark");

    return inside_[which];
}


}
