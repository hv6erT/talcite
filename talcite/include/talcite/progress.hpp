#pragma once

#include <cstdint>

namespace talcite {

struct Progress {
    std::uint64_t done = 0;
    std::uint64_t whole = 0;

    [[nodiscard]] double fraction() const;

    friend bool operator==(const Progress&, const Progress&) = default;
};

}
