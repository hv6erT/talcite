#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "talcite/enabledmixin.hpp"
#include "talcite/optionsmixin.hpp"

namespace talcite::ui {

class Choice : public OptionsMixin<EnabledMixin<Node>> {
public:
    Choice();

    explicit Choice(std::vector<std::string> options, std::size_t which = 0);

    [[nodiscard]] Size wants(Size room) const override;

protected:
    [[nodiscard]] bool takes_focus() const override;

    void layout(Size given) override;

    void paint(Canvas& onto) const override;

    bool handle(KeyEvent& what) override;

    bool handle(ClickEvent& what) override;
};

}
