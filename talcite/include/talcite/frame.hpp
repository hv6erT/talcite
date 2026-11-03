#pragma once

#include <string>
#include <utility>

#include "talcite/node.hpp"
#include "talcite/shape.hpp"

namespace talcite::ui {

class Frame : public Node {
public:
    explicit Frame(std::string title = {}, FrameShape shape = {});

    [[nodiscard]] const std::string& title() const { return title_; }

    void title(std::string given) {
        if (title_ == given) return;

        title_ = std::move(given);

        changed();
    }

    [[nodiscard]] const std::string& footer() const { return footer_; }

    void footer(std::string given) {
        if (footer_ == given) return;

        footer_ = std::move(given);

        changed();
    }

    [[nodiscard]] Border border() const { return shape_.border; }

    void border(Border given) {
        if (shape_.border == given) return;

        shape_.border = given;

        changed();
    }

    [[nodiscard]] Justify justify() const { return shape_.justify; }

    void justify(Justify given) {
        if (shape_.justify == given) return;

        shape_.justify = given;

        changed();
    }

    [[nodiscard]] Justify footing() const { return shape_.footing; }

    void footing(Justify given) {
        if (shape_.footing == given) return;

        shape_.footing = given;

        changed();
    }

    [[nodiscard]] bool top() const { return shape_.top; }

    void top(bool given) {
        if (shape_.top == given) return;

        shape_.top = given;

        changed();
    }

    [[nodiscard]] bool bottom() const { return shape_.bottom; }

    void bottom(bool given) {
        if (shape_.bottom == given) return;

        shape_.bottom = given;

        changed();
    }

    [[nodiscard]] bool left() const { return shape_.left; }

    void left(bool given) {
        if (shape_.left == given) return;

        shape_.left = given;

        changed();
    }

    [[nodiscard]] bool right() const { return shape_.right; }

    void right(bool given) {
        if (shape_.right == given) return;

        shape_.right = given;

        changed();
    }

    [[nodiscard]] bool overlaid() const { return shape_.overlaid; }

    void overlaid(bool given) {
        if (shape_.overlaid == given) return;

        shape_.overlaid = given;

        changed();
    }

    [[nodiscard]] Size wants(Size room) const override;

protected:
    void layout(Size given) override;

    void paint(Canvas& onto) const override;

private:
    struct Edges {
        int top = 0;
        int bottom = 0;
        int left = 0;
        int right = 0;
    };

    [[nodiscard]] Edges edges() const;

    [[nodiscard]] int covered(const Edges& edge) const;

    std::string title_;
    std::string footer_;
    FrameShape shape_;
};

}
