#pragma once

#include <cstddef>

#include "talcite/node.hpp"

namespace talcite::ui {

class Tabs : public Node {
public:
    Tabs();

    [[nodiscard]] std::size_t which() const { return which_; }

    void which(std::size_t at);

    void step(int by);

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] bool accepts(const Node& one) const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    View paint(Size given, Area seen) override;

    bool handle(KeyEvent& what) override;

    bool handle(ShortcutEvent& what) override;

    bool handle(ClickEvent& what) override;

private:
    std::size_t which_ = 0;
};

}
