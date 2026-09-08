#pragma once

#include <functional>
#include <string>
#include <utility>

#include "talcite/node.hpp"

namespace talcite::ui {

class ListItem : public Node {
public:
    explicit ListItem(std::string text);

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given) { text_ = std::move(given); }

    [[nodiscard]] bool able() const { return able_; }

    void able(bool given);

    void pick();

    [[nodiscard]] bool focusable() const override;

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int takes() const override;

protected:
    View paint(Size given, Area seen) override;

    bool react(KeyEvent& what) override;

    bool react(ClickEvent& what) override;

    bool react(DoubleClickEvent& what) override;

private:
    std::string text_;
    bool able_ = true;
};

}
