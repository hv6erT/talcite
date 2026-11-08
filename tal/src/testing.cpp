#include "tal/testing.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "platform/core.hpp"
#include "tal/log.hpp"
#include "tal/text.hpp"
#include "watch.hpp"

#ifdef _WIN32

#include <fcntl.h>
#include <io.h>
#include <windows.h>

#include <shellapi.h>

#endif

namespace tal::testing {

namespace {

// Every trial registered by `TRIAL`, in the order the files were linked and the trials written.
std::vector<Trial>& gathered() {
    static std::vector<Trial> held;

    return held;
}

// Every program the trial binary can pretend to be, by name.
std::map<std::string, int (*)(std::span<const std::string>), std::less<>>& pretenders() {
    static std::map<std::string, int (*)(std::span<const std::string>), std::less<>> held;

    return held;
}

// The trial binary as it was started, absolute.
Path& called() {
    static Path held;

    return held;
}

// The file name without its folders, as `__FILE__` may carry either kind of separator.
std::string_view bared(std::string_view file) {
    const std::size_t at = file.find_last_of("/\\");

    return at == std::string_view::npos ? file : file.substr(at + 1);
}

// The arguments in UTF-8. On Windows `main` gets them in the ANSI code page, which has no place
// for many letters, so they are taken again from the wide command line.
std::vector<std::string> worded(int count, char** given) {
#ifdef _WIN32
    (void)given;

    int many = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &many);
    std::vector<std::string> made;

    if (wide == nullptr) return made;

    for (int at = 0; at < many; ++at) made.push_back(text::narrow(wide[at]));

    LocalFree(wide);

    (void)count;

    return made;
#else
    return std::vector<std::string>{given, given + count};
#endif
}

// The path the binary was started with, made absolute while the working folder is still the one
// it was started in, so that a trial that starts the binary again finds it wherever the trials
// before it went. A bare name was found through PATH and stays as it is.
Path started(const std::string& said) {
    const platform::path given = platform::core::native(Path{said});

    if (said.find_first_of("/\\") == std::string::npos) return platform::core::told(given);

    std::error_code trouble;
    const platform::path made = platform::stood::absolute(given, trouble);

    return platform::core::told(trouble ? given : made);
}

// Sets the process up as every suite needs it before anything runs. On Windows output is UTF-8 and
// binary, so that the bytes are the same as elsewhere and a trial reading a run's output compares
// them, and a program the trials start that cannot load a library ends with a status instead of a
// window nobody sees (the programs started inherit this).
void prepared() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    (void)_setmode(_fileno(stdout), _O_BINARY);
    (void)_setmode(_fileno(stderr), _O_BINARY);
    (void)_setmode(_fileno(stdin), _O_BINARY);
#endif
}

}

// While it lives, an unhandled failure goes to the log only and does not stop a debug build.
Expected::Expected() : start_(::tal::detail::unhandled_count()) { ::tal::detail::tolerate(1); }

// Unhandled failures stop a debug build again, unless another guard still expects them.
Expected::~Expected() { ::tal::detail::tolerate(-1); }

// How many unhandled failures came, from any thread, since the guard was made.
std::uint64_t Expected::failures() const { return ::tal::detail::unhandled_count() - start_; }

// A guard for a trial that makes work fail unhandled on purpose.
Expected expect_failure() { return {}; }

// Registers the trial `named`, written in `file` at `line`.
Adding::Adding(std::string_view named, std::string_view file, int line, std::function<void()> run) {
    gathered().push_back(Trial{named, file, line, std::move(run)});
}

// Registers a program the binary plays when started with `--pretending` and `named`.
Pretending::Pretending(std::string_view named, int (*play)(std::span<const std::string> arguments)) {
    pretenders().emplace(std::string{named}, play);
}

// A failure of the running trial, with its place and reason.
Failed::Failed(std::string said) : said_(std::move(said)) {}

// The place and the reason of the failure.
const char* Failed::what() const noexcept { return said_.c_str(); }

// Ends the running trial as failed, with the place in the source and the reason.
void fell(std::string_view file, int line, std::string said) {
    throw Failed{std::string{bared(file)} + ":" + std::to_string(line) + "  " + said};
}

// The trial binary as it was started, absolute, so that a trial can start it again.
const Path& mine() { return called(); }

// Runs the trials: all of them, those of the files named (`--file`, as many times as there are
// files; the trials keep their order) or one by name (`--only`), or lists them (`--list`). With
// `--pretending` first the binary plays a registered program instead. Returns 0 only when every
// trial that ran passed.
int run(int count, char** given) {
    prepared();

    const std::vector<std::string> words = worded(count, given);

    if (!words.empty()) called() = started(words[0]);

    if (words.size() > 1 && words[1] == "--pretending") {
        const std::string named = words.size() > 2 ? words[2] : std::string{};
        const auto found = pretenders().find(named);

        if (found == pretenders().end()) {
            std::fprintf(stderr, "Unknown pretending: %s\n", named.c_str());

            return 2;
        }

        const std::vector<std::string> rest{words.begin() + std::min<std::ptrdiff_t>(3, std::ssize(words)), words.end()};

        return found->second(rest);
    }

    std::string only;
    std::set<std::string, std::less<>> chosen;
    bool listing = false;

    for (std::size_t at = 1; at < words.size(); ++at) {
        const std::string& one = words[at];

        if (one == "--list") {
            listing = true;
        } else if (one == "--file" && at + 1 < words.size()) {
            chosen.insert(words[++at]);
        } else if (one == "--only" && at + 1 < words.size()) {
            only = words[++at];
        } else {
            std::cerr << "Usage: " << std::string{mine().name()} << " [--list] [--file <file>]... [--only <name>]\n";

            return 2;
        }
    }

    const std::vector<Trial>& held = gathered();

    if (listing) {
        for (const Trial& one : held) std::cout << bared(one.file) << "  " << one.named << "\n";

        return 0;
    }

    std::size_t passed = 0;
    std::size_t ran = 0;

    // Made before the first trial and static, so it outlives what the trials leave behind: the
    // pools and loops they start are made later and destroyed first, at the end of the program,
    // with the watch still running. A hang there (a worker that never ends) is named too.
    static detail::Watch watch;

    for (const Trial& one : held) {
        if (!only.empty() && one.named != only) continue;
        if (!chosen.empty() && !chosen.contains(bared(one.file))) continue;

        ++ran;

        watch.begun(one.named);

        try {
            one.run();
            ++passed;
            std::cout << "  ok    " << one.named << "\n";
        } catch (const Failed& why) {
            std::cout << "  FAIL  " << one.named << "\n        " << why.what() << "\n";
        } catch (const std::exception& why) {
            std::cout << "  FAIL  " << one.named << "\n        threw: " << why.what() << "\n";
        }

        // Each verdict goes out at once: written to a file, the output would otherwise wait in a
        // buffer, and a run that hangs or is stopped would end several trials before the one that
        // hung.
        std::cout.flush();
    }

    std::cout << "\nPassed " << passed << " of " << ran << ".\n" << std::flush;

    watch.begun("the end of the run");

    return passed == ran ? 0 : 1;
}

}
