#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "talcite/node.hpp"
#include "talcite/shape.hpp"

namespace talcite::ui {

class Grip;

class ListItem;

class List : public Node {
public:
    explicit List(int wide = plenty, int high = plenty, ScrollShape shape = {});

    [[nodiscard]] std::optional<std::size_t> which() const;

    void which(std::size_t given);

    [[nodiscard]] ListItem* current() const;

    void current(ListItem& one);

    void pick();

    [[nodiscard]] std::size_t first() const;

    void first(std::size_t given, Moved how = Moved::by_program);

    [[nodiscard]] bool scrollbar() const { return shape_.scrollbar; }

    void scrollbar(bool given) {
        if (shape_.scrollbar == given) return;

        shape_.scrollbar = given;

        changed();
    }

    [[nodiscard]] int wide() const { return wide_; }

    void wide(int given) {
        if (wide_ == given) return;

        wide_ = given;

        changed();
    }

    [[nodiscard]] int high() const { return high_; }

    void high(int given) {
        if (high_ == given) return;

        high_ = given;

        changed();
    }

    void focus() override;

    [[nodiscard]] bool focusable() const override;

    [[nodiscard]] const std::string& scrollbar_role() const { return scrollbar_role_; }

    void scrollbar_role(std::string given) {
        if (scrollbar_role_ == given) return;

        scrollbar_role_ = std::move(given);

        changed();
    }

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] bool accepts(const Node& one) const override;

protected:
    void layout(Size given) override;

    void paint(Canvas& onto) const override;

    bool handle(KeyEvent& what) override;

    bool handle(WheelEvent& what) override;

    bool handle(FocusEvent& what) override;

    bool handle(MouseEvent& what) override;

private:
    friend class ListItem;

    [[nodiscard]] ListItem* item(int which) const;

    void aim(int given);

    [[nodiscard]] int nearest(int from, int step) const;

    bool going(int to, Moved how);

    bool walking(int step, int times, Moved how);

    void slid(int to, Moved how);

    std::string scrollbar_role_ = "talcite.scroll";
    ScrollShape shape_;
    std::shared_ptr<Grip> grip_;
    std::vector<std::weak_ptr<Node>> laid_;
    std::size_t swept_ = 0;
    int which_ = -1;
    int shift_ = 0;
    int wide_ = plenty;
    int high_ = plenty;
    bool follow_ = false;
};

}
