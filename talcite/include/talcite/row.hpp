#pragma once

#include "talcite/node.hpp"

#include <vector>

namespace talcite::ui {

class Row : public Node {
public:
    explicit Row(int gap = 0);

    [[nodiscard]] int gap() const { return gap_; }

    void gap(int given);

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    View paint(Size given, Area seen) override;

private:
    [[nodiscard]] std::vector<Area> placed(Size room) const;

    int gap_ = 0;
};

}
