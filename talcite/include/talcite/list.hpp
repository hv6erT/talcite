#pragma once

#include <string>
#include <utility>

#include "talcite/node.hpp"
#include "talcite/style.hpp"

namespace talcite::ui {

class ListItem;

class List : public Node {
public:
    explicit List(int wide = plenty, int high = plenty, ListStyle style = {});

    [[nodiscard]] int which() const { return which_; }

    void which(int given);

    void which(ListItem* one);

    void pick();

    [[nodiscard]] int first() const { return shift_; }

    void first(int given);

    [[nodiscard]] bool progress() const { return style_.progress; }

    void progress(bool given) { style_.progress = given; }

    [[nodiscard]] int wide() const { return wide_; }

    void wide(int given) { wide_ = given; }

    [[nodiscard]] int high() const { return high_; }

    void high(int given) { high_ = given; }

    void focus() override;

    [[nodiscard]] bool focusable() const override;

    [[nodiscard]] const std::string& bar() const { return bar_; }

    void bar(std::string given) { bar_ = std::move(given); }

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int takes() const override;

    [[nodiscard]] bool accepts(const Node& one) const override;

protected:
    View paint(Size given, Area seen) override;

    bool react(KeyEvent& what) override;

    bool react(WheelEvent& what) override;

    bool react(FocusEvent& what) override;

private:
    [[nodiscard]] ListItem* item(int which) const;

    [[nodiscard]] int nearest(int from, int step) const;

    bool going(int to);

    bool walking(int step, int times);

    void stirred();

    std::string bar_ = "talcite.scroll";
    ListStyle style_;
    int which_ = -1;
    int shift_ = 0;
    int wide_ = plenty;
    int high_ = plenty;
    bool follow_ = false;
};

}
