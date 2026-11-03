#pragma once

#include "talcite/area.hpp"
#include "talcite/node.hpp"
#include "talcite/shape.hpp"

namespace talcite::ui {

class Float : public Node {
public:
    [[nodiscard]] Axis axis() const { return axis_; }

    [[nodiscard]] Align align() const { return align_; }

protected:
    Float(Axis axis, Align align);

    void layout(Size given) override;

    void paint(Canvas& onto) const override;

private:
    Axis axis_;
    Align align_;
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

class Top final : public Float {
public:
    Top();
};

class Bottom final : public Float {
public:
    Bottom();
};

class Middle final : public Float {
public:
    Middle();
};

}
