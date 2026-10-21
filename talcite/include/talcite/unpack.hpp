#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "talcite/bytes.hpp"
#include "talcite/seam.hpp"

namespace talcite {

enum class Packing : std::uint8_t { gzip, deflate, zlib };

class Unpacking {
public:
    virtual ~Unpacking() = default;

    Unpacking(const Unpacking&) = delete;
    Unpacking& operator=(const Unpacking&) = delete;
    Unpacking(Unpacking&&) = delete;
    Unpacking& operator=(Unpacking&&) = delete;

    [[nodiscard]] virtual Bytes fed(std::span<const std::byte> given) = 0;

    [[nodiscard]] virtual bool ended() const = 0;

protected:
    Unpacking() = default;
};

class UnpackProvider : public Provider {
public:
    [[nodiscard]] static constexpr std::string_view kind() { return "Unpack"; }

    [[nodiscard]] virtual std::unique_ptr<Unpacking> started(Packing which) = 0;
};

}
