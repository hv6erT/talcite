#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "talcite/enablednode.hpp"
#include "talcite/optionsnode.hpp"

namespace talcite::ui {

class Choice : public OptionsNode<EnabledNode<Node>> {
public:
    Choice();

    explicit Choice(std::vector<std::string> options, std::size_t which = 0);

    [[nodiscard]] Size wants(Size room) const override;

protected:
    [[nodiscard]] bool takes_focus() const override;

    View paint(Size given, Area seen) override;

    bool handle(KeyEvent& what) override;

    bool handle(ClickEvent& what) override;
};

}
