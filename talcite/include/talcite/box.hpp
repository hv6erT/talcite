#pragma once

#include <functional>

#include "talcite/area.hpp"
#include "talcite/node.hpp"

namespace talcite::ui {

class Box : public Node {
public:
    using Wish = std::function<Size(Size room)>;

    explicit Box(Size given = {plenty, plenty});

    explicit Box(Wish wish);

    [[nodiscard]] Size wants(Size room) const override;

private:
    Wish wish_;
};

}
