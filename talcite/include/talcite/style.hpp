#pragma once

#include <cstdint>

#include "talcite/view.hpp"

namespace talcite::ui {

enum class JustifyStyle : std::uint8_t { left, right, center };

enum class ElideStyle : std::uint8_t { none, left, middle, right };

enum class LineStyle : std::uint8_t { none, thin, twin, thick };

struct SpanStyle {
    bool wrapping = false;
    JustifyStyle justify = JustifyStyle::left;
    ElideStyle elide = ElideStyle::none;
};

struct ScrollStyle {
    bool progress = false;
};

struct FrameStyle {
    LineStyle border = LineStyle::thin;
    JustifyStyle justify = JustifyStyle::center;
    bool top = true;
    bool bottom = true;
    bool left = true;
    bool right = true;
};

struct MenuStyle {
    LineStyle border = LineStyle::thin;
    bool shade = true;
};

struct VeilStyle {
    bool shade = true;
};

struct ListStyle {
    bool progress = false;
};

struct TableStyle {
    bool heading = true;
    bool progress = false;
};

struct GaugeStyle {
    bool percent = true;
};

struct MessageStyle {
    LineStyle border = LineStyle::twin;
    bool boxed = true;
    bool shade = true;
};

}
