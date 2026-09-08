#pragma once

#include <string>
#include <utility>

#include "talcite/node.hpp"
#include "talcite/style.hpp"

namespace talcite::ui {

class Scroll : public Node {
public:
    Scroll() = default;

    explicit Scroll(ScrollStyle style) : style_(style) {}

    [[nodiscard]] Spot shift() const { return shift_; }

    void shift(Spot given);

    [[nodiscard]] bool progress() const { return style_.progress; }

    void progress(bool given) { style_.progress = given; }

    [[nodiscard]] const std::string& bar() const { return bar_; }

    void bar(std::string given) { bar_ = std::move(given); }

    [[nodiscard]] Size wants(Size room) const override;

protected:
    View paint(Size given, Area seen) override;

    bool react(WheelEvent& what) override;

private:
    std::string bar_ = "talcite.scroll";
    ScrollStyle style_;
    Spot shift_;
    Size whole_;
    Size window_;
};

}
