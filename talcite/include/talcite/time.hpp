#pragma once

#include <chrono>

#include "talcite/promise.hpp"

namespace talcite {

class Stopwatch {
public:
    Stopwatch();

    [[nodiscard]] std::chrono::milliseconds spent() const;

    void reset();

private:
    std::chrono::steady_clock::time_point began_;
};

class Time {
public:
    Time() = delete;

    [[nodiscard]] static std::chrono::steady_clock::time_point now();

    [[nodiscard]] static std::chrono::sys_seconds stamp();
};

}
