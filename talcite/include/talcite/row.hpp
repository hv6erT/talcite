#pragma once

#include "talcite/node.hpp"

#include <utility>
#include <vector>

namespace talcite::ui {

struct RowShape {
    int gap = 0;
    bool wrapping = false;
    bool spread = false;
};

class Row : public Node {
public:
    explicit Row(int gap = 0);

    explicit Row(RowShape shape);

    [[nodiscard]] int gap() const { return gap_; }

    void gap(int given);

    [[nodiscard]] bool wrapping() const { return wrapping_; }

    void wrapping(bool given) { wrapping_ = given; }

    [[nodiscard]] bool spread() const { return spread_; }

    void spread(bool given) { spread_ = given; }

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    View paint(Size given, Area seen) override;

private:
    struct Line {
        std::vector<std::pair<Node*, Size>> placed;
        int wide = 0;
        int high = 0;
    };

    [[nodiscard]] std::vector<Line> laid(Size room) const;

    int gap_ = 0;
    bool wrapping_ = false;
    bool spread_ = false;
};

}
