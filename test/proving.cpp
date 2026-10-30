#include "proving.hpp"

#include "talcite/host.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>

#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace proving {

namespace {

// The path this test program was started with, for trials that start it again.
std::string called;

// The file name without its directories.
[[nodiscard]] std::string_view bared(std::string_view file) {
    const std::size_t at = file.find_last_of("/\\");

    return at == std::string_view::npos ? file : file.substr(at + 1);
}

}

// Every trial registered by `TRIAL`, in the order the files were linked and the trials written.
std::vector<Trial>& gathered() {
    static std::vector<Trial> held;

    return held;
}

// The path this test program was started with.
const std::string& mine() { return called; }

// Ends the running trial as failed, with the place and the reason.
void fell(std::string_view file, int line, const std::string& said) {
    throw Failed{std::string{bared(file)} + ":" + std::to_string(line) + "  " + said};
}

}

namespace {

using proving::Trial;

// How long one trial may run before the watch calls it hung: `TALCITE_TRIAL_LIMIT` in seconds, five
// minutes without it. The slowest honest trial takes a few seconds, a few tens under a sanitizer,
// so five minutes leaves a wide margin; the trial of the watch itself sets one second.
std::chrono::seconds longest() {
    const char* given = std::getenv("TALCITE_TRIAL_LIMIT");
    const int seconds = given != nullptr ? std::atoi(given) : 0;

    return std::chrono::seconds{seconds > 0 ? seconds : 300};
}

// A watch over the trial that is running. A trial that hangs (a deadlock, a wait without a
// deadline) would otherwise hold the run until the outer timeout of the verify script, two hours,
// and the log would not say which trial it was; on Windows the log even ended several trials
// earlier, with the rest still in the buffer. The watch names the trial, flushes, and ends the
// program at once (`_Exit`): an orderly exit would wait for the hung work.
class Watch {
public:
    // Starts the watching thread; a trial may run for `limit`.
    explicit Watch(std::chrono::seconds limit) : limit_(limit), thread_([this] { watched(); }) {}

    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;

    // Stops the watching thread.
    ~Watch() {
        {
            const std::lock_guard<std::mutex> lock{guard_};

            done_ = true;
        }

        wake_.notify_all();
        thread_.join();
    }

    // The trial `named` starts now; the time between two trials counts to the next one, and it is
    // only the printing of a verdict.
    void begun(std::string_view named) {
        const std::lock_guard<std::mutex> lock{guard_};

        named_ = std::string{named};
        since_ = std::chrono::steady_clock::now();
    }

private:
    // Looks once a second whether the running trial has run longer than the limit.
    void watched() {
        std::unique_lock<std::mutex> lock{guard_};

        while (!done_) {
            wake_.wait_for(lock, std::chrono::seconds{1});

            if (!named_.empty() && std::chrono::steady_clock::now() - since_ > limit_) {
                std::fflush(stdout);
                std::fprintf(stdout, "  HUNG  %s\n        still running after %d s\n", named_.c_str(),
                             static_cast<int>(limit_.count()));
                std::fflush(stdout);
                std::_Exit(3);
            }
        }
    }

    std::chrono::seconds limit_;
    std::mutex guard_;
    std::condition_variable wake_;
    std::string named_;
    std::chrono::steady_clock::time_point since_{};
    bool done_ = false;
    std::thread thread_;
};

// The file name without its directories.
[[nodiscard]] std::string_view bared(std::string_view file) {
    const std::size_t at = file.find_last_of("/\\");

    return at == std::string_view::npos ? file : file.substr(at + 1);
}

// Writes the text to the stream as it is and flushes it at once.
void said(std::FILE* into, const std::string& what) {
    if (!what.empty()) std::fwrite(what.data(), 1, what.size(), into);

    std::fflush(into);
}

#ifdef _WIN32

// UTF-16 from Windows as UTF-8.
std::string narrowly(const std::wstring& what) {
    if (what.empty()) return {};

    const int much = WideCharToMultiByte(CP_UTF8, 0, what.c_str(), static_cast<int>(what.size()),
                                         nullptr, 0, nullptr, nullptr);

    std::string made(static_cast<std::size_t>(much), '\0');

    WideCharToMultiByte(CP_UTF8, 0, what.c_str(), static_cast<int>(what.size()), made.data(), much,
                        nullptr, nullptr);

    return made;
}

// A line typed into the console, in UTF-8, without its line end; false at the end of the input.
// The C library reads the console in the console's code page (852 or 1250 on a Polish Windows),
// which has no place for many letters and turns them into others, so a console is read as UTF-16
// (ReadConsoleW) whatever its code page; input that is no console is read as it comes.
bool typed_line(std::string& line) {
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);

