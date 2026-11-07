#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "talcite/event.hpp"
#include "talcite/enabledmixin.hpp"
#include "talcite/keyedmixin.hpp"

namespace talcite::ui {

class MenuItem : public KeyedMixin<EnabledMixin<Node>> {
public:
    explicit MenuItem(std::string text);

    MenuItem(std::string text, std::string hint);

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given) {
        if (text_ == given) return;

        text_ = std::move(given);

        changed();
    }

    [[nodiscard]] const std::string& hint() const { return hint_; }

    void hint(std::string given) {
        if (hint_ == given) return;

        hint_ = std::move(given);

        changed();
    }

    [[nodiscard]] const std::optional<bool>& checked() const { return checked_; }

    void checked(std::optional<bool> given) {
        if (checked_ == given) return;

        checked_ = given;

        changed();
    }

    virtual void pick();

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int capacity() const override;

protected:
    [[nodiscard]] int lead() const;

    void paint(Canvas& onto) const override;

    void activate() override;

    using KeyedMixin::handle;

    bool handle(MouseEvent& what) override;

    bool handle(ClickEvent& what) override;

private:
    std::string text_;
    std::string hint_;
    std::optional<bool> checked_;
};

}
