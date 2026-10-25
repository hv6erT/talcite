#pragma once

#include "talcite/enabledmixin.hpp"
#include "talcite/node.hpp"

namespace talcite::ui {

class Pane : public EnabledMixin<Node> {
public:
    Pane() = default;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    bool handle(ClickEvent& what) override;
};

}
