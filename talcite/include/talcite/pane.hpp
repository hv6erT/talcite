#pragma once

#include "talcite/enablednode.hpp"
#include "talcite/node.hpp"

namespace talcite::ui {

class Pane : public EnabledNode<Node> {
public:
    Pane() = default;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    bool handle(ClickEvent& what) override;
};

}
