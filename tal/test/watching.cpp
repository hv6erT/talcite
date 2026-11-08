#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "tal/bytes.hpp"
#include "tal/executor.hpp"
#include "tal/process.hpp"
#include "tal/task.hpp"
#include "tal/testing.hpp"

// The watch of the runner: a trial that runs longer than the limit is named in the output and ends
// the run, so that a hang on some system says which trial it was instead of holding the run until
// the verify script gives up. The trial starts this program again with a one second limit and asks
// it for the one trial below, which sleeps only then. On Windows (x64 and x86) and on Linux the
// watch writes the stacks of the other threads before it ends the run, bare and named.

using namespace std::chrono_literals;

// Sleeps past a short watch when the trial of the watch asks for it (`TAL_TRIAL_NAP`); in an
// ordinary run it passes at once.
TRIAL("Testing: a trial that oversleeps when asked") {
    if (std::getenv("TAL_TRIAL_NAP") == nullptr) return;

    std::this_thread::sleep_for(5s);
}

// The run is named hung after the limit, ends with code 3 and no summary, and on the systems with
// stacks it writes them; on Linux the sleeping trial's thread shows where it sleeps.
TRIAL("Testing: the watch names a hung trial, writes the stacks and ends the run") {
    tal::Loop loop;
    tal::Command again;

    again.program = tal::testing::mine();
    again.arguments = {"--only", "Testing: a trial that oversleeps when asked"};
    again.environment = {tal::Variable{"TAL_TRIAL_LIMIT", "1"}, tal::Variable{"TAL_TRIAL_NAP", "1"}};

    const auto began = std::chrono::steady_clock::now();
    const tal::Output got = tal::wait(loop, tal::Process::run(again));
    const std::string said = tal::text::of(got.output);

    OUGHT(said.find("  HUNG  Testing: a trial that oversleeps when asked\n") != std::string::npos);
    OUGHT(said.find("Passed") == std::string::npos);
#if (defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__))) || defined(__linux__)
    OUGHT(said.find("\n        thread ") != std::string::npos);
    OUGHT(said.find("\n        the same, named:\n") != std::string::npos);
#endif
#if defined(__linux__)
    OUGHT(said.find("nanosleep", said.find("the same, named:")) != std::string::npos);
#endif
    SAME(got.exit.code, 3);
    OUGHT(std::chrono::steady_clock::now() - began < 4s);
}
