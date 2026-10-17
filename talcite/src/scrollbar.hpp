#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "talcite/area.hpp"
#include "talcite/event.hpp"
#include "talcite/view.hpp"

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

class Grip {
public:
    void laid(std::optional<Track> given);

    [[nodiscard]] Part part(Spot here) const;

    [[nodiscard]] bool over(Spot here) const { return part(here) != Part::none; }

    [[nodiscard]] bool held() const { return grip_.has_value(); }

    [[nodiscard]] Asked mouse(const MouseEvent& what, Spot here, Node& owner);

private:
    std::optional<Track> track_;
    std::optional<int> grip_;
};

struct Scrollbar {
    Spot at;

    bool upright = true;

    int much = 0;

    std::size_t shift = 0;
    std::size_t whole = 0;
    std::size_t window = 0;

    bool focused = false;

    const Skin& skin;
    const std::string& role;

    void draw(View& into) const;

    [[nodiscard]] Track track() const { return Track{at, upright, much, shift, whole, window}; }

    [[nodiscard]] static Spot edge(Size given, Area seen, bool upright);

    [[nodiscard]] static bool focused_in(const Node& owner);
};

}
