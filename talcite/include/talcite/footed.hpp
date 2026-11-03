#pragma once

#include "talcite/node.hpp"

#include <vector>

namespace talcite::ui {

class Footed : public Node {
public:
    explicit Footed(int gap = 0);

    [[nodiscard]] int gap() const { return gap_; }

    void gap(int given);

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    void layout(Size given) override;

    void paint(Canvas& onto) const override;

private:
    struct Laid {
        std::vector<int> highs;
        std::vector<int> wides;
        std::vector<int> aparts;
        int left = 0;
    };

    [[nodiscard]] Laid laid(Size room) const;

    int gap_ = 0;
};

}
