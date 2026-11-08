#pragma once

#include <cstdint>

namespace tal::testing {

class Expected {
public:
    Expected();
    ~Expected();

    Expected(const Expected&) = delete;
    Expected& operator=(const Expected&) = delete;

    [[nodiscard]] std::uint64_t failures() const;

private:
    std::uint64_t start_;
};

[[nodiscard]] Expected expect_failure();

}
