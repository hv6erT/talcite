#pragma once

#include <memory>

#include "talcite/layer.hpp"

namespace talcite::ui {

class Window;

class Windows : public Layers {
public:
    void open(std::shared_ptr<Window> given);

    [[nodiscard]] Window* current() const;

    void raise(Window& one);

    void next();

    void previous();

protected:
    void restore(std::weak_ptr<Node> was) override;

    [[nodiscard]] Area placement(const Node& one, Size room) const override;
};

}
