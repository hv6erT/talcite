#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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

    explicit Pool(std::size_t most = 0, std::chrono::milliseconds rest = std::chrono::seconds{30});

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
    friend class Waiting;

    Job handed(std::function<void()> what, Lane lane, std::atomic<Job>* into);

    [[nodiscard]] std::size_t runnable() const;

    void hired(bool strict);

    [[nodiscard]] std::vector<std::thread> reaped();

    void serve(std::unique_ptr<Bell> mine);

    mutable std::mutex guard_;
    std::condition_variable stirred_;
    std::condition_variable emptied_;
    std::map<Job, std::function<void()>> io_;
    std::map<Job, std::function<void()>> cpu_;
    std::unordered_map<Job, Bell*> running_;
    std::vector<std::thread> staff_;
    std::vector<std::thread::id> retired_;
    Job next_ = 0;
    std::size_t most_ = 0;
    std::chrono::milliseconds rest_;
    std::size_t alive_ = 0;
    std::size_t cpu_busy_ = 0;
    std::size_t idle_ = 0;
    std::size_t busy_ = 0;
    bool closing_ = false;
};

class Waiting {
public:
    Waiting();

    ~Waiting();

    Waiting(const Waiting&) = delete;
    Waiting& operator=(const Waiting&) = delete;
    Waiting(Waiting&&) = delete;
    Waiting& operator=(Waiting&&) = delete;

private:
    Pool* pool_ = nullptr;
};

[[nodiscard]] Pool& pooling();

[[nodiscard]] Bell* ringing();

}
