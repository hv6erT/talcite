#pragma once

#include <memory>
#include <string>

#include "talcite/keyedmixin.hpp"
#include "talcite/node.hpp"

namespace talcite::ui {

class Tabs;

class Tab : public KeyedMixin<Node> {
public:
    explicit Tab(std::string text);

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given) {
        if (text_ == given) return;

        text_ = std::move(given);

        changed();
    }

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    void layout(Size given) override;

    void paint(Canvas& onto) const override;

    void activate() override;

private:
    friend class Tabs;

    void shown(bool on);

    std::string text_;
    std::shared_ptr<Node> page_;
};

}
