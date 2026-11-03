#pragma once

#include "talcite/layer.hpp"
#include "talcite/shape.hpp"

namespace talcite::ui {

class Modal : public Layer {
public:
    explicit Modal(ModalShape shape = {});

    [[nodiscard]] bool shade() const { return shape_.shade; }

    void shade(bool given) {
        if (shape_.shade == given) return;

        shape_.shade = given;

        changed();
    }

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] bool focusable() const override;

protected:
    void layout(Size given) override;

    void paint(Canvas& onto) const override;

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
