#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string_view>

#include "tal/port.hpp"

namespace tal {

class Clock : public Adapter {
public:
    static constexpr std::string_view kind = "clock";

    [[nodiscard]] virtual std::chrono::steady_clock::time_point now() const = 0;

    [[nodiscard]] virtual std::chrono::system_clock::time_point calendar() const = 0;

    [[nodiscard]] virtual bool manual() const { return false; }

    [[nodiscard]] static std::shared_ptr<Clock> builtin();
};

class ManualClock : public Clock {
public:
    ManualClock();

    [[nodiscard]] std::string_view name() const override { return "manual"; }

    [[nodiscard]] std::chrono::steady_clock::time_point now() const override;

    [[nodiscard]] std::chrono::system_clock::time_point calendar() const override;

    [[nodiscard]] bool manual() const override { return true; }

    void advance(std::chrono::steady_clock::duration much);

private:
    mutable std::mutex guard_;
    std::chrono::steady_clock::time_point now_;
    std::chrono::system_clock::time_point calendar_;
};

}
