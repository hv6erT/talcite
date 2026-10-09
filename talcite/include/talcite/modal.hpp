#pragma once

#include "talcite/layer.hpp"
#include "talcite/shape.hpp"

namespace talcite::ui {

class Modal : public Layer {
public:
    explicit Modal(ModalShape shape = {});

    [[nodiscard]] bool shade() const { return shape_.shade; }

    void shade(bool given) { shape_.shade = given; }

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] bool focusable() const override;

protected:
    View paint(Size given, Area seen) override;

    [[nodiscard]] virtual Area seat(Size given) const;

    bool handle(KeyEvent& what) override;

    bool handle(ClickEvent& what) override;

    bool handle(MouseEvent& what) override;

    bool handle(WheelEvent& what) override;

private:
    void step(int by);

    ModalShape shape_;
};

}
