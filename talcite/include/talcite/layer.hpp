#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include "talcite/node.hpp"
#include "talcite/stack.hpp"

namespace talcite::ui {

class Layers;

class Layer : public Node {
public:
    void close();

    [[nodiscard]] bool current() const;

protected:
    Layer() = default;

private:
    friend class Layers;

    void closing();

    std::weak_ptr<Node> was_;
};

class Layers : public Stack {
public:
    void close();

    void close(Layer& one);

    [[nodiscard]] std::size_t many() const { return opened_.size(); }

    [[nodiscard]] bool empty() const { return opened_.empty(); }

protected:
    Layers() = default;

    void adopt(std::shared_ptr<Layer> given);

    [[nodiscard]] Layer* top() const;

    [[nodiscard]] const std::vector<std::shared_ptr<Layer>>& opened() const { return opened_; }

    static void remember(Layer& one, Node* focus);

    [[nodiscard]] static std::shared_ptr<Node> remembered(const Layer& one);

    virtual void restore(std::weak_ptr<Node> was) = 0;

private:
    friend class Layer;

    std::vector<std::shared_ptr<Layer>> opened_;
};

}
