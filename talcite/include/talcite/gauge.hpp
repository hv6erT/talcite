#pragma once

#include <memory>
#include <string>
#include <utility>

#include "talcite/node.hpp"
#include "talcite/shape.hpp"

namespace talcite::ui {

class Gauge : public Node {
public:
    explicit Gauge(int wide, GaugeShape shape = {});

    [[nodiscard]] double part() const { return part_; }

    void part(double given);

    [[nodiscard]] bool busy() const { return busy_; }

    void busy(bool given);

    void step();

    [[nodiscard]] int wide() const { return wide_; }

    void wide(int given) { wide_ = given; }

    [[nodiscard]] bool percent() const { return shape_.percent; }

    void percent(bool given) { shape_.percent = given; }

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] int capacity() const override;

protected:
    View paint(Size given, Area seen) override;

private:
    void beating();

    std::shared_ptr<void> beat_;
    GaugeShape shape_;
    double part_ = 0.0;
    int wide_ = 0;
    int at_ = 0;
    int turn_ = 0;
    bool busy_ = false;
};

}
