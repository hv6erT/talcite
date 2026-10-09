#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "talcite/bell.hpp"

namespace talcite {

enum class Lane : std::uint8_t { io, cpu };

class Pool {
public:
    using Job = std::uint64_t;

    explicit Pool(std::size_t most = 0);

    ~Pool();

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    Pool(Pool&&) = delete;
    Pool& operator=(Pool&&) = delete;

    Job post(std::function<void()> what, Lane lane = Lane::io);

    Job post(std::function<void()> what, Lane lane, std::atomic<Job>& into);

    void drop(Job which);

    void drain();

    [[nodiscard]] std::size_t staff() const;

    [[nodiscard]] std::size_t most() const { return most_; }

private:
    struct Doing {
        Job which = 0;
        std::function<void()> what;
    };

    Job handed(std::function<void()> what, Lane lane, std::atomic<Job>* into);

    [[nodiscard]] static bool pulled(std::deque<Doing>& from, Job which);

    void serve();

    mutable std::mutex guard_;
    std::condition_variable stirred_;
    std::condition_variable emptied_;
    std::deque<Doing> io_;
    std::deque<Doing> cpu_;
    std::unordered_map<Job, Bell*> running_;
    std::vector<std::thread> staff_;
    Job next_ = 0;
    std::size_t most_ = 0;
    std::size_t spare_ = 0;
    std::size_t cpu_busy_ = 0;
    std::size_t idle_ = 0;
    std::size_t busy_ = 0;
    bool closing_ = false;
};

[[nodiscard]] Pool& pooling();

[[nodiscard]] Bell* ringing();

}
