#pragma once

#include "talcite/node.hpp"

namespace talcite::ui {

class Inset : public Node {
public:
    explicit Inset(int sides, int top = 0);

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    View paint(Size given, Area seen) override;

private:
    int sides_ = 0;
    int top_ = 0;
};

}
