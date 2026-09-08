#pragma once

#include "talcite/menuitem.hpp"

namespace talcite::ui {

class MenuLine : public MenuItem {
public:
    MenuLine();

    [[nodiscard]] Size wants(Size room) const override;

protected:
    View paint(Size given, Area seen) override;
};

}
