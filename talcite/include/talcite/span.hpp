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
        const bool same = text_ == given && std::as_const(*this).marks().empty();

        text_ = std::move(given);

        if (!same) marks().clear();
    }

    [[nodiscard]] bool wrapped() const { return shape_.wrapped; }

    void wrapped(bool given) {
        if (shape_.wrapped == given) return;

        shape_.wrapped = given;

        changed();
    }

    [[nodiscard]] Justify justify() const { return shape_.justify; }

    void justify(Justify given) {
        if (shape_.justify == given) return;

        shape_.justify = given;

        changed();
    }

    [[nodiscard]] Elide elide() const { return shape_.elide; }

    void elide(Elide given) {
        if (shape_.elide == given) return;

        shape_.elide = given;

        changed();
    }

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    void paint(Canvas& onto) const override;

    bool handle(ClickEvent& what) override;

private:
    [[nodiscard]] std::vector<std::string> lines(int wide) const;

    std::string text_;
    SpanShape shape_;
};

}
