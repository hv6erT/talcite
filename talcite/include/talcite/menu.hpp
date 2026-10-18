#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

#include "talcite/keyednode.hpp"
#include "talcite/node.hpp"
#include "talcite/shape.hpp"

namespace talcite::ui {

class MenuItem;
class Menubar;
class Submenu;

class Menu : public KeyedNode<Node> {
public:
    explicit Menu(std::string title, MenuShape shape = {});

    [[nodiscard]] const std::string& title() const { return title_; }

    void title(std::string given) { title_ = std::move(given); }

    [[nodiscard]] Border border() const { return shape_.border; }

    void border(Border given) { shape_.border = given; }

    [[nodiscard]] bool shade() const { return shape_.shade; }

    void shade(bool given) { shape_.shade = given; }

    [[nodiscard]] bool opened() const;

    [[nodiscard]] std::optional<std::size_t> which() const;

    void which(std::size_t given);

    [[nodiscard]] MenuItem* current() const;

    void current(MenuItem& one);

    void pick();

    void open();

    void close();

    void focus() override;

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] Area placement(Size room) const override;

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] bool accepts(const Node& one) const override;

    [[nodiscard]] bool focusable() const override;

protected:
    View paint(Size given, Area seen) override;

    void activate() override;

    using KeyedNode::handle;

    bool handle(KeyEvent& what) override;

    bool handle(FocusEvent& what) override;

    bool handle(UnfocusEvent& what) override;

private:
    friend class Menubar;
    friend class MenuTitle;
    friend class Submenu;

    [[nodiscard]] Size fold() const;

    [[nodiscard]] int nearest(int from, int step) const;

    [[nodiscard]] MenuItem* item(int which) const;

    void aim(int given);

    void shut(const Node* kept);

    std::string title_;
    MenuShape shape_;
    int which_ = -1;
    Menubar* bar_ = nullptr;
    Node* head_ = nullptr;
    Submenu* owner_ = nullptr;
};

}
