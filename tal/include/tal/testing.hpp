#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "tal/executor.hpp"
#include "tal/path.hpp"

namespace tal::testing {

class Expected {
public:
    Expected();
    ~Expected();

    Expected(const Expected&) = delete;
    Expected& operator=(const Expected&) = delete;

    [[nodiscard]] std::uint64_t failures() const;

private:
    std::uint64_t start_;
};

[[nodiscard]] Expected expect_failure();

struct Trial {
    std::string_view named;
    std::string_view file;
    int line = 0;
    std::function<void()> run;
};

class Adding {
public:
    Adding(std::string_view named, std::string_view file, int line, std::function<void()> run);
};

class Pretending {
public:
    Pretending(std::string_view named, int (*play)(std::span<const std::string> arguments));
};

class Failed : public std::exception {
public:
    explicit Failed(std::string said);

    [[nodiscard]] const char* what() const noexcept override;

private:
    std::string said_;
};

[[noreturn]] void fell(std::string_view file, int line, std::string said);

template <typename Held>
[[nodiscard]] std::string spelled(const Held& one) {
    if constexpr (requires(std::ostringstream& into) { into << one; }) {
        std::ostringstream into;

        into << one;

        return into.str();
    } else {
        return "<not printable>";
    }
}

template <typename Left, typename Right>
void matched(std::string_view file, int line, std::string_view shown, const Left& left, const Right& right) {
    if (left == right) return;

    fell(file, line, std::string{shown} + "\n        left:  " + spelled(left) + "\n        right: " + spelled(right));
}

[[nodiscard]] const Path& mine();

int run(int count, char** given);

template <typename Condition>
void waited(std::string_view file, int line, std::string_view shown, Loop& loop, Condition done,
            std::chrono::milliseconds patience = std::chrono::seconds{10}) {
    const auto latest = std::chrono::steady_clock::now() + patience;

    while (!done()) {
        const auto now = std::chrono::steady_clock::now();

        if (now >= latest) fell(file, line, std::string{shown});

        (void)loop.turn(std::min(latest, now + std::chrono::milliseconds{10}));
    }
}

template <typename Condition>
[[nodiscard]] bool came(Condition done, std::chrono::milliseconds patience = std::chrono::seconds{10}) {
    const auto latest = std::chrono::steady_clock::now() + patience;

    while (!done()) {
        const auto now = std::chrono::steady_clock::now();

        if (now >= latest) return false;

        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(latest - now, std::chrono::milliseconds{10}));
    }

    return true;
}

template <typename Condition>
void waited(std::string_view file, int line, std::string_view shown, Condition done,
            std::chrono::milliseconds patience = std::chrono::seconds{10}) {
    if (!came(std::move(done), patience)) fell(file, line, std::string{shown});
}

}

#define TAL_TRIAL_JOINED(one, two) TAL_TRIAL_GLUED(one, two)
#define TAL_TRIAL_GLUED(one, two) one##two

#define TRIAL(named)                                                                                                   \
    static void TAL_TRIAL_JOINED(tal_trial_, __LINE__)();                                                              \
    static const ::tal::testing::Adding TAL_TRIAL_JOINED(tal_adding_, __LINE__){named, __FILE__, __LINE__,             \
                                                                                &TAL_TRIAL_JOINED(tal_trial_, __LINE__)}; \
    static void TAL_TRIAL_JOINED(tal_trial_, __LINE__)()

#define OUGHT(...)                                                                                                     \
    do {                                                                                                               \
        if (!(__VA_ARGS__)) ::tal::testing::fell(__FILE__, __LINE__, "not true: " #__VA_ARGS__);                       \
    } while (false)

#define SAME(given, wanted)                                                                                            \
    ::tal::testing::matched(__FILE__, __LINE__, "differ: " #given " vs " #wanted, (given), (wanted))

#define THREW(...)                                                                                                     \
    do {                                                                                                               \
        bool tal_thrown = false;                                                                                       \
        try {                                                                                                          \
            __VA_ARGS__;                                                                                               \
        } catch (const std::exception&) {                                                                              \
            tal_thrown = true;                                                                                         \
        }                                                                                                              \
        if (!tal_thrown) ::tal::testing::fell(__FILE__, __LINE__, "did not throw: " #__VA_ARGS__);                     \
    } while (false)

#define UNTIL(loop, ...)                                                                                               \
    ::tal::testing::waited(__FILE__, __LINE__, "never came: " #__VA_ARGS__, (loop),                                   \
                           [&]() -> bool { return static_cast<bool>(__VA_ARGS__); })

#define SOON(...)                                                                                                      \
    ::tal::testing::waited(__FILE__, __LINE__, "never came: " #__VA_ARGS__,                                           \
                           [&]() -> bool { return static_cast<bool>(__VA_ARGS__); })
