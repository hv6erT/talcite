#pragma once

#include "talcite/node.hpp"
#include "talcite/shape.hpp"

namespace talcite::ui {

class Float : public Node {
public:
    [[nodiscard]] Justify side() const { return side_; }

protected:
    explicit Float(Justify side);

    View paint(Size given, Area seen) override;

private:
    Justify side_;
};

class Left final : public Float {
public:
    Left();
};

class Right final : public Float {
public:
    Right();
};

class Center final : public Float {
public:
    Center();
};

}
