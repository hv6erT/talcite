#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "talcite/event.hpp"
#include "talcite/handler.hpp"
#include "talcite/modal.hpp"
#include "talcite/node.hpp"

namespace demo {

// The hooks one place keeps. A hook lasts as long as whoever keeps it, so every listener has a
// keeper: a demo's screen keeps those of its widgets for the whole run, in a `Hooks` of its `main`,
// and a window keeps those of its own buttons for as long as it stands (`Kept`).
using Hooks = std::vector<talcite::ui::Hook>;

// A window of the given kind (a dialog, a message) that keeps the hooks of what happens in it: the
// actions of its buttons and its closing on Escape go together with the window.
template <typename Base>
class Kept : public Base {
public:
    using Base::Base;

    Hooks heard;
};

// Attaches an action to a press, kept in `into`.
//
// The library has one notification mechanism, `on<PickEvent>`, and deliberately offers no
// shortcut, because a shortcut in the library would become a second mechanism. Here, on the
// application side, a shortcut is fine: it fits the call on one line, and underneath it is the
// same handler that anyone could attach.
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

// Closes the window on Escape; the window keeps the hook.
//
// The library does not do this: closing a window is an action, and `handle` must not perform an
// action the caller cannot cancel. Here, on the application side, the decision is ours, and this
// is exactly the one line that the rule costs.
template <typename One>
std::shared_ptr<One> closing(std::shared_ptr<One> one) {
    const std::weak_ptr<talcite::ui::Modal> weak = one;

    one->heard.push_back(one->template on<talcite::ui::KeyEvent>([weak](talcite::ui::KeyEvent& what) {
        if (what.key().code() != talcite::ui::Code::escape) return false;

        if (const std::shared_ptr<talcite::ui::Modal> mine = weak.lock()) mine->close();

        return true;
    }));

    return one;
}

}
