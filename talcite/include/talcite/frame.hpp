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

    void title(std::string given) { title_ = std::move(given); }

    [[nodiscard]] const std::string& footer() const { return footer_; }

    void footer(std::string given) { footer_ = std::move(given); }

    [[nodiscard]] Border border() const { return shape_.border; }

    void border(Border given) { shape_.border = given; }

    [[nodiscard]] Justify justify() const { return shape_.justify; }

    void justify(Justify given) { shape_.justify = given; }

    [[nodiscard]] Justify footing() const { return shape_.footing; }

    void footing(Justify given) { shape_.footing = given; }

    [[nodiscard]] bool top() const { return shape_.top; }

    void top(bool given) { shape_.top = given; }

    [[nodiscard]] bool bottom() const { return shape_.bottom; }

    void bottom(bool given) { shape_.bottom = given; }

    [[nodiscard]] bool left() const { return shape_.left; }

    void left(bool given) { shape_.left = given; }

    [[nodiscard]] bool right() const { return shape_.right; }

    void right(bool given) { shape_.right = given; }

    [[nodiscard]] Size wants(Size room) const override;

protected:
    View paint(Size given, Area seen) override;

private:
    struct Edges {
        int top = 0;
        int bottom = 0;
        int left = 0;
        int right = 0;
    };

    [[nodiscard]] Edges edges() const;

    std::string title_;
    std::string footer_;
    FrameShape shape_;
};

}
