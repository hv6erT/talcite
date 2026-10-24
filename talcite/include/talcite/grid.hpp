#pragma once

#include "talcite/node.hpp"

#include <vector>

namespace talcite::ui {

class Grid : public Node {
public:
    explicit Grid(int gap = 1);

    [[nodiscard]] int gap() const { return gap_; }

    void gap(int given);

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    View paint(Size given, Area seen) override;

private:
    [[nodiscard]] std::vector<int> columns(Size room) const;

    [[nodiscard]] std::vector<Area> rows(Size room, const std::vector<int>& columns) const;

    [[nodiscard]] std::vector<Area> cells(const Node& row, int high, const std::vector<int>& columns) const;

    int gap_ = 1;
};

class GridRow final : public Node {
public:
    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

protected:
    View paint(Size given, Area seen) override;
};

class Across final : public Node {};

class Blank final : public Node {
public:
    explicit Blank(Size size = {});

    [[nodiscard]] int capacity() const override;

    [[nodiscard]] Size wants(Size room) const override;

private:
    Size size_;
};

}
