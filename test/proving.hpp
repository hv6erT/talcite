#pragma once

#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace proving {

struct Trial {
    std::string_view named;
    std::string_view file;
    int line = 0;
    std::function<void()> run;
};

[[nodiscard]] std::vector<Trial>& gathered();

[[nodiscard]] const std::string& mine();

class Failed : public std::exception {
public:
    explicit Failed(std::string said) : said_(std::move(said)) {}

    [[nodiscard]] const char* what() const noexcept override { return said_.c_str(); }

private:
    std::string said_;
};

struct Adding {
    Adding(std::string_view named, std::string_view file, int line, std::function<void()> run) {
        gathered().push_back(Trial{named, file, line, std::move(run)});
    }
};

template <typename Held>
[[nodiscard]] std::string spelled(const Held& one) {
    if constexpr (requires(std::ostringstream& into) { into << one; }) {
        std::ostringstream into;
        into << one;

        return into.str();
    } else {
        return "<nie do pokazania>";
    }
}

void fell(std::string_view file, int line, const std::string& said);

template <typename Left, typename Right>
void matched(std::string_view file, int line, std::string_view shown, const Left& left,
             const Right& right) {
    if (left == right) return;

    fell(file, line,
         std::string{shown} + "\n        po lewej:  " + spelled(left) +
             "\n        po prawej: " + spelled(right));
}

}

#define TRIAL_JOINED(one, two) TRIAL_GLUED(one, two)
#define TRIAL_GLUED(one, two) one##two

#define TRIAL(named)                                                                     \
    static void TRIAL_JOINED(trial_, __LINE__)();                                        \
    static const ::proving::Adding TRIAL_JOINED(adding_, __LINE__){                      \
        named, __FILE__, __LINE__, &TRIAL_JOINED(trial_, __LINE__)};                     \
    static void TRIAL_JOINED(trial_, __LINE__)()

#define ought(...)                                                                       \
    do {                                                                                 \
        if (!(__VA_ARGS__))                                                              \
            ::proving::fell(__FILE__, __LINE__, "nie zaszło: " #__VA_ARGS__);             \
    } while (false)

#define same(given, wanted)                                                              \
    ::proving::matched(__FILE__, __LINE__, "różne: " #given " vs " #wanted, (given), (wanted))

#define threw(...)                                                                       \
    do {                                                                                 \
        bool thrown = false;                                                             \
        try {                                                                            \
            __VA_ARGS__;                                                                 \
        } catch (const std::exception&) {                                                \
            thrown = true;                                                               \
        }                                                                                \
        if (!thrown)                                                                     \
            ::proving::fell(__FILE__, __LINE__, "nie rzuciło: " #__VA_ARGS__);            \
    } while (false)