    DWORD mode = 0;

    if (GetConsoleMode(in, &mode) == 0) return static_cast<bool>(std::getline(std::cin, line));

    std::wstring wide;

    while (true) {
        wchar_t one = 0;
        DWORD got = 0;

        if (ReadConsoleW(in, &one, 1, &got, nullptr) == 0 || got == 0) {
            if (wide.empty()) return false;

            break;
        }

        if (one == L'\n') break;

        wide += one;
    }

    line = narrowly(wide);

    return true;
}

// UTF-8 as UTF-16 for Windows.
std::wstring widely(const std::string& what) {
    if (what.empty()) return {};

    const int much =
        MultiByteToWideChar(CP_UTF8, 0, what.c_str(), static_cast<int>(what.size()), nullptr, 0);

    std::wstring made(static_cast<std::size_t>(much), L'\0');

    MultiByteToWideChar(CP_UTF8, 0, what.c_str(), static_cast<int>(what.size()), made.data(), much);

    return made;
}

#endif

// The arguments in UTF-8, from talcite: on Windows `main` gets them converted to the ANSI code
// page, while the trials compare bytes. The wide-character path lives in `talcite::arguments`.
std::vector<std::string> worded(int count, char** given) { return talcite::arguments(count, given); }

// The value of an environment variable in UTF-8, or "<none>" when it is not set.
std::string around(const std::string& name) {
#ifdef _WIN32

    std::wstring room(32767, L'\0');

    const DWORD much =
        GetEnvironmentVariableW(widely(name).c_str(), room.data(), static_cast<DWORD>(room.size()));

    if (much == 0) return "<none>";

    room.resize(much);

    return narrowly(room);

#else

    const char* told = std::getenv(name.c_str());

    return told == nullptr ? std::string{"<none>"} : std::string{told};

#endif
}

// The current directory in UTF-8.
std::string here() {
#ifdef _WIN32

    return narrowly(std::filesystem::current_path().wstring());

#else

    return std::filesystem::current_path().string();

#endif
}

// The path the test program was started with, made absolute when it is relative, so that a trial
// that starts the program again in another working directory still finds it. A bare name was found
// through PATH and stays as it is.
std::string started(const std::string& said) {
    const bool bare = said.find_first_of("/\\") == std::string::npos;
    const bool rooted = said.starts_with('/') || said.starts_with('\\') ||
                        (said.size() > 2 && said[1] == ':' && (said[2] == '/' || said[2] == '\\'));

    return bare || rooted ? said : here() + "/" + said;
}

// The helper program lives in the tests, not in the library: the test binary can pretend to be
// another program and starts itself, so the trials of starting programs do not depend on whether
// the machine happens to have a shell, `echo` or cmd. Returns the exit code of the pretended
// program.
//
// Called before anything else in `main`, so that pretending does not pull in the trials.
int pretending(const std::vector<std::string>& given) {
    const std::string what = given.empty() ? std::string{} : given[0];
    const std::string rest = given.size() > 1 ? given[1] : std::string{};

    if (what == "said") {
        said(stdout, rest);

        return 0;
    }

    if (what == "moaned") {
        said(stderr, rest);

        return 0;
    }

    if (what == "both") {
        said(stdout, "output");
        said(stderr, "error");

        return 0;
    }

    if (what == "echo") {
        char room[4096];

        for (;;) {
            const std::size_t got = std::fread(room, 1, sizeof(room), stdin);

            if (got == 0) break;

            std::fwrite(room, 1, got, stdout);
        }

        std::fflush(stdout);

        return 0;
    }

    if (what == "much") {
        // More than a pipe holds. A reader that forgot to read both sides at once would get stuck
        // here and wait forever.
        const std::string one(1000, 'x');

        for (int at = 0; at < 300; ++at) {
            said(stdout, one);
            said(stderr, one);
        }

        return 0;
    }

    if (what == "code") return rest.empty() ? 0 : std::atoi(rest.c_str());

    if (what == "sleeping") {
        std::this_thread::sleep_for(std::chrono::milliseconds{rest.empty() ? 0 : std::atoi(rest.c_str())});

        return 0;
    }

    // A parent with a grandchild: it starts itself as `sleeping` without talcite, by a plain
    // fork/exec or CreateProcessW, with no group of its own and no job, prints the grandchild's
    // number and sleeps as long. That is what a shell or a batch file that started the real program
    // looks like: killing the parent alone would leave the grandchild alive.
    //
    // On Windows the grandchild is `noted`, and the parent prints its number only once it has
    // written it: a process still loading its libraries does not take a console event yet, and a
    // Ctrl-Break sent a few milliseconds after its start reached the parent alone (verify.py 30.10:
    // the parent ended with 0xC000013A after 2 to 10 ms, the grandchild slept on).
    if (what == "parent") {
#ifdef _WIN32
        const std::filesystem::path note =
            std::filesystem::temp_directory_path() / ("talcite-parent-" + std::to_string(GetCurrentProcessId()));

        std::error_code gone;

        std::filesystem::remove(note, gone);

        std::wstring line = L"\"" + widely(proving::mine()) + L"\" --pretending noted \"" + note.wstring() + L"\" " + widely(rest);

        STARTUPINFOW setting{};
        setting.cb = sizeof setting;

        PROCESS_INFORMATION told{};

        if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &setting, &told) == 0)
            return 3;

        CloseHandle(told.hThread);
        CloseHandle(told.hProcess);

        for (int tries = 0; tries < 1000 && !std::filesystem::exists(note, gone); ++tries)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});

        std::filesystem::remove(note, gone);

        said(stdout, std::to_string(told.dwProcessId) + "\n");
