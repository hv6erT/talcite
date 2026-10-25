#include "proving.hpp"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "talcite/child.hpp"
#include "talcite/encoding.hpp"
#include "talcite/fs.hpp"

// The watch of the trial runner (proving.cpp): a trial that runs longer than the limit is named in
// the output and ends the run, so that a hang on some system says which trial it was instead of
// holding the run until the verify script gives up. The trial starts this program again with a one
// second limit and asks it for the one trial below, which sleeps only then.

using namespace std::chrono_literals;

// Sleeps past a short watch when the trial of the watch asks for it (`TALCITE_TRIAL_NAP`); in an
// ordinary run it passes at once.
TRIAL("Proving: a trial that oversleeps when asked") {
    if (std::getenv("TALCITE_TRIAL_NAP") == nullptr) return;

    std::this_thread::sleep_for(5s);
}

TRIAL("Proving: the watch names a hung trial and ends the run") {
    talcite::Command again;

    again.program = talcite::fs::from_string(proving::mine());
    again.arguments = {"--only", "Proving: a trial that oversleeps when asked"};
    again.environment = {talcite::Variable{"TALCITE_TRIAL_LIMIT", "1"}, talcite::Variable{"TALCITE_TRIAL_NAP", "1"}};

    const auto began = std::chrono::steady_clock::now();
    const talcite::Output got = talcite::Child::run(again).timeout(30s).get();
    const std::string said = talcite::text(got.out);

    ought(said.find("  HUNG  Proving: a trial that oversleeps when asked\n") != std::string::npos);
    ought(said.find("Passed") == std::string::npos);
    same(got.exit.code, 3);
    ought(std::chrono::steady_clock::now() - began < 4s);
}
