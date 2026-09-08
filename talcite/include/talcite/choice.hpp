#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "talcite/optionsnode.hpp"

namespace talcite::ui {

class Choice : public OptionsNode<Node> {
public:
    Choice();

    explicit Choice(std::vector<std::string> options, std::size_t which = 0);

    [[nodiscard]] bool focusable() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    View paint(Size given, Area seen) override;

    bool react(KeyEvent& what) override;

    bool react(ClickEvent& what) override;
};

}
