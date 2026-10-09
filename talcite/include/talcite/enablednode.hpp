#pragma once

#include <concepts>

#include "talcite/event.hpp"
#include "talcite/node.hpp"

namespace talcite::ui {

template <std::derived_from<Node> Base>
class EnabledNode : public Base {
public:
    [[nodiscard]] bool enabled() const { return enabled_; }

    void enabled(bool given) {
        if (enabled_ == given) return;

        enabled_ = given;

        if (enabled_) {
            (void)this->trigger(EnableEvent{});

            return;
        }

        (void)this->trigger(DisableEvent{});

        if (!enabled_) this->unfocus();
    }

    [[nodiscard]] bool focusable() const final { return enabled_ && takes_focus(); }

protected:
    using Base::Base;

    [[nodiscard]] virtual bool takes_focus() const { return true; }

private:
    bool enabled_ = true;
};

}
