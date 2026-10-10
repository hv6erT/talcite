#include "talcite/chosen.hpp"

#include <algorithm>

namespace talcite::ui {

// Marks a row as chosen. The set grows on demand, and the table trims it. The set does not know the
// number of rows and has no way to learn it, so choosing a row that does not exist yet is
// remembered, and `keep()`, called while painting, cuts off everything above. The side effect is
// wanted here: the choice survives until the table grows enough for that row to exist.
//
// The number of chosen rows is kept next to the bits (`many_`), so that `many()` and `empty()` do
// not count the whole vector on every question: a panel asks on every Insert, and Insert held down
// over a hundred thousand rows would take quadratic time.
void Chosen::add(std::size_t which) {
    if (inside_.size() <= which) inside_.resize(which + 1, false);

    if (!inside_[which]) ++many_;

    inside_[which] = true;
}

// Unmarks a row; a row that is not chosen is left alone.
void Chosen::remove(std::size_t which) {
    if (which >= inside_.size() || !inside_[which]) return;

    inside_[which] = false;
    --many_;
}

// Unmarks every row.
void Chosen::clear() {
    inside_.clear();
    many_ = 0;
}

// Whether the row is chosen.
bool Chosen::contains(std::size_t which) const {
    return which < inside_.size() && inside_[which];
}

// How many rows are chosen, kept up to date by every change.
std::size_t Chosen::many() const { return many_; }

// The chosen rows in ascending order. It is put together on every call, because inside lies a
// vector of bits; the caller makes a list of files to copy out of it, so it is one pass per action,
// not per frame.
std::vector<std::size_t> Chosen::all() const {
    std::vector<std::size_t> made;

    for (std::size_t at = 0; at < inside_.size(); ++at)
        if (inside_[at]) made.push_back(at);

    return made;
}

// Forgets every row from `rows` on. The trimming subtracts from the count only what it cuts off.
void Chosen::keep(std::size_t rows) {
    if (inside_.size() <= rows) return;

    many_ -= static_cast<std::size_t>(std::count(inside_.begin() + static_cast<std::ptrdiff_t>(rows), inside_.end(), true));

    inside_.resize(rows);
}

}
