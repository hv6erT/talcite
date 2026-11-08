#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <system_error>
#include <thread>

#include "tal/testing.hpp"
#include "tal/text.hpp"

#ifdef _WIN32

#include <io.h>
#include <windows.h>

#else

#include <sys/ioctl.h>
#include <unistd.h>

#endif

// The programs a trial binary pretends to be (`--pretending <name> ...`), shared by every suite.
// The helpers live in the trials, not in the libraries: the binary starts itself, so the trials of
// starting programs do not depend on whether the machine happens to have a shell, `echo` or cmd.

namespace {

using Given = std::span<const std::string>;

// The first argument, or nothing.
std::string first(Given given) { return given.empty() ? std::string{} : given[0]; }

// The first argument as milliseconds, zero without it.
std::chrono::milliseconds waited_for(Given given) {
    return std::chrono::milliseconds{given.empty() ? 0 : std::atoi(given[0].c_str())};
}

// Writes the text to the stream as it is and flushes it at once.
void said(std::FILE* into, const std::string& what) {
    if (!what.empty()) std::fwrite(what.data(), 1, what.size(), into);

    std::fflush(into);
}

// The value of an environment variable in UTF-8, or "<none>" when it is not set.
std::string around(const std::string& name) {
#ifdef _WIN32
    std::wstring room(32767, L'\0');

    const DWORD much = GetEnvironmentVariableW(tal::text::wide(name).c_str(), room.data(), static_cast<DWORD>(room.size()));

    if (much == 0) return "<none>";

    room.resize(much);

    return tal::text::narrow(room);
#else
    const char* told = std::getenv(name.c_str());

    return told == nullptr ? std::string{"<none>"} : std::string{told};
#endif
}

// The current folder in UTF-8.
std::string here() {
#ifdef _WIN32
    return tal::text::narrow(std::filesystem::current_path().wstring());
#else
    return std::filesystem::current_path().string();
#endif
}

#ifdef _WIN32

// Takes every console event as handled, so that a Ctrl-Break does not end the program.
BOOL WINAPI deaf_to_breaks(DWORD) { return TRUE; }

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

    line = tal::text::narrow(wide);

    return true;
}

#else

// A line of the input without its line end; false at the end of the input.
bool typed_line(std::string& line) { return static_cast<bool>(std::getline(std::cin, line)); }

#endif

// Writes the first argument to the output.
int spoken(Given given) {
    said(stdout, first(given));

    return 0;
}

// Writes the first argument to the errors.
int moaned(Given given) {
    said(stderr, first(given));

    return 0;
}

// Writes to both streams.
int both(Given) {
    said(stdout, "output");
    said(stderr, "error");

    return 0;
}

// Copies the input to the output until the input ends.
int echoed(Given) {
    char room[4096];

    for (;;) {
        const std::size_t got = std::fread(room, 1, sizeof(room), stdin);

        if (got == 0) break;

        std::fwrite(room, 1, got, stdout);
    }

    std::fflush(stdout);

    return 0;
}

// More than a pipe holds, on both streams in turn. A reader that forgot to read both at once would
// get stuck here and wait forever.
int much(Given) {
    const std::string one(1000, 'x');

    for (int at = 0; at < 300; ++at) {
        said(stdout, one);
        said(stderr, one);
    }

    return 0;
}

// Ends with the code given as the first argument.
int coded(Given given) { return given.empty() ? 0 : std::atoi(given[0].c_str()); }

// Sleeps as many milliseconds as given.
int sleeping(Given given) {
    std::this_thread::sleep_for(waited_for(given));

    return 0;
}

// A program that will not be asked to end: it ignores SIGTERM (Ctrl-Break on Windows) and sleeps
// on, so that only killing ends it.
int stubborn(Given given) {
#ifdef _WIN32
    SetConsoleCtrlHandler(&deaf_to_breaks, TRUE);
#else
    (void)std::signal(SIGTERM, SIG_IGN);
#endif

    said(stdout, "ready\n");

    std::this_thread::sleep_for(waited_for(given));

    return 0;
}

// A parent with a grandchild: it starts itself as `sleeping` without any library, by a plain
// fork/exec or CreateProcessW, with no group of its own and no job, prints the grandchild's number
// and sleeps as long. That is what a shell or a batch file that started the real program looks
// like: killing the parent alone would leave the grandchild alive.
//
// On Windows the grandchild is `noted`, and the parent prints its number only once it has written
// it: a process still loading its libraries does not take a console event yet, and a Ctrl-Break
// sent a few milliseconds after its start reached the parent alone (verify.py 30.10: the parent
// ended with 0xC000013A after 2 to 10 ms, the grandchild slept on).
int parent(Given given) {
    const std::string rest = first(given);

#ifdef _WIN32
    const std::filesystem::path note =
        std::filesystem::temp_directory_path() / ("talcite-parent-" + std::to_string(GetCurrentProcessId()));

    std::error_code gone;

    std::filesystem::remove(note, gone);

    std::wstring line = L"\"" + tal::text::wide(tal::testing::mine().text()) + L"\" --pretending noted \"" + note.wstring() +
                        L"\" " + tal::text::wide(rest);

    STARTUPINFOW setting{};
    setting.cb = sizeof setting;

    PROCESS_INFORMATION told{};

    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &setting, &told) == 0) return 3;

    CloseHandle(told.hThread);
    CloseHandle(told.hProcess);

    for (int tries = 0; tries < 1000 && !std::filesystem::exists(note, gone); ++tries)
        std::this_thread::sleep_for(std::chrono::milliseconds{10});

    std::filesystem::remove(note, gone);

    said(stdout, std::to_string(told.dwProcessId) + "\n");
