#pragma once

#include <functional>
#include <string>
#include <utility>

#include "talcite/event.hpp"
#include "talcite/keyednode.hpp"

namespace talcite::ui {

class Button : public KeyedNode<Node> {
public:
    explicit Button(std::string text);

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given) { text_ = std::move(given); }

    [[nodiscard]] bool able() const override { return able_; }

    void able(bool given);

    [[nodiscard]] bool primary() const { return primary_; }

    void primary(bool given) { primary_ = given; }

    void pick();

    [[nodiscard]] bool focusable() const override;

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int takes() const override;

protected:
    View paint(Size given, Area seen) override;

    void act() override;

    bool react(KeyEvent& what) override;

    bool react(ClickEvent& what) override;

private:
    std::string text_;
    bool able_ = true;
    bool primary_ = false;
};

}
