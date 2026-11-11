#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "tal/geometry.hpp"
#include "tal/node.hpp"

namespace tal {

enum class Edge { start, middle, end };

class Row : public Node {
public:
    explicit Row(int gap = 0);

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

private:
    int gap_;
};

class Column : public Node {
public:
    explicit Column(int gap = 0);

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

private:
    int gap_;
};

class Grid : public Node {
public:
    Grid(std::vector<Share> columns, std::vector<Share> rows, int gap = 1);

    void at(Node& child, Spot cell, Size span = {1, 1});

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

private:
    struct Placement {
        std::weak_ptr<Node> node;
        Spot cell;
        Size span;
    };

    [[nodiscard]] std::vector<std::pair<Node*, Area>> cells() const;

    std::vector<Share> columns_;
    std::vector<Share> rows_;
    int gap_;
    std::vector<Placement> placed_;
};

class Align : public Node {
public:
    Align(Axis axis, Edge where);

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

private:
    Axis axis_;
    Edge where_;
};

}
