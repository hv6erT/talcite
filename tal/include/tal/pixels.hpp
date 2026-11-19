#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/port.hpp"
#include "tal/task.hpp"

namespace tal::media {

struct Size {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    friend bool operator==(const Size&, const Size&) = default;
};

struct Still {
    Bytes rgba;
    std::chrono::milliseconds after{0};
};

class Pixels {
public:
    Pixels() = default;

    Pixels(Size size, std::vector<Still> stills);

    [[nodiscard]] bool held() const;

    [[nodiscard]] Size size() const { return size_; }

    [[nodiscard]] const std::vector<Still>& stills() const { return stills_; }

    [[nodiscard]] Pixels scaled(Size to) const;

private:
    Size size_;
    std::vector<Still> stills_;
};

enum class Format { none, gif, png, jpeg, bmp };

[[nodiscard]] Format format_of(std::span<const std::byte> head);

[[nodiscard]] std::optional<Size> size_of(std::span<const std::byte> head);

inline constexpr std::uint64_t ceiling = 8000000;

using Taking = std::function<bool(std::span<const std::byte> rgba, Size size, std::chrono::milliseconds after)>;

class Decoding : public Adapter {
public:
    static constexpr std::string_view kind = "decoding";

    virtual void decoded(std::span<const std::byte> given, const Taking& each) = 0;
};

void decoded(std::span<const std::byte> given, const Taking& each, Optional<Decoding> decoding = Optional<Decoding>{defaults().get<Decoding>()});

[[nodiscard]] Task<Pixels> decoded(Bytes given, Size want = {}, Optional<Decoding> decoding = Optional<Decoding>{defaults().get<Decoding>()}, std::size_t most = 0);

}