#else
        const pid_t which = ::fork();

        if (which < 0) return 3;

        if (which == 0) {
            ::execl(proving::mine().c_str(), proving::mine().c_str(), "--pretending", "sleeping", rest.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }

        said(stdout, std::to_string(which) + "\n");
#endif

        std::this_thread::sleep_for(std::chrono::milliseconds{rest.empty() ? 0 : std::atoi(rest.c_str())});

        return 0;
    }

    // A program let go (`Child::detached`) has no pipes, so it speaks through a file: its number,
    // and on POSIX also its session and parent. The file is written under another name and renamed
    // in one piece, so that the reader never sees half of it. Then it sleeps as long as given.
    if (what == "noted") {
#ifdef _WIN32
        const std::string made = std::to_string(GetCurrentProcessId());
#else
        const std::string made =
            std::to_string(::getpid()) + " " + std::to_string(::getsid(0)) + " " + std::to_string(::getppid());
#endif
        const std::filesystem::path where{rest};

        std::filesystem::path part = where;

        part += ".part";

        {
            std::ofstream out{part, std::ios::binary};

            out << made;
        }

        std::error_code why;

        std::filesystem::rename(part, where, why);

        if (given.size() > 2) std::this_thread::sleep_for(std::chrono::milliseconds{std::atoi(given[2].c_str())});

        return why ? 4 : 0;
    }

    // A program in a terminal: it says whether its input and output are a terminal and how large
    // that is, then answers line by line: `size` gives the size again, `bar` asks for a blinking
    // bar for a cursor and `usual` for the terminal's usual one, `quit` ends it, anything else
    // comes back after "got:". The size is the window's on Windows, the terminal's on POSIX.
    if (what == "tty") {
        const auto measured = [] {
#ifdef _WIN32
            CONSOLE_SCREEN_BUFFER_INFO told{};

            if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &told) == 0) return std::string{"?"};

            return std::to_string(told.srWindow.Right - told.srWindow.Left + 1) + "x" +
                   std::to_string(told.srWindow.Bottom - told.srWindow.Top + 1);
#else
            winsize told{};

            if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &told) != 0) return std::string{"?"};

            return std::to_string(told.ws_col) + "x" + std::to_string(told.ws_row);
#endif
        };

#ifdef _WIN32
        const bool typed = _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
#else
        const bool typed = ::isatty(STDIN_FILENO) != 0 && ::isatty(STDOUT_FILENO) != 0;
#endif

#ifdef _WIN32
        // What it writes is UTF-8, so the console reads it as such.
        SetConsoleOutputCP(CP_UTF8);
#endif

        said(stdout, std::string{"tty "} + (typed ? "1 " : "0 ") + measured() + "\n");

        std::string line;

