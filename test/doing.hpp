#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "talcite/event.hpp"
#include "talcite/handler.hpp"
#include "talcite/node.hpp"

namespace proving {

// The hooks a trial keeps for as long as its widgets should answer.
using Hooks = std::vector<talcite::ui::Hook>;

// Attaches an action to a press, kept in `into`.
//
// The same shortcut as in the demos: the library has one notification mechanism and deliberately
// offers no shortcut, because a shortcut in the library would become a second mechanism. A trial
// that only wants to count presses does not have to spell out the handler every time.
//
// Returns the same pointer, so the call can be woven into the place where the widget is created.
template <typename One>
std::shared_ptr<One> doing(Hooks& into, std::shared_ptr<One> one, std::function<void()> what) {
    into.push_back(one->template on<talcite::ui::PickEvent>([what = std::move(what)](talcite::ui::PickEvent&) {
        what();

        return true;
    }));

    return one;
}

}
