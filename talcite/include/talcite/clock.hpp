#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>

#include "talcite/detail/deadlines.hpp"

namespace talcite {

class Clock {
public:
    using Moment = std::chrono::steady_clock::time_point;

    Clock() = default;

    virtual ~Clock() = default;

    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    Clock(Clock&&) = delete;
    Clock& operator=(Clock&&) = delete;

    [[nodiscard]] virtual Moment now() const = 0;

    [[nodiscard]] virtual std::uint64_t scheduled(Moment when, std::function<void()> then) = 0;

    [[nodiscard]] virtual bool unscheduled(std::uint64_t which) = 0;

    [[nodiscard]] static Clock& real();
};

class ManualClock : public Clock {
public:
    ManualClock() = default;

    [[nodiscard]] Moment now() const override;

    [[nodiscard]] std::uint64_t scheduled(Moment when, std::function<void()> then) override;

    [[nodiscard]] bool unscheduled(std::uint64_t which) override;

    void advance(std::chrono::milliseconds by);

    [[nodiscard]] std::size_t waiting() const;

private:
    mutable std::mutex guard_;
    Moment now_{};
    detail::Deadlines<std::function<void()>> waiting_;
};

}
