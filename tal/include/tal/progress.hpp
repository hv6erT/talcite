#pragma once

#include <atomic>
#include <cstdint>

namespace tal {

class Progress {
public:
    Progress() = default;

    Progress(const Progress&) = delete;
    Progress& operator=(const Progress&) = delete;

    [[nodiscard]] std::uint64_t done() const { return done_.load(); }

    [[nodiscard]] std::uint64_t total() const { return total_.load(); }

    void add(std::uint64_t much) { done_.fetch_add(much); }

    void expect(std::uint64_t total) { total_.store(total); }

private:
    std::atomic<std::uint64_t> done_{0};
    std::atomic<std::uint64_t> total_{0};
};

}
