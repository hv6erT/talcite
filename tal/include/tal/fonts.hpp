#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "tal/bytes.hpp"
#include "tal/drawing.hpp"

namespace tal::drawing {

enum class Align : std::uint8_t { left, center, right };

enum class Baseline : std::uint8_t { top, middle, alphabetic, bottom };

struct Measure {
    double wide = 0;
    double ascent = 0;
    double descent = 0;

    friend bool operator==(const Measure&, const Measure&) = default;
};

class Font {
public:
    [[nodiscard]] static Font builtin();

    [[nodiscard]] static Font read(Bytes file);

    [[nodiscard]] const std::string& name() const;

    [[nodiscard]] Path outlined(std::string_view text, double size, Point at, Align align = Align::left, Baseline baseline = Baseline::alphabetic) const;

    [[nodiscard]] Measure measured(std::string_view text, double size) const;

    [[nodiscard]] bool covers(char32_t code) const;

    struct Data;

private:
    explicit Font(std::shared_ptr<const Data> data);

    std::shared_ptr<const Data> data_;
};

}
