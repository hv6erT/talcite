#pragma once

#include <string>
#include <utility>

#include "talcite/node.hpp"
#include "talcite/style.hpp"

namespace talcite::ui {

class Frame : public Node {
public:
    explicit Frame(std::string title = {}, FrameStyle style = {});

    [[nodiscard]] const std::string& title() const { return title_; }

    void title(std::string given) { title_ = std::move(given); }

    [[nodiscard]] const std::string& footer() const { return footer_; }

    void footer(std::string given) { footer_ = std::move(given); }

    [[nodiscard]] LineStyle border() const { return style_.border; }

    void border(LineStyle given) { style_.border = given; }

    [[nodiscard]] JustifyStyle justify() const { return style_.justify; }

    void justify(JustifyStyle given) { style_.justify = given; }

    [[nodiscard]] bool top() const { return style_.top; }

    void top(bool given) { style_.top = given; }

    [[nodiscard]] bool bottom() const { return style_.bottom; }

    void bottom(bool given) { style_.bottom = given; }

    [[nodiscard]] bool left() const { return style_.left; }

    void left(bool given) { style_.left = given; }

    [[nodiscard]] bool right() const { return style_.right; }

    void right(bool given) { style_.right = given; }

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
    FrameStyle style_;
};

}
