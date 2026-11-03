#pragma once

#include <string>
#include <utility>

#include "talcite/event.hpp"
#include "talcite/enabledmixin.hpp"
#include "talcite/keyedmixin.hpp"
#include "talcite/node.hpp"
#include "talcite/shape.hpp"

namespace talcite::ui {

class KeyBarItem : public KeyedMixin<EnabledMixin<Node>> {
public:
    KeyBarItem(Key key, std::string text);

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given) {
        if (text_ == given) return;

        text_ = std::move(given);

        changed();
    }

    [[nodiscard]] std::string tag() const;

    void pick();

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int capacity() const override;

protected:
    [[nodiscard]] bool takes_focus() const override;

    void paint(Canvas& onto) const override;

    void activate() override;

    using KeyedMixin::handle;

    bool handle(ClickEvent& what) override;

private:
    std::string text_;
};

}
