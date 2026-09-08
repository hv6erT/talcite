#pragma once

#include <chrono>
#include <stdexcept>
#include <utility>

#include "talcite/promise.hpp"

namespace talcite {

class Overdue : public std::runtime_error {
public:
    Overdue();
};

class Time {
public:
    Time();

    [[nodiscard]] std::chrono::milliseconds spent() const;

    void reset();

    [[nodiscard]] static std::chrono::steady_clock::time_point now();

    [[nodiscard]] static std::chrono::sys_seconds stamp();

    [[nodiscard]] static Promise<void> later(std::chrono::milliseconds patience);

    [[nodiscard]] static Promise<void> limit(std::chrono::milliseconds patience);

    template <Promised One>
    [[nodiscard]] static Promise<void> within(One one, std::chrono::milliseconds patience) {
        return Promise<void>::race(std::move(one), limit(patience));
    }

private:
    std::chrono::steady_clock::time_point began_;
};

}
