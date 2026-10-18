#pragma once

#include <memory>
#include <string>

#include "talcite/menu.hpp"
#include "talcite/menuitem.hpp"

namespace talcite::ui {

class Submenu : public MenuItem {
public:
    explicit Submenu(std::string text, std::string hint = {});

    ~Submenu() override;

    [[nodiscard]] Menu& menu() const { return *menu_; }

    void pick() override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    View paint(Size given, Area seen) override;

    using MenuItem::handle;

    bool handle(KeyEvent& what) override;

private:
    std::shared_ptr<Menu> menu_;
};

}
