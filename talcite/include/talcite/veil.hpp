#pragma once

#include <memory>

#include "talcite/node.hpp"
#include "talcite/style.hpp"

namespace talcite::ui {

class Veil : public Node {
public:
    explicit Veil(VeilStyle style = {});

    void close();

    void step(int stops);

    [[nodiscard]] bool shade() const { return style_.shade; }

    void shade(bool given) { style_.shade = given; }

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int layer() const override;

    [[nodiscard]] int takes() const override;

    [[nodiscard]] bool focusable() const override;

protected:
    View paint(Size given, Area seen) override;

    [[nodiscard]] virtual Area box(Size given) const;

    bool react(KeyEvent& what) override;

    bool react(ClickEvent& what) override;

    bool react(MouseEvent& what) override;

    bool react(WheelEvent& what) override;

private:
    friend class VeilWindow;

    void closing();

    VeilStyle style_;
    std::weak_ptr<Node> was_;
};

}
