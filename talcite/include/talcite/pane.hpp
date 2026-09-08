#pragma once

#include "talcite/node.hpp"

namespace talcite::ui {

class Pane : public Node {
public:
    Pane() = default;

    [[nodiscard]] bool able() const { return able_; }

    void able(bool given);

    [[nodiscard]] bool focusable() const override;

    void full(bool given) { full_ = given; }

    [[nodiscard]] bool full() const { return full_; }

    [[nodiscard]] Size wants(Size room) const override;

protected:
    bool react(ClickEvent& what) override;

private:
    bool able_ = true;
    bool full_ = true;
};

}
