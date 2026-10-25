#pragma once

#include <memory>
#include <string>
#include <vector>

#include "talcite/column.hpp"
#include "talcite/keyedmixin.hpp"
#include "talcite/row.hpp"
#include "talcite/stack.hpp"
#include "talcite/shape.hpp"

namespace talcite::ui {

class Menu;

class Menubar : public KeyedMixin<Stack> {
public:
    Menubar();

    std::shared_ptr<Menu> add(std::string title, MenuShape shape = {});

    [[nodiscard]] const std::vector<std::shared_ptr<Menu>>& menus() const { return menus_; }

    void clear();

    [[nodiscard]] Node* content() const;

    void content(std::shared_ptr<Node> given);

    [[nodiscard]] Menu* current() const;

    [[nodiscard]] bool expanded() const { return expanded_; }

    [[nodiscard]] bool dropdown() const { return dropdown_; }

    void dropdown(bool given) { dropdown_ = given; }

    void enter();

    void leave();

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] bool accepts(const Node& one) const override;

protected:
    void activate() override;

    using KeyedMixin::handle;

    bool handle(KeyEvent& what) override;

    bool handle(FocusEvent& what) override;

    bool handle(UnfocusEvent& what) override;

private:
    friend class Menu;

    void unfold(Menu& one);

    void fold();

    [[nodiscard]] int find(const Menu* one) const;

    [[nodiscard]] bool ours(const Node* one) const;

    std::shared_ptr<Column> frame_;
    std::shared_ptr<Row> bar_;
    std::shared_ptr<Row> body_;
    std::vector<std::shared_ptr<Menu>> menus_;
    std::weak_ptr<Node> was_;
    bool expanded_ = false;
    bool dropdown_ = false;
};

}
