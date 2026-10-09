#pragma once

#include "talcite/node.hpp"

namespace talcite::ui {

class Stack : public Node {
public:
    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    View paint(Size given, Area seen) override;
};

}
