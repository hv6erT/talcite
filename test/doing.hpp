#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "talcite/event.hpp"
#include "talcite/node.hpp"

namespace proving {

// Attaches an action to a press.
//
// The same shortcut as in the demos: the library has one notification mechanism and deliberately
// offers no shortcut, because a shortcut in the library would become a second mechanism. A trial
// that only wants to count presses does not have to spell out the handler every time.
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

}
