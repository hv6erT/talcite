#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "tal/event.hpp"
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

class Split : public Node {
public:
    explicit Split(Axis along, int gap = 0);

    Property<Axis> axis;
    Property<int> least{*this, Effect::layout, 4};

    void part(Node& child, double much);

    [[nodiscard]] double part(const Node& child) const;

    void moved(std::size_t border, int by);

    [[nodiscard]] std::size_t borders() const;

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

    void paint(Canvas& onto) const override;

private:
    struct Part {
        std::weak_ptr<Node> node;
        double much = 1;
    };

    [[nodiscard]] std::vector<double> parts() const;

    [[nodiscard]] int border_spot(std::size_t border) const;

    [[nodiscard]] int along_of(Spot one) const;

    [[nodiscard]] std::optional<std::size_t> border_at(int at) const;

    int gap_;
    std::vector<Part> parts_;
    std::vector<int> lengths_;
    Area inside_{};
    std::optional<std::size_t> dragging_;
    int grip_ = 0;
    Hooks hooks_;
};

struct Resized : Event {
    explicit Resized(std::size_t at) : border(at) {}

    std::size_t border = 0;
};

}
