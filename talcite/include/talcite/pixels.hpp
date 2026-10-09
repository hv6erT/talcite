#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "talcite/area.hpp"
#include "talcite/bytes.hpp"
#include "talcite/promise.hpp"

namespace talcite {

struct Still {
    Bytes bytes;
    std::chrono::milliseconds after{0};
};

class Pixels {
public:
    Pixels() = default;

    Pixels(ui::Size given, std::vector<Still> stills);

    [[nodiscard]] bool held() const;

    [[nodiscard]] ui::Size size() const { return size_; }

    [[nodiscard]] const std::vector<Still>& stills() const { return stills_; }

    [[nodiscard]] Pixels scaled(ui::Size given) const;

private:
    ui::Size size_;
    std::vector<Still> stills_;
};

[[nodiscard]] ui::Size fitting(ui::Size image, ui::Size room, ui::Size cell);

enum class Format : std::uint8_t { none, gif, png, jpeg, bmp };

using Taking = std::function<bool(const Bytes& canvas, ui::Size size,
                                  std::chrono::milliseconds after)>;

[[nodiscard]] Format format_of(const Bytes& what);

[[nodiscard]] std::optional<ui::Size> size_of(const Bytes& head);

using Fetch = std::function<Promise<Pixels>(ui::Size want)>;

struct Figure {
    Format format = Format::none;
    ui::Size size;
    Fetch fetch;
};

}
