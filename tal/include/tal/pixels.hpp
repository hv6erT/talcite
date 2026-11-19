#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/fs.hpp"
#include "tal/path.hpp"
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

    [[nodiscard]] Pixels oriented(std::uint8_t orientation) const;

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

    [[nodiscard]] static std::shared_ptr<Decoding> builtin();

    [[nodiscard]] static std::shared_ptr<Decoding> png();

    [[nodiscard]] static std::shared_ptr<Decoding> jpeg();

    [[nodiscard]] static std::shared_ptr<Decoding> gif();

    [[nodiscard]] virtual bool takes(std::span<const std::byte> head) const = 0;

    [[nodiscard]] virtual std::optional<Size> sized(std::span<const std::byte> head) const;

    virtual void decoded(std::span<const std::byte> given, const Taking& each) = 0;

    virtual void decoded(std::span<const std::byte> given, const Taking& each, [[maybe_unused]] Size most) { decoded(given, each); }
};

class Chain final : public Decoding {
public:
    explicit Chain(std::vector<std::shared_ptr<Decoding>> links, std::string name = "chain");

    [[nodiscard]] std::string_view name() const override { return name_; }

    [[nodiscard]] bool takes(std::span<const std::byte> head) const override;

    [[nodiscard]] std::optional<Size> sized(std::span<const std::byte> head) const override;

    void decoded(std::span<const std::byte> given, const Taking& each) override;

    void decoded(std::span<const std::byte> given, const Taking& each, Size most) override;

private:
    [[nodiscard]] Decoding* taking(std::span<const std::byte> head) const;

    std::vector<std::shared_ptr<Decoding>> links_;
    std::string name_;
};

void decoded(std::span<const std::byte> given, const Taking& each, Optional<Decoding> decoding = Optional<Decoding>{defaults().get<Decoding>()}, Size most = {});

[[nodiscard]] Task<Pixels> decoded(Bytes given, Size want = {}, Optional<Decoding> decoding = Optional<Decoding>{defaults().get<Decoding>()}, std::size_t most = 0);

[[nodiscard]] Task<Pixels> previewed(Path where, Size most, Port<fs::Files> files = {});

[[nodiscard]] Task<Pixels> thumbnailed(Path where, std::uint32_t side, Port<fs::Files> files = {});

[[nodiscard]] Task<void> trimmed(std::uint64_t most = std::uint64_t{256} << 20, Port<fs::Files> files = {});

[[nodiscard]] std::uint64_t hashed(const Pixels& pixels);

[[nodiscard]] int apart(std::uint64_t one, std::uint64_t other);

}
