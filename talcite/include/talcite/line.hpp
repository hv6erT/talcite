#pragma once

#include <string>
#include <utility>

#include "talcite/node.hpp"
#include "talcite/style.hpp"

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
    explicit Line(std::string text = {}, LineStyle lines = LineStyle::thin);

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given) { text_ = std::move(given); }

    [[nodiscard]] LineStyle border() const { return lines_; }

    void border(LineStyle given) { lines_ = given; }

    [[nodiscard]] JustifyStyle justify() const { return justify_; }

    void justify(JustifyStyle given) { justify_ = given; }

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int takes() const override;

protected:
    View paint(Size given, Area seen) override;

private:
    std::string text_;
    LineStyle lines_ = LineStyle::thin;
    JustifyStyle justify_ = JustifyStyle::center;
};

}
