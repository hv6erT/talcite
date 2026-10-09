#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "talcite/bell.hpp"

namespace talcite {

template <typename T>
class Promise;

namespace detail {

class Door {
public:
    void post(std::function<void()> what);

    [[nodiscard]] std::vector<std::function<void()>> taken();

    [[nodiscard]] std::size_t waiting() const;

    [[nodiscard]] Bell& bell() { return bell_; }

private:
    mutable std::mutex guard_;
    std::vector<std::function<void()>> queued_;
    Bell bell_;
};

}

class Loop {
public:
    Loop();

    ~Loop();

    Loop(const Loop&) = delete;
    Loop& operator=(const Loop&) = delete;
    Loop(Loop&&) = delete;
    Loop& operator=(Loop&&) = delete;

    void post(std::function<void()> what);

    std::size_t turn();

    [[nodiscard]] std::size_t waiting() const;

    [[nodiscard]] Bell& bell() { return door_->bell(); }

    [[nodiscard]] static Loop* current();

private:
    template <typename T>
    friend class Promise;

    std::shared_ptr<detail::Door> door_ = std::make_shared<detail::Door>();
};

}
