#pragma once

#include <cstdint>

namespace talcite::ui {

enum class Justify : std::uint8_t { left, right, center };

enum class Elide : std::uint8_t { none, left, middle, right };

enum class Border : std::uint8_t { none, thin, twin, thick };

struct SpanShape {
    bool wrapped = false;
    Justify justify = Justify::left;
    Elide elide = Elide::none;
};

struct ScrollShape {
    bool scrollbar = false;
    bool needed = false;
};

struct FrameShape {
    Border border = Border::thin;
    Justify justify = Justify::center;
    Justify footing = Justify::center;
    bool top = true;
    bool bottom = true;
    bool left = true;
    bool right = true;
};

struct MenuShape {
    Border border = Border::thin;
    bool shade = true;
};

struct ModalShape {
    bool shade = true;
};

struct TableShape {
    bool heading = true;
    bool scrollbar = false;
};

struct DiffShape {
    bool scrollbar = true;
    Elide elide = Elide::middle;
};

struct GaugeShape {
    bool percent = true;
};

struct MessageShape {
    Border border = Border::twin;
    bool boxed = true;
    bool shade = true;
};

struct FormShape {
    Border border = Border::twin;
    bool shade = true;
};

}
