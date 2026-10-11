#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace talcite {

class Bell {
public:
    struct Ear {
        std::mutex* guard = nullptr;
        std::condition_variable* stirred = nullptr;
    };

    Bell();

    ~Bell();

    Bell(const Bell&) = delete;
    Bell& operator=(const Bell&) = delete;
    Bell(Bell&&) = delete;
    Bell& operator=(Bell&&) = delete;

    void ring();

    void clear();

    [[nodiscard]] bool wait(std::chrono::milliseconds patience);

    [[nodiscard]] bool rang() const;

    Ear heed(Ear ear);

#ifdef _WIN32
    [[nodiscard]] void* waited() const { return event_; }
#else
    [[nodiscard]] int waited() const { return heard_; }
#endif

private:
    void spread();

    std::atomic<bool> rang_{false};
    std::mutex ear_guard_;
    Ear ear_{};

#ifdef _WIN32
    void* event_ = nullptr;
#else
    int heard_ = -1;
    int rung_ = -1;
#endif
};

}
