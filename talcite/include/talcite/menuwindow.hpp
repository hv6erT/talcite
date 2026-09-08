#pragma once

#include <memory>
#include <string>
#include <vector>

#include "talcite/keyednode.hpp"
#include "talcite/node.hpp"
#include "talcite/row.hpp"
#include "talcite/style.hpp"

namespace talcite::ui {

class Menu;

class MenuWindow : public KeyedNode<Node> {
public:
    MenuWindow();

    std::shared_ptr<Menu> add(std::string title, MenuStyle style = {});

    [[nodiscard]] const std::vector<std::shared_ptr<Menu>>& menus() const { return menus_; }

    void clear();

    [[nodiscard]] const std::shared_ptr<Row>& body() const { return body_; }

    [[nodiscard]] Menu* opened() const;

    void close();

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int takes() const override;

protected:
    View paint(Size given, Area seen) override;

    void act() override;

    bool react(KeyEvent& what) override;

    bool react(FocusEvent& what) override;

private:
    [[nodiscard]] int width(int which) const;

    [[nodiscard]] int along(int which) const;

    [[nodiscard]] int which(const Menu* one) const;

    [[nodiscard]] bool ours(const Node* one) const;

    std::shared_ptr<Row> body_;
    std::vector<std::shared_ptr<Menu>> menus_;
    std::weak_ptr<Node> was_;
};

}
