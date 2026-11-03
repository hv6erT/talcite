#pragma once

#include "talcite/node.hpp"

#include <utility>
#include <vector>

namespace talcite::ui {

class Flow : public Node {
public:
    explicit Flow(int gap = 0);

    [[nodiscard]] int gap() const { return gap_; }

    void gap(int given);

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    void layout(Size given) override;

    void paint(Canvas& onto) const override;

private:
    struct Line {
        std::vector<std::pair<Node*, Size>> placed;
        int wide = 0;
        int high = 0;
    };

    [[nodiscard]] std::vector<Line> laid(Size room) const;

    int gap_ = 0;
};

}
