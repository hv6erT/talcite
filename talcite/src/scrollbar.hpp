#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "talcite/area.hpp"
#include "talcite/canvas.hpp"
#include "talcite/event.hpp"
#include "talcite/snapshot.hpp"

namespace talcite::ui {

class Node;
class Skin;

struct Track {
    Spot at;
    bool upright = true;
    int much = 0;
    std::size_t shift = 0;
    std::size_t whole = 0;
    std::size_t window = 0;
};

enum class Part : std::uint8_t { none, before, after, thumb };

struct Asked {
    Part part = Part::none;
    std::optional<std::size_t> to;
};

struct Hold {
    int grip = 0;
    int from = 0;
    std::size_t shift = 0;
};

class Grip {
public:
    void laid(std::optional<Track> given);

    [[nodiscard]] Part part(Spot here) const;

    [[nodiscard]] bool over(Spot here) const { return part(here) != Part::none; }

    [[nodiscard]] bool held() const { return hold_.has_value(); }

    [[nodiscard]] Asked mouse(const MouseEvent& what, Spot here, Node& owner);

private:
    std::optional<Track> track_;
    std::optional<Hold> hold_;
};

struct Scrollbar {
    Track track;

    bool focused = false;

    const Skin& skin;
    const std::string& role;

    void draw(Canvas& onto) const;

    [[nodiscard]] static Spot edge(Size given, Area seen, bool upright);

    [[nodiscard]] static bool focused_in(const Node& owner);
};

[[nodiscard]] int wheeled(int steps);

}
