#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "talcite/clock.hpp"

namespace talcite::ui {

namespace detail {

struct Timers;

}

#if defined(_MSC_VER) && !defined(__clang__)
class Clock;
#endif

class [[nodiscard]] Timer {
public:
    Timer() = default;

    Timer(Timer&& other) noexcept;

    Timer& operator=(Timer&& other) noexcept;

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    ~Timer();

    [[nodiscard]] bool active() const;

    void stop();

private:
    friend class Clock;

    Timer(std::weak_ptr<detail::Timers> timers, std::uint64_t number) : timers_(std::move(timers)), number_(number) {}

    std::weak_ptr<detail::Timers> timers_;
    std::uint64_t number_ = 0;
};

class Clock {
public:
    explicit Clock(talcite::Clock& time);

    ~Clock();

    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    Clock(Clock&&) = delete;
    Clock& operator=(Clock&&) = delete;

    [[nodiscard]] Timer after(std::chrono::milliseconds delay, std::function<void()> then);

    [[nodiscard]] Timer every(std::chrono::milliseconds period, std::function<void()> then);

    [[nodiscard]] talcite::Clock::Moment now() const;

    [[nodiscard]] std::optional<talcite::Clock::Moment> next() const;

private:
    friend class Root;

    bool elapse();

    [[nodiscard]] Timer kept(talcite::Clock::Moment when, std::chrono::milliseconds period, std::function<void()> then);

    talcite::Clock* time_;
    std::shared_ptr<detail::Timers> timers_;
};

}
