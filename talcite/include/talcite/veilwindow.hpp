#pragma once

#include <memory>
#include <vector>

#include "talcite/node.hpp"
#include "talcite/row.hpp"

namespace talcite::ui {

class Veil;

class VeilWindow : public Node {
public:
    VeilWindow();

    void open(std::shared_ptr<Veil> given);

    void close();

    void close(Veil* one);

    [[nodiscard]] const std::vector<std::shared_ptr<Veil>>& veils() const { return veils_; }

    [[nodiscard]] Veil* opened() const;

    [[nodiscard]] const std::shared_ptr<Row>& body() const { return body_; }

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int takes() const override;

protected:
    View paint(Size given, Area seen) override;

private:
    std::shared_ptr<Row> body_;
    std::vector<std::shared_ptr<Veil>> veils_;
};

}
