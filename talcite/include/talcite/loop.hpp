#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <vector>

#include "talcite/bell.hpp"

namespace talcite {

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

    [[nodiscard]] Bell& bell() { return bell_; }

    [[nodiscard]] static Loop* current();

private:
    mutable std::mutex guard_;
    std::vector<std::function<void()>> queued_;
    Bell bell_;
};

}
