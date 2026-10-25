#include "talcite/optionsmixin.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace talcite::ui {

namespace {

// Removes `one` from the list if it is there. After an addition the string is in the set exactly
// once and stands where it was asked to: a repeat does not duplicate the entry but moves it. This
// is the same rule as in the key set, for the same reason: two identical entries cannot be told
// apart, so the second one would have no way to win. In a choice or an option group two equal
// items would make `value(…)` ambiguous, and in a history the move to the front is the whole
// "newest on top" rule.
void without(std::vector<std::string>& these, const std::string& one) {
    const auto at = std::find(these.begin(), these.end(), one);

    if (at != these.end()) these.erase(at);
}

}

// Appends a string at the end, moving it there if it is already present. An empty string is
// ignored, just like an empty key in the key set and an empty node among children: an empty item
// in a choice cannot be told from the background, and in a history there is nothing to recall.
void Options::add(std::string one) {
    if (one.empty()) return;

    without(inside_, one);

    inside_.push_back(std::move(one));
}

// Inserts a string before position `at` (clamped to the end), moving it there if it is already
// present. An empty string is ignored, as in `add`.
void Options::insert(std::size_t at, std::string one) {
    if (one.empty()) return;

    without(inside_, one);

    inside_.insert(inside_.begin() + static_cast<std::ptrdiff_t>(std::min(at, inside_.size())),
                   std::move(one));
}

// Removes the item at position `which`; a position past the end does nothing.
void Options::remove(std::size_t which) {
    if (which >= inside_.size()) return;

    inside_.erase(inside_.begin() + static_cast<std::ptrdiff_t>(which));
}

// Removes every item.
void Options::clear() { inside_.clear(); }

// Replaces the whole list. Replacing everything does not filter out repeats: the caller hands over
// a finished list and it is the caller's list. Repeats are filtered where items are added one at
// a time, because only there is it known which one should give way to which.
void Options::all(std::vector<std::string> given) { inside_ = std::move(given); }

// Whether the list holds exactly this string.
bool Options::contains(const std::string& one) const {
    return std::find(inside_.begin(), inside_.end(), one) != inside_.end();
}

// The position of `one`, or nothing when it is absent. Searching has one name in the library:
// `find` returns the index of what is looked for, or nothing, just like `Children::find` for a
// node.
std::optional<std::size_t> Options::find(const std::string& one) const {
    const auto at = std::find(inside_.begin(), inside_.end(), one);

    if (at == inside_.end()) return std::nullopt;

    return static_cast<std::size_t>(at - inside_.begin());
}

// The item at position `which`. Reaching past the end is the caller's error, not a state to pass
// over in silence, so it throws `std::out_of_range`, just like the children of a node.
const std::string& Options::operator[](std::size_t which) const {
    if (which >= inside_.size()) throw std::out_of_range("talcite::ui::Options: no such option");

    return inside_[which];
}

// The first item; throws `std::out_of_range` when the list is empty.
const std::string& Options::front() const {
    if (inside_.empty()) throw std::out_of_range("talcite::ui::Options: no options");

    return inside_.front();
}

// The last item; throws `std::out_of_range` when the list is empty.
const std::string& Options::back() const {
    if (inside_.empty()) throw std::out_of_range("talcite::ui::Options: no options");

    return inside_.back();
}

}
