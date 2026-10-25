#pragma once

#include "talcite/enabledmixin.hpp"
#include "talcite/node.hpp"

namespace talcite::ui {

class FocusHolder : public EnabledMixin<Node> {
public:
    FocusHolder() = default;

protected:
    bool handle(ClickEvent& what) override;
};

}
