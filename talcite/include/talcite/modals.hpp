#pragma once

#include <memory>

#include "talcite/layer.hpp"
#include "talcite/row.hpp"

namespace talcite::ui {

class Modal;
class Root;

class Modals : public Layers {
public:
    Modals();

    void open(std::shared_ptr<Modal> given);

    [[nodiscard]] Modal* current() const;

    [[nodiscard]] Node* content() const;

    void content(std::shared_ptr<Node> given);

    [[nodiscard]] int capacity() const override;

protected:
    void restore(std::weak_ptr<Node> was) override;

private:
    friend class Root;

    [[nodiscard]] bool withheld(Node& one);

    std::shared_ptr<Row> body_;
};

}
