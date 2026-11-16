#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>

#include "tal/bytes.hpp"
#include "tal/crypto.hpp"

namespace tal::crypto::summing {

class Machine {
public:
    Machine() = default;
    virtual ~Machine() = default;

    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    virtual void feed(std::span<const std::byte> what) = 0;

    [[nodiscard]] virtual Bytes done() = 0;
};

using Key = std::optional<std::span<const std::byte>>;

[[nodiscard]] std::size_t wide(Sum which);

[[nodiscard]] std::unique_ptr<Machine> system(Sum which, Key key);

#ifdef TAL_OWN_SUMS
[[nodiscard]] std::unique_ptr<Machine> own(Sum which, Key key);
#endif

}
