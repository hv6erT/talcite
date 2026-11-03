#pragma once

#include <memory>
#include <string>
#include <utility>

#include "talcite/node.hpp"
#include "talcite/shape.hpp"

namespace talcite::ui {

class Grip;

class Scroll : public Node {
public:
    explicit Scroll(Axis axis, ScrollShape shape = {});

    [[nodiscard]] Axis axis() const { return axis_; }

    [[nodiscard]] Spot shift() const { return shift_; }

    void shift(Spot given, Moved how = Moved::by_program);

    [[nodiscard]] bool scrollbar() const { return shape_.scrollbar; }

    void scrollbar(bool given) {
        if (shape_.scrollbar == given) return;

        shape_.scrollbar = given;

        changed();
    }

    [[nodiscard]] const std::string& scrollbar_role() const { return scrollbar_role_; }

    void scrollbar_role(std::string given) {
        if (scrollbar_role_ == given) return;

        scrollbar_role_ = std::move(given);

        changed();
    }

    [[nodiscard]] Size wants(Size room) const override;

protected:
    void layout(Size given) override;

    void paint(Canvas& onto) const override;

    bool handle(WheelEvent& what) override;

    bool handle(FocusEvent& what) override;

    bool handle(MouseEvent& what) override;

private:
    [[nodiscard]] bool across() const { return axis_ != Axis::vertical; }

    [[nodiscard]] bool down() const { return axis_ != Axis::horizontal; }

    Axis axis_;
    std::string scrollbar_role_ = "talcite.scroll";
    ScrollShape shape_;
    Spot shift_;
    std::shared_ptr<Grip> upright_;
    std::shared_ptr<Grip> sideways_;
    Size whole_;
    Size window_;
    bool follow_ = false;
};

}