#else
    const std::string self{tal::testing::mine().stored()};
    const pid_t which = ::fork();

    if (which < 0) return 3;

    if (which == 0) {
        ::execl(self.c_str(), self.c_str(), "--pretending", "sleeping", rest.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }

    said(stdout, std::to_string(which) + "\n");
#endif

    std::this_thread::sleep_for(waited_for(given));

    return 0;
}

// A program let go has no pipes, so it speaks through the file named first: its number, and on
// POSIX also its session and parent. The file is written under another name and renamed in one
// piece, so that the reader never sees half of it. Then it sleeps as long as the second argument.
int noted(Given given) {
#ifdef _WIN32
    const std::string made = std::to_string(GetCurrentProcessId());
#else
    const std::string made = std::to_string(::getpid()) + " " + std::to_string(::getsid(0)) + " " + std::to_string(::getppid());
#endif
    const std::filesystem::path where{first(given)};

    std::filesystem::path part = where;

    part += ".part";

    {
        std::ofstream out{part, std::ios::binary};

        out << made;
    }

    std::error_code why;

    std::filesystem::rename(part, where, why);

    if (given.size() > 1) std::this_thread::sleep_for(std::chrono::milliseconds{std::atoi(given[1].c_str())});

    return why ? 4 : 0;
}

// The size of the terminal as columns x rows: the window's on Windows, the terminal's on POSIX.
std::string measured() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO told{};

    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &told) == 0) return "?";

    return std::to_string(told.srWindow.Right - told.srWindow.Left + 1) + "x" +
           std::to_string(told.srWindow.Bottom - told.srWindow.Top + 1);
#else
    winsize told{};

    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &told) != 0) return "?";

    return std::to_string(told.ws_col) + "x" + std::to_string(told.ws_row);
#endif
}

// A program in a terminal: it says whether its input and output are a terminal and how large that
// is, then answers line by line: `size` gives the size again, `bar` asks for a blinking bar for a
// cursor and `usual` for the terminal's usual one, `quit` ends it, anything else comes back after
// "got:".
int tty(Given) {
#ifdef _WIN32
    const bool typed = _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;

    SetConsoleOutputCP(CP_UTF8);
#else
    const bool typed = ::isatty(STDIN_FILENO) != 0 && ::isatty(STDOUT_FILENO) != 0;
#endif

    said(stdout, std::string{"tty "} + (typed ? "1 " : "0 ") + measured() + "\n");

    std::string line;

    while (typed_line(line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line == "quit") return 0;

        if (line == "size")
            said(stdout, "size " + measured() + "\n");
        else if (line == "bar")
            said(stdout, "\x1b[5 qbarred\n");
        else if (line == "usual")
            said(stdout, "\x1b[0 qas usual\n");
        else
            said(stdout, "got:" + line + "\n");
    }

    return 0;
}

// A shell's prompt as a shell integrated with its terminal gives it: two lines of output, the
// folder (OSC 7) and the marks of the prompt (OSC 133), then the prompt itself. It waits for a line
// and ends at `quit`.
int prompted(Given) {
    said(stdout, "one\ntwo\n\x1b]7;file://here/tmp/prompted\x07\x1b]133;A\x07$ \x1b]133;B\x07");

    std::string line;

    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line == "quit") return 0;
    }

    return 0;
}

// A program that says much and goes at once: three thousand numbered lines, more than one read of
// the terminal takes, then the mark of a finished command (OSC 133 D) and "bye". Its exit is known
// while the rest of what it said still waits to be read.
int farewell(Given) {
    std::string made;

    for (int at = 0; at < 3000; ++at) made += "line " + std::to_string(at) + " of what goes before the end\n";

    made += "\x1b]133;D;0\x07" "bye\n";

    said(stdout, made);

    return 0;
}

// A program that leaves another one holding its terminal: on POSIX a child that sleeps three
// seconds with the terminal for its output. It says "left" and goes.
int forsaking(Given) {
#ifndef _WIN32
    if (::fork() == 0) {
        (void)::sleep(3);

        ::_exit(0);
    }
#endif

    said(stdout, "left\n");

    return 0;
}

// Writes the current folder.
int where(Given) {
    said(stdout, here());

    return 0;
}

// Writes the value of the environment variable named first.
int surrounded(Given given) {
    said(stdout, given.empty() ? std::string{"<none>"} : around(given[0]));

    return 0;
}

// Writes every argument on a line of its own.
int args(Given given) {
    std::string made;

    for (const std::string& one : given) {
        made += one;
        made += '\n';
    }

    said(stdout, made);

    return 0;
}

// Every program above, registered under the name a trial gives after `--pretending`.
const tal::testing::Pretending pretended[] = {
    {"said", &spoken},       {"moaned", &moaned},     {"both", &both},         {"echo", &echoed},
    {"much", &much},         {"code", &coded},        {"sleeping", &sleeping}, {"stubborn", &stubborn},
    {"parent", &parent},     {"noted", &noted},       {"tty", &tty},           {"prompted", &prompted},
    {"farewell", &farewell}, {"forsaking", &forsaking}, {"where", &where},     {"around", &surrounded},
    {"args", &args},
};

}
