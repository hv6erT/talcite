#pragma once

#include <functional>
#include <string>
#include <utility>

#include "talcite/event.hpp"
#include "talcite/enabledmixin.hpp"
#include "talcite/keyedmixin.hpp"

namespace talcite::ui {

class Button : public KeyedMixin<EnabledMixin<Node>> {
public:
    explicit Button(std::string text);

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given) {
        if (text_ == given) return;

        text_ = std::move(given);

        changed();
    }

    [[nodiscard]] bool primary() const { return primary_; }

    void primary(bool given) {
        if (primary_ == given) return;

        primary_ = given;

        changed();
    }

    void pick();

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int capacity() const override;

protected:
    void paint(Canvas& onto) const override;

    void activate() override;

    using KeyedMixin::handle;

    bool handle(KeyEvent& what) override;

    bool handle(ClickEvent& what) override;

private:
    std::string text_;
    bool primary_ = false;
};

}
