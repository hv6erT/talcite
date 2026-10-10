#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "talcite/modal.hpp"

namespace talcite::ui {

class Modals;

class Rolled : public Modal {
public:
    explicit Rolled(std::weak_ptr<Node> under);

protected:
    [[nodiscard]] Area seat(Size given) const override;

    bool handle(KeyEvent& what) override;

private:
    std::weak_ptr<Node> under_;
};

[[nodiscard]] Modals* housing(Node* from);

class Popup {
public:
    explicit Popup(Node& under) : under_(&under) {}

    [[nodiscard]] int rows() const { return rows_; }

    void rows(int given) { rows_ = std::max(given, 1); }

    [[nodiscard]] bool opened() const { return !rolled_.expired(); }

    void close();

    void open(const std::vector<std::string>& among, std::size_t which,
              std::function<void(std::size_t)> took);

private:
    Node* under_;
    std::weak_ptr<Modal> rolled_;
    int rows_ = 8;
};

}
