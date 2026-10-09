#pragma once

#include <functional>
#include <string>
#include <utility>

#include "talcite/event.hpp"
#include "talcite/enablednode.hpp"
#include "talcite/keyednode.hpp"

namespace talcite::ui {

class Button : public KeyedNode<EnabledNode<Node>> {
public:
    explicit Button(std::string text);

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given) { text_ = std::move(given); }

    [[nodiscard]] bool primary() const { return primary_; }

    void primary(bool given) { primary_ = given; }

    void pick();

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int capacity() const override;

protected:
    View paint(Size given, Area seen) override;

    void activate() override;

    using KeyedNode::handle;

    bool handle(KeyEvent& what) override;

    bool handle(ClickEvent& what) override;

private:
    std::string text_;
    bool primary_ = false;
};

}
