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
// when that one takes it in and nothing sealed it. No step throws std::invalid_argument.
void Undo::push(std::unique_ptr<Step> one) {
    if (one == nullptr) throw std::invalid_argument{"tal::Undo::push: no step"};

    undone_.clear();

    if (!sealed_ && !done_.empty() && done_.back()->merged(*one)) return;

    sealed_ = false;
    done_.push_back(std::move(one));

    if (done_.size() > most_) done_.erase(done_.begin());
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

// Forgets every step, as after a file was loaded anew.
void Undo::clear() {
    done_.clear();
    undone_.clear();
}

// What undoing would undo, for a menu ("Undo typing"); nothing without a step.
std::optional<std::string> Undo::undoing() const {
    if (done_.empty()) return std::nullopt;

    return done_.back()->said();
}

// What redoing would do again; nothing without a step.
std::optional<std::string> Undo::redoing() const {
    if (undone_.empty()) return std::nullopt;

    return undone_.back()->said();
}

}
