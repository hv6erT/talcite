#pragma once

#include <functional>
#include <string>
#include <utility>

#include "talcite/event.hpp"
#include "talcite/keyednode.hpp"

namespace talcite::ui {

class MenuItem : public KeyedNode<Node> {
public:
    explicit MenuItem(std::string text);

    MenuItem(std::string text, std::string hint);

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given) { text_ = std::move(given); }

    [[nodiscard]] const std::string& hint() const { return hint_; }

    void hint(std::string given) { hint_ = std::move(given); }

    [[nodiscard]] bool able() const override { return able_; }

    void able(bool given);

    void pick();

    [[nodiscard]] bool focusable() const override;

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int takes() const override;

protected:
    View paint(Size given, Area seen) override;

    void act() override;

    bool react(MouseEvent& what) override;

    bool react(ClickEvent& what) override;

private:
    std::string text_;
    std::string hint_;
    bool able_ = true;
};

}
