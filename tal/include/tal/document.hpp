#pragma once

#include <cstdint>
#include <vector>

#include "tal/clock.hpp"
#include "tal/geometry.hpp"
#include "tal/node.hpp"
#include "tal/port.hpp"
#include "tal/style.hpp"
#include "tal/surface.hpp"

namespace tal {

struct Tally {
    std::uint64_t styled = 0;
    std::uint64_t measured = 0;
    std::uint64_t arranged = 0;
    std::uint64_t painted = 0;

    friend bool operator==(const Tally&, const Tally&) = default;
};

class Document : public Node {
public:
    explicit Document(Port<Clock> time = {});

    void sheet(StyleSheet given);

    [[nodiscard]] const StyleSheet& sheet() const { return sheet_; }

    void display(Size screen, Colors colors, bool unicode);

    [[nodiscard]] bool due() const;

    const Surface& frame();

    [[nodiscard]] const Tally& tally() const { return tally_; }

private:
    friend class Node;

    enum class Phase { idle, styling, laying, painting };

    void damaged(Area part);

    void styled(Node& one);

    void painted(Node& one, Spot origin, Area clip, Area part);

    Port<Clock> time_;
    StyleSheet sheet_;
    Size screen_;
    Colors colors_ = Colors::mono;
    bool unicode_ = false;
    Surface surface_{{0, 0}};
    std::vector<Area> damage_;
    bool restyling_ = true;
    Phase phase_ = Phase::idle;
    std::uint64_t frames_ = 0;
    Tally tally_;
};

}
