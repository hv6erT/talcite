#pragma once

#include <string>
#include <utility>

#include "talcite/keyednode.hpp"
#include "talcite/node.hpp"
#include "talcite/style.hpp"

namespace talcite::ui {

class MenuItem;

class Menu : public KeyedNode<Node> {
public:
    explicit Menu(std::string title, MenuStyle style = {});

    [[nodiscard]] const std::string& title() const { return title_; }

    void title(std::string given) { title_ = std::move(given); }

    [[nodiscard]] LineStyle border() const { return style_.border; }

    void border(LineStyle given) { style_.border = given; }

    [[nodiscard]] bool shade() const { return style_.shade; }

    void shade(bool given) { style_.shade = given; }

    [[nodiscard]] bool opened() const;

    [[nodiscard]] int which() const { return which_; }

    void which(int given);

    void which(MenuItem* one);

    void pick();

    void focus() override;

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int takes() const override;

    [[nodiscard]] bool accepts(const Node& one) const override;

    [[nodiscard]] int layer() const override;

    [[nodiscard]] bool focusable() const override;

protected:
    View paint(Size given, Area seen) override;

    void act() override;

    bool react(KeyEvent& what) override;

    bool react(ClickEvent& what) override;

    bool react(FocusEvent& what) override;

    bool react(UnfocusEvent& what) override;

private:
    friend class MenuWindow;

    [[nodiscard]] Size fold() const;

    [[nodiscard]] int nearest(int from, int step) const;

    [[nodiscard]] MenuItem* item(int which) const;

    std::string title_;
    MenuStyle style_;
    int which_ = -1;
    int aside_ = 0;
};

}