#ifdef _WIN32
        while (typed_line(line)) {
#else
        while (std::getline(std::cin, line)) {
#endif
            if (!line.empty() && line.back() == '\r') line.pop_back();

            if (line == "quit") return 0;

            if (line == "size") said(stdout, "size " + measured() + "\n");
            else if (line == "bar") said(stdout, "\x1b[5 qbarred\n");
            else if (line == "usual") said(stdout, "\x1b[0 qas usual\n");
            else said(stdout, "got:" + line + "\n");
        }

        return 0;
    }

    // A shell's prompt as a shell integrated with its terminal gives it: two lines of output, the
    // folder (OSC 7) and the marks of the prompt (OSC 133), then the prompt itself. It waits for a
    // line and ends at `quit`.
    if (what == "prompted") {
        said(stdout, "one\ntwo\n\x1b]7;file://here/tmp/prompted\x07\x1b]133;A\x07$ \x1b]133;B\x07");

        std::string line;

        while (std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();

            if (line == "quit") return 0;
        }

        return 0;
    }

    // A program that says much and goes at once: three thousand numbered lines, more than one read
    // of the terminal takes, then the mark of a finished command (OSC 133 D) and "bye". Its exit is
    // known while the rest of what it said still waits to be read.
    if (what == "farewell") {
        std::string made;

        for (int at = 0; at < 3000; ++at) made += "line " + std::to_string(at) + " of what goes before the end\n";

        made += "\x1b]133;D;0\x07" "bye\n";

        said(stdout, made);

        return 0;
    }

    // A program that leaves another one holding its terminal: on POSIX a child that sleeps three
    // seconds with the terminal for its output. It says "left" and goes.
    if (what == "forsaking") {
#ifndef _WIN32
        if (::fork() == 0) {
            (void)::sleep(3);

            ::_exit(0);
        }
#endif

        said(stdout, "left\n");

        return 0;
    }

    if (what == "where") {
        said(stdout, here());

        return 0;
    }

    if (what == "around") {
        said(stdout, rest.empty() ? std::string{"<none>"} : around(rest));

        return 0;
    }

    if (what == "args") {
        std::string made;

        for (std::size_t at = 1; at < given.size(); ++at) {
            made += given[at];
            made += '\n';
        }

        said(stdout, made);

        return 0;
    }

    std::fprintf(stderr, "Unknown pretending: %s\n", what.c_str());

    return 2;
}

}

// Runs the trials (all, one file with `--file`, one trial with `--only`), lists them with `--list`,
// or pretends to be another program with `--pretending`. Exits with 0 only when every trial passed.
int main(int count, char** given) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);

    // A program the trials start that cannot load a library (an AddressSanitizer build without its
    // runtime on PATH) ends with a status instead of a system window nobody sees, which would hold
    // the trial until the watch; the programs we start inherit this.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    // Every run writes the same bytes as on other systems: without this Windows would turn every
    // `\n` into `\r\n`, and the trials that read a run's output (the pretended programs, the
    // watch) would compare nothing.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif

    const std::vector<std::string> words = worded(count, given);

    if (!words.empty()) proving::called = started(words[0]);

    if (words.size() > 1 && words[1] == "--pretending")
        return pretending(std::vector<std::string>{words.begin() + 2, words.end()});

    const int many = static_cast<int>(words.size());

    std::string only;
    std::string chosen;
    bool listing = false;

    for (int at = 1; at < many; ++at) {
        const std::string& one = words[static_cast<std::size_t>(at)];

        if (one == "--list") listing = true;
        else if (one == "--file" && at + 1 < many) chosen = words[static_cast<std::size_t>(++at)];
        else if (one == "--only" && at + 1 < many) only = words[static_cast<std::size_t>(++at)];
        else {
            std::cerr << "Usage: talcite-test [--list] [--file <file>] [--only <name>]\n";
            return 2;
        }
    }

    std::vector<Trial>& held = proving::gathered();

    if (listing) {
        for (const Trial& one : held) std::cout << bared(one.file) << "  " << one.named << "\n";

        return 0;
    }

    std::size_t passed = 0;
    std::size_t ran = 0;

    // Static and made before the first trial, so it outlives what the trials leave behind: the
    // pools and loops they start are made later and destroyed first, at the end of the program,
    // with the watch still running. A hang there (a worker that never ends) is named too.
    static Watch watch{longest()};

    for (const Trial& one : held) {
        if (!only.empty() && one.named != only) continue;
        if (!chosen.empty() && bared(one.file) != chosen) continue;

        ++ran;

        watch.begun(one.named);

        try {
            one.run();
            ++passed;
            std::cout << "  ok    " << one.named << "\n";
        } catch (const proving::Failed& why) {
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
