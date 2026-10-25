#pragma once

#include <string>
#include <utility>
#include <vector>

#include "talcite/event.hpp"
#include "talcite/following.hpp"
#include "talcite/markedmixin.hpp"
#include "talcite/node.hpp"
#include "talcite/shape.hpp"

namespace talcite::ui {

class Span : public FollowingMixin<MarkedMixin<Node>> {
public:
    explicit Span(std::string text, SpanShape shape = {});

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given) {
        text_ = std::move(given);
        marks().clear();
    }

    [[nodiscard]] bool wrapped() const { return shape_.wrapped; }

    void wrapped(bool given) { shape_.wrapped = given; }

    [[nodiscard]] Justify justify() const { return shape_.justify; }

    void justify(Justify given) { shape_.justify = given; }

    [[nodiscard]] Elide elide() const { return shape_.elide; }

    void elide(Elide given) { shape_.elide = given; }

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    View paint(Size given, Area seen) override;

    bool handle(ClickEvent& what) override;

private:
    [[nodiscard]] std::vector<std::string> lines(int wide) const;

    std::string text_;
    SpanShape shape_;
};

}
