#pragma once

#include "talcite/node.hpp"

namespace talcite::ui {

class MenuSeparator : public Node {
public:
    MenuSeparator();

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int capacity() const override;

protected:
    void paint(Canvas& onto) const override;
};

}
