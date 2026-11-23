#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "tal/trouble.hpp"

namespace tal {

class Ticking {
public:
    Ticking(double dt, std::function<void()> step, std::function<void()> settle);

    ~Ticking();

    Ticking(const Ticking&) = delete;
    Ticking& operator=(const Ticking&) = delete;

    void advance(std::chrono::nanoseconds real);

    void step();

    void run(bool on);

    [[nodiscard]] bool running() const;

    void pace(double scale);

    [[nodiscard]] double pace() const;

    void woken();

    [[nodiscard]] bool busy() const;

    [[nodiscard]] std::uint64_t steps() const;

    [[nodiscard]] double dt() const;

    [[nodiscard]] std::optional<Trouble> trouble() const;

private:
    struct Held;

    std::unique_ptr<Held> held_;
};

template <typename State>
class Simulation {
public:
    using Stepper = std::function<void(State& state, double dt)>;

    Simulation(State start, Stepper stepper, double dt)
        : working_(start), published_(std::move(start)), stepper_(std::move(stepper)), ticking_(dt, [this] { stepper_(working_, ticking_.dt()); }, [this] { settled(); }) {}

    void advance(std::chrono::nanoseconds real) { ticking_.advance(real); }

    void step() { ticking_.step(); }

    void run(bool on) { ticking_.run(on); }

    [[nodiscard]] bool running() const { return ticking_.running(); }

    void pace(double scale) { ticking_.pace(scale); }

    [[nodiscard]] double pace() const { return ticking_.pace(); }

    [[nodiscard]] State latest() const {
        const std::lock_guard<std::mutex> lock{mutex_};

        return published_;
    }

    template <typename Change>
    void changed(Change change) {
        {
            const std::lock_guard<std::mutex> lock{mutex_};

            changes_.push_back(std::function<void(State&)>{std::move(change)});
        }

        ticking_.woken();
    }

    [[nodiscard]] bool busy() const { return ticking_.busy(); }

    [[nodiscard]] std::uint64_t steps() const { return ticking_.steps(); }

    [[nodiscard]] double time() const { return static_cast<double>(ticking_.steps()) * ticking_.dt(); }

    [[nodiscard]] std::optional<Trouble> trouble() const { return ticking_.trouble(); }

private:
    void settled() {
        std::vector<std::function<void(State&)>> waiting;

        {
            const std::lock_guard<std::mutex> lock{mutex_};

            waiting.swap(changes_);
        }

        for (std::function<void(State&)>& one : waiting) one(working_);

        const std::lock_guard<std::mutex> lock{mutex_};

        published_ = working_;
    }

    State working_;
    mutable std::mutex mutex_;
    State published_;
    std::vector<std::function<void(State&)>> changes_;
    Stepper stepper_;
    Ticking ticking_;
};

}
