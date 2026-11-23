#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "tal/drawing.hpp"

#include "flattening.hpp"

namespace tal::drawing::rasterizing {

struct Box {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    [[nodiscard]] bool empty() const { return x0 >= x1 || y0 >= y1; }

    [[nodiscard]] int wide() const { return x1 - x0; }

    [[nodiscard]] int high() const { return y1 - y0; }

    [[nodiscard]] Box common(const Box& other) const;

    friend bool operator==(const Box&, const Box&) = default;
};

struct Step {
    int x = 0;
    float cover = 0;
};

[[nodiscard]] Box bounded(const std::vector<flattening::Line>& lines, const Box& within);

class Coverage {
public:
    void reset(const Box& box, int rows = 16);

    void covered(const std::vector<flattening::Line>& lines, Rule rule);

    [[nodiscard]] const Box& box() const { return box_; }

    [[nodiscard]] std::span<const Step> steps(int y) const;

private:
    void lined(Point from, Point to);

    void accumulated(double x0, double y0, double x1, double y1, double direction);

    void resolved(Rule rule);

    template <typename Ruling>
    [[nodiscard]] float across(std::int32_t head, double winding, Rule rule, const Ruling& rule_of);

    struct Crossing {
        float place = 0;
        float height = 0;
        std::int32_t next = -1;
    };

    Box box_;
    int rows_ = 1;
    int top_ = 0;
    int high_ = 0;
    std::size_t stride_ = 0;
    std::vector<float> cells_;
    struct Note {
        std::uint32_t stamp = 0;
        std::int32_t head = -1;
        float place = 0;
        float height = 0;
    };

    std::vector<Note> notes_;
    std::uint32_t serial_ = 0;
    std::vector<Crossing> crossings_;
    std::vector<std::pair<float, float>> sorted_;
    std::vector<std::uint8_t> marks_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> spans_;
    std::vector<double> windings_;
    std::vector<Step> steps_;
    std::vector<std::size_t> starts_;
    std::vector<std::pair<Point, Point>> edges_;
    std::vector<std::size_t> bucketed_;
    std::vector<std::size_t> buckets_;
};

}
