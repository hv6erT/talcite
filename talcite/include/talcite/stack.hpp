#pragma once

#include "talcite/node.hpp"

namespace talcite::ui {

class Stack : public Node {
public:
    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    void layout(Size given) override;

    void paint(Canvas& onto) const override;

    [[nodiscard]] virtual Area placement(const Node& one, Size room) const;
};

}
