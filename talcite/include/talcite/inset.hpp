#pragma once

#include "talcite/node.hpp"

namespace talcite::ui {

class Inset : public Node {
public:
    explicit Inset(int sides, int top = 0);

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    void layout(Size given) override;

    void paint(Canvas& onto) const override;

private:
    int sides_ = 0;
    int top_ = 0;
};

}
