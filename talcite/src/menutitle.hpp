#pragma once

#include "talcite/node.hpp"

namespace talcite::ui {

class Menu;

class MenuTitle : public Node {
public:
    explicit MenuTitle(Menu& menu);

    [[nodiscard]] Menu& menu() const { return menu_; }

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] bool focusable() const override;

protected:
    void paint(Canvas& onto) const override;

    bool handle(KeyEvent& what) override;

    bool handle(ShortcutEvent& what) override;

    bool handle(ClickEvent& what) override;

private:
    Menu& menu_;
};

}
