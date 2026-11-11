#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tal/action.hpp"
#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/layout.hpp"
#include "tal/node.hpp"
#include "tal/surface.hpp"

namespace tal {

class MenuBar;

class Menu : public Node {
public:
    Menu();

    void add(std::shared_ptr<Action> one);

    Menu& sub(std::string text, std::optional<char32_t> letter = {});

    void separator();

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

    void outline(Canvas& box) const override;

private:
    friend class MenuBar;

    class Item;
    class Separator;

    void picked(Item& one);

    void stepped(Item* from, int by, bool ends);

    MenuBar* bar_ = nullptr;
    Hooks hooks_;
};

class MenuBar : public Node {
public:
    MenuBar();

    ~MenuBar() override;

    Menu& add(std::string text, std::optional<char32_t> letter = {});

    void open(std::size_t which = 0);

    void close();

    [[nodiscard]] bool opened() const;

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

private:
    friend class Menu;

    class Title;
    class Drop;

    [[nodiscard]] std::vector<Title*> titles() const;

    void dropped(Menu& one, const Node& beside);

    void shut(std::size_t from);

    void left(Drop& one);

    void outside(Spot screen, Drop& one);

    void stepped(int by);

    std::vector<std::weak_ptr<Drop>> drops_;
    std::size_t root_ = 0;
};

class KeyBar : public Node {
public:
    KeyBar();

    void add(std::shared_ptr<Action> one);

    void clear();

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

private:
    class Item;
};

class Tabs : public Node {
public:
    Tabs();

    Column& add(std::string title, std::optional<char32_t> letter = {});

    Property<std::size_t> current{*this, Effect::layout, 0};

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

    void paint(Canvas& onto) const override;

private:
    class Name;

    [[nodiscard]] std::size_t shown() const;

    [[nodiscard]] bool crowded(int wide) const;

    void switched();

    void turned(int by);

    std::vector<Name*> names_;
    std::vector<Column*> pages_;
    bool moving_ = false;
    Hooks hooks_;
};

}
