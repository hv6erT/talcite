#pragma once

#include <optional>

#include "talcite/area.hpp"
#include "talcite/blocks.hpp"
#include "talcite/capabilities.hpp"

namespace talcite {

struct Setup {
    std::optional<Colors> colors;
    std::optional<Mouse> mouse;
    std::optional<Screen> screen;
    std::optional<Shade> shade;
    std::optional<Graphics> graphics;
    std::optional<ui::Blocks> blocks;
    std::optional<ui::Size> cell;

    std::optional<bool> unicode;
    std::optional<bool> hover;
    std::optional<bool> bracketed_paste;
    std::optional<bool> synchronized;
    std::optional<bool> titles;
};

}
