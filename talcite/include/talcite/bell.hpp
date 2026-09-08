#pragma once

#include <chrono>

namespace talcite {

class Bell {
public:
    Bell();

    ~Bell();

    Bell(const Bell&) = delete;
    Bell& operator=(const Bell&) = delete;
    Bell(Bell&&) = delete;
    Bell& operator=(Bell&&) = delete;

    void ring();

    void clear();

    [[nodiscard]] bool wait(std::chrono::milliseconds patience);

#ifdef _WIN32
    [[nodiscard]] void* waited() const { return event_; }
#else
    [[nodiscard]] int waited() const { return heard_; }
#endif

private:
#ifdef _WIN32
    void* event_ = nullptr;
#else
    int heard_ = -1;
    int rung_ = -1;
#endif
};

}
