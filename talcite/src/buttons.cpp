#include "talcite/buttons.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

#include "talcite/button.hpp"
#include "talcite/row.hpp"

namespace talcite::ui {

// A window's row of answers. A form and a message have it alike: a row of buttons centered below
// the content. Adding on the window itself would not say what it adds, so a subobject says it:
// `buttons().add("OK")`.
//
// The subobject is not a node. The row belongs to the window and stands in its tree from the start;
// `Buttons` only manages it, so nobody but the window makes one, and the constructor is private.
//
// The subobject makes the button and hands it to the caller. If it took a ready button, it would
// have to overwrite its role and squeeze in before the handlers the caller had already hung on it.
// This way there is one order and no way to break it, and the caller gets a full button:
// `primary()`, `keys()`, `role()` and its own `on<PickEvent>`, where the action of this answer
// lives.
//
// The window hangs nothing of its own on the button and does not close by itself after a press.
// Closing is part of the answer's action, so it belongs to the caller: its handler calls `close()`
// when it decides the window should go. Not every answer ends the window (a Help button in a form
// should not close it), and the library does not know better than the application.
std::shared_ptr<Button> Buttons::add(std::string text) {
    auto one = std::make_shared<Button>(std::move(text));

    if (!role_.empty()) one->role(role_);

    row_->children().add(one);

    all_.push_back(one);

    return one;
}

// The button at the given position. Reaching past the end is the caller's error, not a state to
// keep quiet about, just as with a node's children and a set of options.
const std::shared_ptr<Button>& Buttons::operator[](std::size_t which) const {
    if (which >= all_.size()) throw std::out_of_range("talcite::ui::Buttons: no such button");

    return all_[which];
}

// Clearing takes the buttons away together with the gaps between them: the whole row is theirs and
// nobody else's.
void Buttons::clear() {
    row_->children().clear();

    all_.clear();
}

// The role is handed out by a message, because it has a family of roles (`<stem>.button`); a form
// has no roles of its own and does not call this at all, so its buttons are drawn as
// `talcite.button`. The role goes to the buttons already standing and to every later one.
void Buttons::role(std::string given) {
    role_ = std::move(given);

    for (const std::shared_ptr<Button>& one : all_) one->role(role_);
}

}
