#pragma once

#include "talcite/node.hpp"

namespace talcite::ui {

class Column : public Node {
public:
    explicit Column(int gap = 0);

    [[nodiscard]] int gap() const { return gap_; }

    void gap(int given);

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    View paint(Size given, Area seen) override;

private:
    int gap_ = 0;
};

}
