#pragma once

#include <string>
#include <utility>
#include <vector>

#include "talcite/node.hpp"
#include "talcite/style.hpp"

namespace talcite::ui {

class Span : public Node {
public:
    explicit Span(std::string text, SpanStyle style = {});

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given) { text_ = std::move(given); }

    [[nodiscard]] bool wrapping() const { return style_.wrapping; }

    void wrapping(bool given) { style_.wrapping = given; }

    [[nodiscard]] JustifyStyle justify() const { return style_.justify; }

    void justify(JustifyStyle given) { style_.justify = given; }

    [[nodiscard]] ElideStyle elide() const { return style_.elide; }

    void elide(ElideStyle given) { style_.elide = given; }

    [[nodiscard]] int takes() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    View paint(Size given, Area seen) override;

private:
    [[nodiscard]] std::vector<std::string> lines(int wide) const;

    std::string text_;
    SpanStyle style_;
};

}
