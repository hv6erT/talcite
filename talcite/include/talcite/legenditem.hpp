#pragma once

#include <string>
#include <utility>

#include "talcite/event.hpp"
#include "talcite/keyednode.hpp"
#include "talcite/node.hpp"
#include "talcite/style.hpp"

namespace talcite::ui {

class LegendItem : public KeyedNode<Node> {
public:
    LegendItem(Key key, std::string text);

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given) { text_ = std::move(given); }

    [[nodiscard]] bool able() const override { return able_; }

    void able(bool given) { able_ = given; }

    [[nodiscard]] std::string tag() const;

    void pick();

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int takes() const override;

protected:
    View paint(Size given, Area seen) override;

    void act() override;

    bool react(ClickEvent& what) override;

private:
    std::string text_;
    bool able_ = true;
};

}
