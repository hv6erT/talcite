#pragma once

#include <functional>
#include <string>
#include <utility>

#include "talcite/enablednode.hpp"
#include "talcite/node.hpp"

namespace talcite::ui {

class ListItem : public EnabledNode<Node> {
public:
    explicit ListItem(std::string text);

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given) { text_ = std::move(given); }

    void pick();

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int capacity() const override;

protected:
    View paint(Size given, Area seen) override;

    bool handle(KeyEvent& what) override;

    bool handle(ClickEvent& what) override;

    bool handle(DoubleClickEvent& what) override;

private:
    std::string text_;
};

}
