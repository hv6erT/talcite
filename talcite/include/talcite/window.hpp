#pragma once

#include <cstdint>

#include "talcite/layer.hpp"

namespace talcite::ui {

class Window : public Layer {
public:
    Window();

    void raise();

    [[nodiscard]] bool full() const { return full_; }

    void full(bool given);

    void seat(Spot where, Size size);

    [[nodiscard]] Area seat() const { return seat_; }

    void move(int across, int down);

    void resize(int across, int down);

    [[nodiscard]] Size least() const { return least_; }

    void least(Size given);

    [[nodiscard]] bool dragged() const { return drag_ != Drag::none; }

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    bool handle(ClickEvent& what) override;

    bool handle(MouseEvent& what) override;

private:
    enum class Drag : std::uint8_t { none, move, resize };

    Area seat_{};
    Size least_{1, 1};
    bool full_ = true;
    Drag drag_ = Drag::none;
    Spot grip_{};
};

}
