#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "talcite/pool.hpp"

namespace talcite::detail {

enum class State : std::uint8_t { fresh, running, resolved, rejected };

class Waiter {
public:
    virtual ~Waiter() = default;

    Waiter(const Waiter&) = delete;
    Waiter& operator=(const Waiter&) = delete;
    Waiter(Waiter&&) = delete;
    Waiter& operator=(Waiter&&) = delete;

    virtual void woke() = 0;

protected:
    Waiter() = default;
};

struct Ticket {
    virtual ~Ticket() = default;

    [[nodiscard]] virtual double progress() const;

    virtual void dropped();

    std::mutex guard;
    std::condition_variable stirred;
    std::atomic<State> state{State::fresh};
    Lane lane = Lane::io;
    std::atomic<bool> quitted{false};
    std::atomic<std::uint64_t> job{0};
    std::exception_ptr why;
    std::vector<std::weak_ptr<Waiter>> watching;
};

template <typename T>
struct Result : Ticket {
    std::optional<T> value;
};

template <>
struct Result<void> : Ticket {};

[[nodiscard]] bool over(const Ticket& ticket);

[[nodiscard]] bool hooked(Ticket& ticket, const std::shared_ptr<Waiter>& one);

void woken(Ticket& ticket);

void spoiled(Ticket& ticket, std::exception_ptr why);

inline void settled(Result<void>& result) {
    {
        const std::lock_guard<std::mutex> held{result.guard};
        if (over(result)) return;

        result.state.store(State::resolved);
    }

    woken(result);
}

template <typename T>
void settled(Result<T>& result, T given) {
    {
        const std::lock_guard<std::mutex> held{result.guard};
        if (over(result)) return;

        result.value.emplace(std::move(given));
        result.state.store(State::resolved);
    }

    woken(result);
}

}
