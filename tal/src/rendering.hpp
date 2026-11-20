#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/docs.hpp"
#include "tal/pixels.hpp"
#include "tal/trouble.hpp"

namespace tal::rendering {

inline constexpr std::uint32_t version = 1;

enum class Sort : std::uint8_t {
    hello = 1,
    done = 2,
    failed = 3,
    read = 4,
    open = 10,
    render = 11,
    close = 12,
    navigate = 13,
    shot = 14,
    resize = 15,
    dismiss = 16,
    block = 20,
    unread = 21,
};

inline constexpr std::uint8_t documents = 1;
inline constexpr std::uint8_t pages = 2;
inline constexpr std::uint8_t scripts = 4;
inline constexpr std::uint8_t network = 8;

inline constexpr std::size_t head = 9;
inline constexpr std::size_t widest = media::ceiling * 4 + 4096;
inline constexpr std::uint32_t widest_block = std::uint32_t{1} << 20;

struct Frame {
    Sort sort = Sort::done;
    std::uint32_t id = 0;
    Bytes payload;
};

[[nodiscard]] Bytes framed(const Frame& frame);

class Unframing {
public:
    explicit Unframing(std::size_t most = widest) : most_(most) {}

    void add(std::span<const std::byte> more);

    [[nodiscard]] std::optional<Frame> next();

    [[nodiscard]] bool empty() const { return held_.size() == at_; }

private:
    std::size_t most_;
    Bytes held_;
    std::size_t at_ = 0;
};

class Writing {
public:
    Writing& u8(std::uint8_t value);

    Writing& u32(std::uint32_t value);

    Writing& u64(std::uint64_t value);

    Writing& f64(double value);

    Writing& text(std::string_view value);

    Writing& bytes(std::span<const std::byte> value);

    [[nodiscard]] Bytes done() { return std::move(made_); }

private:
    Bytes made_;
};

class Reading {
public:
    explicit Reading(std::span<const std::byte> from) : from_(from) {}

    [[nodiscard]] std::uint8_t u8();

    [[nodiscard]] std::uint32_t u32();

    [[nodiscard]] std::uint64_t u64();

    [[nodiscard]] double f64();

    [[nodiscard]] std::string text();

    [[nodiscard]] std::span<const std::byte> bytes(std::size_t much);

    [[nodiscard]] std::span<const std::byte> rest();

    void ended() const;

private:
    [[nodiscard]] std::span<const std::byte> taken(std::size_t much);

    std::span<const std::byte> from_;
    std::size_t at_ = 0;
};

struct Hello {
    std::uint32_t version = 0;
    std::string engine;
    std::uint8_t abilities = 0;
};

[[nodiscard]] Hello greeting(std::span<const std::byte> payload);

[[nodiscard]] Bytes greeted(const Hello& hello);

[[nodiscard]] Trouble failure(std::span<const std::byte> payload);

[[nodiscard]] Bytes failed_with(const Trouble& trouble);

[[nodiscard]] media::Pixels pictured(Reading& from, media::Size size);

[[nodiscard]] std::vector<docs::Page> paged(std::span<const std::byte> payload);

}
