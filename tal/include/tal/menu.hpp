#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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

    Property<std::string> folded{*this, Effect::layout, std::string{}};

    [[nodiscard]] bool folding() const { return folding_; }

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
    bool folding_ = false;
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

    Column& insert(std::size_t at, std::string title, std::optional<char32_t> letter = {});

    void remove(std::size_t which);

    void move(std::size_t from, std::size_t to);

    void title(std::size_t which, std::string said);

    [[nodiscard]] const std::string& title(std::size_t which) const;

    [[nodiscard]] Column& page(std::size_t which) const;

    [[nodiscard]] std::size_t count() const { return pages_.size(); }

    [[nodiscard]] std::optional<std::size_t> named_at(Spot screen) const;

    Property<std::size_t> current{*this, Effect::layout, 0};
    Property<bool> framed{*this, Effect::layout, false};
    Property<std::string> heading{*this, Effect::layout, {}};

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

    void paint(Canvas& onto) const override;

    void outline(Canvas& box) const override;

private:
    class Name;

    [[nodiscard]] std::size_t shown() const;

    [[nodiscard]] bool crowded(int wide) const;

    [[nodiscard]] int named() const;

    [[nodiscard]] bool edged() const;

    [[nodiscard]] int lead() const;

    [[nodiscard]] std::optional<std::pair<int, int>> arrows() const;

    void lighted();

    void switched();

    void turned(int by);

    std::vector<Name*> names_;
    std::vector<Column*> pages_;
    bool moving_ = false;
    Hooks hooks_;
};

struct Closing : Event {
    explicit Closing(std::size_t at) : which(at) {}

    std::size_t which = 0;
};

}
