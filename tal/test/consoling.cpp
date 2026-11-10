#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "tal/console.hpp"

#include "tal/testing.hpp"

#ifndef _WIN32

#include <unistd.h>

#endif

// The console port: the scripted console that trials and scenes talk to, and on POSIX the system's
// console, whose hearing an interruption from another thread ends.

namespace {

using namespace std::chrono_literals;

using tal::Console;
using tal::ScriptedConsole;

TRIAL("Scripted console: replies whole or in pieces, then nothing at once; every round noted") {
    ScriptedConsole whole{"abcdef"};
    ScriptedConsole pieces{"abcdef", 4};

    SAME(whole.hear(std::nullopt), std::string{"abcdef"});
    SAME(whole.hear(10ms), std::string{});
    SAME(pieces.hear(5ms), std::string{"abcd"});
    SAME(pieces.hear(std::nullopt), std::string{"ef"});
    SAME(whole.rounds, 2);
    OUGHT(whole.patience == (std::vector<std::optional<std::chrono::milliseconds>>{std::nullopt, 10ms}));
}

TRIAL("Scripted console: raw mode nests, a resume is told once, interruptions and writes are kept") {
    ScriptedConsole one;

    one.raw(true);
    one.raw(true);
    one.raw(false);
    one.raw(false);
    one.raw(false);

    SAME(one.depth, 0);
    SAME(one.entered, 2);

    one.comeback = true;

    OUGHT(one.resumed());
    OUGHT(!one.resumed());

    one.interrupt();
    one.write("ab");
    one.write("c");

    SAME(one.interrupted, 1);
    SAME(one.written, std::string{"abc"});
    SAME(one.variable("TERM"), std::optional<std::string>{"xterm-256color"});
    OUGHT(!one.variable("NOTHING").has_value());
}

#ifndef _WIN32

// Standard input replaced by the reading end of a pipe for as long as it lives, the writing end
// kept open, so that the system console has something to wait on that never ends by itself.
class Piped {
public:
    Piped() {
        OUGHT(::pipe(ends_) == 0);

        was_ = ::dup(STDIN_FILENO);

        OUGHT(::dup2(ends_[0], STDIN_FILENO) == STDIN_FILENO);
    }

    ~Piped() {
        (void)::dup2(was_, STDIN_FILENO);
        (void)::close(was_);
        (void)::close(ends_[0]);
        (void)::close(ends_[1]);
    }

    Piped(const Piped&) = delete;
    Piped& operator=(const Piped&) = delete;

    // Writes `text` to the pipe, for the console to hear.
    void said(std::string_view text) { OUGHT(::write(ends_[1], text.data(), text.size()) == static_cast<ssize_t>(text.size())); }

private:
    int ends_[2] = {-1, -1};
    int was_ = -1;
};

TRIAL("System console: an interruption from another thread ends a hearing without patience") {
    Piped input;
    const std::shared_ptr<Console> console = Console::builtin();
    std::atomic<bool> done{false};
    std::string heard{"unset"};

    std::thread listening{[&] {
        heard = console->hear(std::nullopt);
        done = true;
    }};

    std::this_thread::sleep_for(50ms);

    const bool early = done;

    console->interrupt();

    const bool ended = tal::testing::came([&] { return done.load(); }, 5s);

    if (!ended) input.said("x");

    listening.join();

    OUGHT(!early);
    OUGHT(ended);
    SAME(heard, std::string{});
}

TRIAL("System console: an interruption while nobody hears ends the next hearing, once") {
    Piped input;
    const std::shared_ptr<Console> console = Console::builtin();

    console->interrupt();

    const auto began = std::chrono::steady_clock::now();

    SAME(console->hear(std::nullopt), std::string{});
    OUGHT(std::chrono::steady_clock::now() - began < 2s);

    const auto then = std::chrono::steady_clock::now();

    SAME(console->hear(30ms), std::string{});
    OUGHT(std::chrono::steady_clock::now() - then >= 30ms);

    input.said("ab");

    SAME(console->hear(1s), std::string{"ab"});
}

#endif

}
