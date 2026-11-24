#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "tal/drawing.hpp"

namespace tal::drawing {

struct Recording::Step {
    enum class Kind : std::uint8_t { save, restore, clip, fill, stroke, draw };

    Kind kind = Kind::save;
    Path path;
    Matrix matrix;
    Rule rule = Rule::nonzero;
    Paint paint;
    Stroke style;
    float opacity = 1;
    bool smooth = true;
    std::shared_ptr<const media::Pixels> image;
    Rect from;
    Rect into;
    std::size_t still = 0;
};

struct Recording::Steps {
    std::vector<Step> all;
};

}
