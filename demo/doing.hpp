#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "talcite/event.hpp"
#include "talcite/node.hpp"
#include "talcite/modal.hpp"

namespace demo {

// Attaches an action to a press.
//
// The library has one notification mechanism, `on<PickEvent>`, and deliberately offers no
// shortcut, because a shortcut in the library would become a second mechanism. Here, on the
// application side, a shortcut is fine: it fits the call on one line, and underneath it is the
// same handler that anyone could attach.
//
// Returns the same pointer, so the call can be woven into the place where the widget is created.
template <typename One>
std::shared_ptr<One> doing(std::shared_ptr<One> one, std::function<void()> what) {
    (void)one->template on<talcite::ui::PickEvent>(
        [what = std::move(what)](talcite::ui::PickEvent&) {
            what();

            return true;
        });

    return one;
}

// Closes the modal on Escape.
//
// The library does not do this: closing a window is an action, and `handle` must not perform an
// action the caller cannot cancel. Here, on the application side, the decision is ours, and this
// is exactly the one line that the rule costs.
template <typename One>
std::shared_ptr<One> closing(std::shared_ptr<One> one) {
    const std::weak_ptr<talcite::ui::Modal> weak = one;

    (void)one->template on<talcite::ui::KeyEvent>([weak](talcite::ui::KeyEvent& what) {
        if (what.key().code() != talcite::ui::Code::escape) return false;

        if (const std::shared_ptr<talcite::ui::Modal> mine = weak.lock()) mine->close();

        return true;
    });

    return one;
}

}
