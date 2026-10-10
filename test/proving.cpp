#include "proving.hpp"

#include "talcite/host.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>

#else
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
#ifdef _WIN32

    // Without this, Windows would turn every `\n` into `\r\n`, and comparing bytes would mean
    // nothing.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);

#endif

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
    if (what == "parent") {
#ifdef _WIN32
        std::wstring line = L"\"" + widely(proving::mine()) + L"\" --pretending sleeping " + widely(rest);

        STARTUPINFOW setting{};
        setting.cb = sizeof setting;

        PROCESS_INFORMATION told{};

        if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &setting, &told) == 0)
            return 3;

        CloseHandle(told.hThread);
        CloseHandle(told.hProcess);

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

    for (const Trial& one : held) {
        if (!only.empty() && one.named != only) continue;
        if (!chosen.empty() && bared(one.file) != chosen) continue;

        ++ran;

        try {
            one.run();
            ++passed;
            std::cout << "  ok    " << one.named << "\n";
        } catch (const proving::Failed& why) {
            std::cout << "  FAIL  " << one.named << "\n        " << why.what() << "\n";
        } catch (const std::exception& why) {
            std::cout << "  FAIL  " << one.named << "\n        threw: " << why.what() << "\n";
        }
    }

    std::cout << "\nPassed " << passed << " of " << ran << ".\n";

    return passed == ran ? 0 : 1;
}
