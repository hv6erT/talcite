#pragma once

#include <functional>
#include <string>
#include <utility>

#include "talcite/event.hpp"
#include "talcite/enablednode.hpp"
#include "talcite/keyednode.hpp"

namespace talcite::ui {

class MenuItem : public KeyedNode<EnabledNode<Node>> {
public:
    explicit MenuItem(std::string text);

    MenuItem(std::string text, std::string hint);

    [[nodiscard]] const std::string& text() const { return text_; }

    void text(std::string given) { text_ = std::move(given); }

    [[nodiscard]] const std::string& hint() const { return hint_; }

    void hint(std::string given) { hint_ = std::move(given); }

    void pick();

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int capacity() const override;

protected:
    View paint(Size given, Area seen) override;

    void activate() override;

    using KeyedNode::handle;

    bool handle(MouseEvent& what) override;

    bool handle(ClickEvent& what) override;

private:
    std::string text_;
    std::string hint_;
};

}
