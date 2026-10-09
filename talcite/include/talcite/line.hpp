#pragma once

#include <string>
#include <utility>

#include "talcite/node.hpp"
#include "talcite/shape.hpp"

namespace talcite::ui {

struct Lines {
    char32_t upper_left;
    char32_t upper_right;
    char32_t lower_left;
    char32_t lower_right;
    char32_t across;
    char32_t upright;
    char32_t left_joint;
    char32_t right_joint;
};

class Line : public Node {
public:
    explicit Line(std::string text = {}, Border border = Border::thin);

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given) { text_ = std::move(given); }

    [[nodiscard]] Border border() const { return border_; }

    void border(Border given) { border_ = given; }

    [[nodiscard]] Justify justify() const { return justify_; }

    void justify(Justify given) { justify_ = given; }

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int capacity() const override;

protected:
    View paint(Size given, Area seen) override;

private:
    std::string text_;
    Border border_ = Border::thin;
    Justify justify_ = Justify::center;
};

}
