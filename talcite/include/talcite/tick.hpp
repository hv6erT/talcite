#pragma once

#include <string>
#include <utility>

#include "talcite/event.hpp"
#include "talcite/keyednode.hpp"

namespace talcite::ui {

class Tick : public KeyedNode<Node> {
public:
    explicit Tick(std::string text, bool on = false);

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given) { text_ = std::move(given); }

    [[nodiscard]] bool ticked() const { return on_; }

    void ticked(bool given);

    void pick();

    [[nodiscard]] bool able() const override { return able_; }

    void able(bool given);

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
    bool on_ = false;
    bool able_ = true;
};

}
