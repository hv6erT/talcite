#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "talcite/area.hpp"
#include "talcite/bytes.hpp"

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

enum class Picture : std::uint8_t { none, gif, png, jpeg, bmp };

using Taking = std::function<bool(const Bytes& canvas, ui::Size size,
                                  std::chrono::milliseconds after)>;

[[nodiscard]] Picture guessed(const Bytes& what);

[[nodiscard]] Pixels decoded(const Bytes& what, std::size_t most = 0);

void decoded(const Bytes& what, const Taking& each);

}
