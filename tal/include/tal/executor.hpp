#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "tal/clock.hpp"
#include "tal/work.hpp"

namespace tal {

class Executor {
public:
    virtual ~Executor() = default;

    virtual void post(Work what) = 0;
};

class Loop : public Executor {
public:
    explicit Loop(Port<Clock> time = {});

    ~Loop() override;

    Loop(const Loop&) = delete;
    Loop& operator=(const Loop&) = delete;

    void post(Work what) override;

    bool turn();

    bool turn(std::chrono::steady_clock::time_point latest);

    int run();

    void pause();

    void stop(int code = 0);

    [[nodiscard]] Clock& clock() const { return *time_; }

    [[nodiscard]] std::uint64_t wake(std::chrono::steady_clock::time_point when, Work what);

    void unwake(std::uint64_t which);

    [[nodiscard]] bool idle() const;

    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> next() const;

protected:
    virtual void stirred();

private:
    Port<Clock> time_;
    mutable std::mutex guard_;
    std::condition_variable stirred_;
    std::deque<Work> ready_;
    std::multimap<std::chrono::steady_clock::time_point, std::uint64_t> due_;
    std::map<std::uint64_t, Work> timers_;
    std::uint64_t numbered_ = 0;
    bool stopping_ = false;
    int code_ = 0;
};

class Pool : public Executor {
public:
    explicit Pool(unsigned threads);

    ~Pool() override;

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    void post(Work what) override;

private:
    void worked();

    std::mutex guard_;
    std::condition_variable stirred_;
    std::deque<Work> ready_;
    std::vector<std::thread> threads_;
    bool stopping_ = false;
};

[[nodiscard]] Loop& loop();

[[nodiscard]] Executor& pool();

[[nodiscard]] Executor& io();

}
