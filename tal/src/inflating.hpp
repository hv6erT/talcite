#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "tal/bytes.hpp"

namespace tal::inflating {

enum class Wrapping { raw, gzip };

[[nodiscard]] std::uint32_t crc32(std::uint32_t crc, std::span<const std::byte> bytes);

class Inflater {
public:
    explicit Inflater(Wrapping how);

    [[nodiscard]] Bytes fed(std::span<const std::byte> given);

    [[nodiscard]] bool ended() const;

private:
    enum class Stage { header, block, stored, coded, trailer, done };

    struct Table {
        std::array<std::uint16_t, 16> counts{};
        std::vector<std::uint16_t> symbols;
        std::size_t longest = 0;
    };

    struct Short {};

    void step(Bytes& out);

    [[nodiscard]] std::uint32_t bits(int many);

    [[nodiscard]] int decoded(const Table& table);

    void headed();

    void blocked();

    void coded(Bytes& out);

    void stored(Bytes& out);

    void trailed();

    void kept(Bytes& out);

    Wrapping how_;
    Stage stage_;
    Bytes in_;
    std::size_t at_ = 0;
    int bit_ = 0;
    bool last_ = false;
    std::uint32_t left_ = 0;
    Table lengths_;
    Table distances_;
    Bytes history_;
    std::uint32_t crc_ = 0;
    std::uint32_t size_ = 0;
    std::size_t emitted_ = 0;
};

}
