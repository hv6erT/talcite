#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace tal::testing::detail {

class Watch {
public:
    Watch();
    ~Watch();

    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;

    void begun(std::string_view named);

private:
    void watched();

    std::chrono::seconds limit_;
    std::mutex guard_;
    std::condition_variable wake_;
    std::string named_;
    std::chrono::steady_clock::time_point since_{};
    bool done_ = false;
    std::thread thread_;
};

}
