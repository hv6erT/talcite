#include "tal/undo.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace tal {

// A step goes with nothing to let go of.
Step::~Step() = default;

// Whether the step took in `later`, a step done right after it, so that both are undone at once:
// by default no.
bool Step::merged(const Step& later) {
    (void)later;

    return false;
}

// An empty history keeping at most `most` steps (at least one); the oldest go first.
Undo::Undo(std::size_t most) : most_(std::max<std::size_t>(most, 1)) {}

// Takes `one`, a step just done: what waited to be redone is gone, and the step joins the last one
// when that one takes it in and nothing sealed it. A saved place among what went, or one that falls
// off the front with the oldest step, can no longer be reached. No step throws
// std::invalid_argument.
void Undo::push(std::unique_ptr<Step> one) {
    if (one == nullptr) throw std::invalid_argument{"tal::Undo::push: no step"};

    undone_.clear();

    if (saved_ && *saved_ > done_.size()) saved_.reset();

    if (!sealed_ && !done_.empty() && done_.back()->merged(*one)) return;

    sealed_ = false;
    done_.push_back(std::move(one));

    if (done_.size() <= most_) return;

    done_.erase(done_.begin());

    if (saved_) saved_ = *saved_ == 0 ? std::nullopt : std::optional<std::size_t>{*saved_ - 1};
}

// Undoes the last step; false when there is none. The next step does not join it.
bool Undo::undo() {
    if (done_.empty()) return false;

    std::unique_ptr<Step> one = std::move(done_.back());

    done_.pop_back();
    one->undo();
    undone_.push_back(std::move(one));
    sealed_ = true;

    return true;
}

// Does again the step undone last; false when there is none. An undo came before it and sealed the
// history, so the next step does not join it.
bool Undo::redo() {
    if (undone_.empty()) return false;

    std::unique_ptr<Step> one = std::move(undone_.back());

    undone_.pop_back();
    one->redo();
    done_.push_back(std::move(one));

    return true;
}

// Keeps the next step from joining the last one, as a move of the cursor does to typing.
void Undo::seal() { sealed_ = true; }

// Forgets every step, as after a file was loaded anew: what there is now counts as saved.
void Undo::clear() {
    done_.clear();
    undone_.clear();
    saved_ = 0;
}

// Marks the place in the history as the one saved, and seals it, so that typing on does not join
// the step before the save.
void Undo::saved() {
    saved_ = done_.size();
    sealed_ = true;
}

// Whether the history stands elsewhere than where it was saved: undoing back to the save gives
// false again. A new history counts as saved.
bool Undo::changed() const { return saved_ != done_.size(); }

// What undoing would undo, for a menu ("Undo typing"); nothing without a step.
std::optional<std::string> Undo::undoing() const {
    if (done_.empty()) return std::nullopt;

    return done_.back()->said();
}

// The step that undoing would take back, as it stands after joining; none without one. It lives on
// in the history after it is undone.
const Step* Undo::last() const { return done_.empty() ? nullptr : done_.back().get(); }

// What redoing would do again; nothing without a step.
std::optional<std::string> Undo::redoing() const {
    if (undone_.empty()) return std::nullopt;

    return undone_.back()->said();
}

}
