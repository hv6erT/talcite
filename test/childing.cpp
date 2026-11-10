#include "tal/testing.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "talcite/child.hpp"
#include "talcite/encoding.hpp"
#include "talcite/fs.hpp"
#include "talcite/listener.hpp"
#include "talcite/udp.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace talcite;

namespace {

// All trials start the test binary, not someone else's program: --pretending tells it to pretend
// whatever is needed. Thanks to this we do not depend on whether the machine has a shell, echo or
// cmd, and the same works under Windows and under Linux.
Command pretending(std::vector<std::string> what) {
    Command made;

    made.program = talcite::fs::from_string(tal::testing::mine().text());
    made.arguments.emplace_back("--pretending");

    for (std::string& one : what) made.arguments.push_back(std::move(one));

    return made;
}


}

// --- Start and end -------------------------------------------------------------------------------

TRIAL("Child: an empty child knows nothing") {
    const Child one;

    OUGHT(!one.held());
    SAME(one.pid(), std::int64_t{-1});
}

TRIAL("Child: a started program gives its output and code zero") {
    const Output made = Child::run(pretending({"said", "dzień dobry"})).get();

    SAME(text(made.out), std::string{"dzień dobry"});
    OUGHT(made.err.empty());
    OUGHT(made.exit.ended == Ended::normal);
    SAME(made.exit.code, 0);
}

TRIAL("Child: the exit code comes back as the program gave it") {
    const Output made = Child::run(pretending({"code", "7"})).get();

    OUGHT(made.exit.ended == Ended::normal);
    SAME(made.exit.code, 7);
}

// The two streams are separate until someone asks for them to be merged into one.
TRIAL("Child: output and errors go separately") {
    const Output made = Child::run(pretending({"both"})).get();

    SAME(text(made.out), std::string{"output"});
    SAME(text(made.err), std::string{"error"});
}

TRIAL("Child: merge pours errors into the output") {
    Command what = pretending({"both"});
    what.merge = true;

    const Output made = Child::run(std::move(what)).get();

    OUGHT(made.err.empty());
    SAME(made.out.size(), std::string{"output"}.size() + std::string{"error"}.size());
}

// A program that cannot be started is a refusal on every system. On Windows CreateProcessW
// refuses at once; on POSIX the fork succeeds and exec does not, and the child tells the parent so
// through the start pipe before it ends, instead of passing for a program that ended with 127.
TRIAL("Child: a missing program is a refusal on every system") {
    Command what;
    what.program = fs::path{"nie-ma-takiego-programu-w-talcite"};

    THREW((void)Child::run(what).get());
    THREW((void)Child::start(what).get());

    what.program = fs::temp() / "talcite-nie-ma-takiego-programu";

    THREW((void)Child::start(what).get());
}

// A working directory that does not exist is a refusal as well, not a program started elsewhere.
TRIAL("Child: a missing working directory is a refusal") {
    Command what = pretending({"code", "0"});

    what.where = fs::temp() / "talcite-nie-ma-takiego-katalogu";

    THREW((void)Child::start(what).get());
}

TRIAL("Child: no program is a refusal, not silence") {
    THREW((void)Child::run(Command{}).get());
}

// --- Input ---------------------------------------------------------------------------------------

TRIAL("Child: the given input comes back as output") {
    const Output made = Child::run(pretending({"echo"}), bytes("tam i z powrotem")).get();

    SAME(text(made.out), std::string{"tam i z powrotem"});
    SAME(made.exit.code, 0);
}

// A second program does not inherit the first one's pipes. Without the close-on-exec flag on our
// ends of the pipes, a program started later would hold the first one's input open, and the first
// would not see the end of its input while the second lived.
TRIAL("Child: a second program does not hold the first one's input") {
    const Child first = Child::start(pretending({"echo"})).get();
    const Child second = Child::start(pretending({"sleeping", "8000"})).get();

    const auto began = std::chrono::steady_clock::now();

    first.close_input();

    while (!first.read(4096).get().empty()) {
    }

    SAME(first.wait().get().code, 0);

    OUGHT(std::chrono::steady_clock::now() - began < std::chrono::seconds{4});

    second.kill();
    (void)second.wait().get();
}

#ifndef _WIN32

// A process without standard input (descriptor zero closed: a daemon, a service) gets exactly
// zero from pipe2 for the child's end of the pipe. dup2 onto itself does nothing and leaves the
// close-on-exec flag, so the child would get a closed input and read nothing.
TRIAL("Child: a pipe on descriptor zero reaches the program") {
    const int saved = ::dup(STDIN_FILENO);

    ::close(STDIN_FILENO);

    std::string got;

    try {
        const Child one = Child::start(pretending({"echo"})).get();

        (void)one.write(bytes("tam i z powrotem")).get();
        one.close_input();

        for (Bytes part = one.read(4096).get(); !part.empty(); part = one.read(4096).get()) got += text(part);

        (void)one.wait().get();
    } catch (...) {
    }

    ::dup2(saved, STDIN_FILENO);
    ::close(saved);

    SAME(got, std::string{"tam i z powrotem"});
}

#endif

#if defined(__linux__)

// A socket we opened does not reach a child program; otherwise it would keep the port taken long
// after we close it. We check directly: the child's descriptors in /proc.
TRIAL("Child: our sockets do not reach the program") {
    const net::Listener gate{net::Address::loopback(net::Family::four, 0)};
    const net::Udp post{net::Address::loopback(net::Family::four, 0)};

    const Child one = Child::start(pretending({"sleeping", "3000"})).get();

    // We count after exec, not before. Between fork and exec the child still holds all our
    // descriptors (only exec closes them, CLOEXEC), and start may hand over the child earlier.
    // Under load the trial could hit that gap and see sockets that are gone after exec. A socket
    // that really got through stays forever, so waiting does not hide it.
    std::size_t sockets = 0;

    (void)tal::testing::came([&] {
        sockets = 0;

        std::error_code ignored;

        for (const std::filesystem::directory_entry& fd :
             std::filesystem::directory_iterator{"/proc/" + std::to_string(one.pid()) + "/fd", ignored})
            if (std::filesystem::read_symlink(fd.path(), ignored).string().rfind("socket:", 0) == 0) ++sockets;

        return sockets == 0;
    });

    one.kill();
    (void)one.wait().get();

    SAME(sockets, std::size_t{0});
}

#endif

// Closing the input is a command: without it echo would wait forever.
TRIAL("Child: without closing the input the program waits, with it the program ends") {
    const Child one = Child::start(pretending({"echo"})).get();

    (void)one.write(bytes("raz")).get();
    one.close_input();

    Bytes whole;

    for (;;) {
        const Bytes got = one.read(4096).get();

        if (got.empty()) break;

        whole.insert(whole.end(), got.begin(), got.end());
    }

    SAME(text(whole), std::string{"raz"});
    SAME(one.wait().get().code, 0);
}

// A pipe has a bottom. A program that writes to both streams more than they hold deadlocks a reader
// that watches only one of them, and that is exactly why taken exists.
TRIAL("Child: taken does not deadlock on plentiful output") {
    const Output made = Child::run(pretending({"much"})).get();

    SAME(made.out.size(), std::size_t{300 * 1000});
    SAME(made.err.size(), std::size_t{300 * 1000});
    SAME(made.exit.code, 0);
}

// --- Command -------------------------------------------------------------------------------------

TRIAL("Child: arguments go separately, also with spaces") {
    const Output made = Child::run(pretending({"args", "jeden dwa", "trzy"})).get();

    SAME(text(made.out), std::string{"jeden dwa\ntrzy\n"});
}

TRIAL("Child: the working directory is the one given") {
    Command what = pretending({"where"});
    what.where = fs::temp();

    const Output made = Child::run(std::move(what)).get();

    // We ask the system instead of comparing strings. The same path can be written differently
    // (Windows gives the temporary directory with a trailing backslash and the current directory
    // without one), so a character-by-character comparison would speak of the spelling, not of
    // what we ask.
    OUGHT(fs::equivalent(fs::path{text(made.out)}, fs::temp()).get());
}

TRIAL("Child: an environment variable reaches the program") {
    Command what = pretending({"around", "TALCITE_PROBA"});
    what.environment.push_back(Variable{"TALCITE_PROBA", "wartość"});

    const Output made = Child::run(std::move(what)).get();

    SAME(text(made.out), std::string{"wartość"});
}

// Without inheritance the program has nothing but what it was given, and that is the only way to
// run something under known conditions: a variable of ours does not reach it, one given does. On
// Windows it is given PATH too, since a build with AddressSanitizer loads its runtime
// (clang_rt.asan_dynamic) from there and without it would not start at all.
TRIAL("Child: without inheritance only what was given remains") {
    const auto given = [] {
        Command made = pretending({"around", "TALCITE_ODZIEDZICZONA"});

        made.inherit = false;
        made.environment.push_back(Variable{"TALCITE_PROBA", "wartość"});

#ifdef _WIN32
        std::wstring path(32767, L'\0');

        path.resize(GetEnvironmentVariableW(L"PATH", path.data(), static_cast<DWORD>(path.size())));

        made.environment.push_back(Variable{"PATH", to_string(path)});
#endif

        return made;
    };

#ifdef _WIN32
    SetEnvironmentVariableW(L"TALCITE_ODZIEDZICZONA", L"nasza");
#else
    ::setenv("TALCITE_ODZIEDZICZONA", "nasza", 1);
#endif

    const Output ours = Child::run(given()).get();

    Command probed = given();

    probed.arguments.back() = "TALCITE_PROBA";

    const Output theirs = Child::run(std::move(probed)).get();

#ifdef _WIN32
    SetEnvironmentVariableW(L"TALCITE_ODZIEDZICZONA", nullptr);
#else
    ::unsetenv("TALCITE_ODZIEDZICZONA");
#endif

    SAME(text(ours.out), std::string{"<none>"});
    SAME(text(theirs.out), std::string{"wartość"});
}

// --- Ending --------------------------------------------------------------------------------------

TRIAL("Child: kill ends a program that is still sleeping") {
    const Child one = Child::start(pretending({"sleeping", "30000"})).get();

    Promise<Exit> waiting = one.wait();

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    one.kill();

    const Exit made = std::move(waiting).get();

    OUGHT(made.ended != Ended::lost);

#ifndef _WIN32

    OUGHT(made.ended == Ended::killed);
    SAME(made.signal, SIGKILL);

#endif
}

namespace {

// Whether the process with this id is still alive, waiting for its end at most `patience`. A
// zombie counts as ended: it executes nothing any more, and only its parent can reap it.
bool alive(std::int64_t which, std::chrono::milliseconds patience) {
#ifdef _WIN32
    const HANDLE one = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(which));

    if (one == nullptr) return false;

    const bool still = WaitForSingleObject(one, static_cast<DWORD>(patience.count())) == WAIT_TIMEOUT;

    CloseHandle(one);

    return still;
#else
    const auto end = std::chrono::steady_clock::now() + patience;

    for (;;) {
        std::ifstream reading{"/proc/" + std::to_string(which) + "/stat"};

        std::string line;

        if (!std::getline(reading, line)) {
            if (::kill(static_cast<pid_t>(which), 0) != 0) return false;
        } else {
            const std::size_t closing = line.rfind(')');

            if (closing != std::string::npos && closing + 2 < line.size() && line[closing + 2] == 'Z') return false;
        }

        if (std::chrono::steady_clock::now() >= end) return true;

        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
#endif
}

}

// `kill()` ends the whole tree: the program and what it started. The parent here starts the
// grandchild the ordinary system way, just as a shell or a batch file starts the actual program;
// the grandchild must not survive the parent's killing and sleep on.
TRIAL("Child: kill also ends what the program started") {
    const Child one = Child::start(pretending({"parent", "30000"})).get();

    std::string line;

    for (int again = 0; again < 100 && line.find('\n') == std::string::npos; ++again)
        line += text(one.read(64).get());

    const std::int64_t grandchild = std::stoll(line);

    OUGHT(alive(grandchild, std::chrono::milliseconds{0}));

    one.kill();

    (void)one.wait().get();

    OUGHT(!alive(grandchild, std::chrono::milliseconds{3000}));
}

// `stop()` asks the whole group to end (the parent and the grandchild), not the parent alone.
TRIAL("Child: stop also asks what the program started to end") {
    const Child one = Child::start(pretending({"parent", "30000"})).get();

    std::string line;

    for (int again = 0; again < 100 && line.find('\n') == std::string::npos; ++again)
        line += text(one.read(64).get());

    const std::int64_t grandchild = std::stoll(line);

    const auto began = std::chrono::steady_clock::now();

    one.stop();

    const Exit ended = one.wait().get();

    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);

    // On Windows the grandchild has outlived the Ctrl-Break more than once: how the parent ended
    // and how soon tell whether the event reached the group at all (a parent asleep for its full
    // thirty seconds did not get it either) or the parent alone.
    if (alive(grandchild, std::chrono::milliseconds{3000})) {
        SAME("parent: code " + std::to_string(static_cast<unsigned>(ended.code)) + ", signal " + std::to_string(ended.signal) + ", after " +
                 std::to_string(took.count()) + " ms",
             std::string{"the grandchild ends with it"});

#ifdef _WIN32
        if (const HANDLE left = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(grandchild)); left != nullptr) {
            (void)TerminateProcess(left, 1);
            CloseHandle(left);
        }
#endif
    }
}

// Killing after reaping must be quiet and safe: the process id may then be given to someone else,
// so a signal sent blindly would hit a foreign program.
TRIAL("Child: kill after the end does nothing") {
    const Child one = Child::start(pretending({"code", "3"})).get();

    SAME(one.wait().get().code, 3);

    one.kill();
    one.stop();

    SAME(one.wait().get().code, 3);
}

TRIAL("Child: the end can be asked for many times and always says the same") {
    const Child one = Child::start(pretending({"code", "5"})).get();

    SAME(one.wait().get().code, 5);
    SAME(one.wait().get().code, 5);
    SAME(one.wait().get().code, 5);
}

// Letting go of the handle is not an order to die; the program should finish its work.
TRIAL("Child: close does not kill the program") {
    Child one = Child::start(pretending({"code", "0"})).get();

    Promise<Exit> waiting = one.wait();

    one.close();

    OUGHT(!one.held());
    SAME(std::move(waiting).get().code, 0);
}

#ifndef _WIN32

// Whoever let go of the handle and never asked for the end still leaves no zombie behind: reaping
// starts at launch, not at the first question. Checkable only on POSIX, because only there an
// uncollected end stays in the process table.
TRIAL("Child: an abandoned child is reaped without being asked") {
    std::int64_t which = -1;

    {
        Child one = Child::start(pretending({"code", "0"})).get();

        which = one.pid();

        one.close();
    }

    // We ask with signal zero, not with waitpid: waitpid would clean up the zombie itself and the
    // trial would stop checking anything. A zombie still answers this question, a reaped one does
    // not.
    SOON(::kill(static_cast<pid_t>(which), 0) != 0 && errno == ESRCH);
}

#endif

TRIAL("Child: the process id is known at once") {
    const Child one = Child::start(pretending({"code", "0"})).get();

    OUGHT(one.held());
    OUGHT(one.pid() > 0);

    (void)one.wait().get();
}

// `close_input()` interrupts a write that is stuck on a full pipe. Without the wake-up pipe,
// closing the descriptor does not wake a poll already waiting on it: the write would end only with
// the child's death, and a pool worker would sit there all that time.
TRIAL("Child: seal() interrupts a write stuck on a full pipe") {
    const Child one = Child::start(pretending({"sleeping", "5000"})).get();

    // More than the pipe holds, to a child that does not read at all: the write is sure to stall.
    Promise<std::size_t> wrote = one.write(bytes(std::string(1024 * 1024, 'x')));

    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    const auto began = std::chrono::steady_clock::now();

    one.close_input();

    bool told = false;

    try {
        (void)std::move(wrote).get();
    } catch (const std::exception&) {
        told = true;
    }

    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - began)
                          .count();

    // The write ends on both systems, but for different reasons, so only that is checked
    // everywhere.
    OUGHT(told);

    // Immediacy holds on both systems. On POSIX the wake-up pipe in the poll set, next to the
    // input and the bell, wakes it; under Windows an event that the writer waits for together with
    // the completion of its own overlapped portion. The shape is the same: wait for two things at
    // once, because a write stuck on a full pipe cannot be woken any other way.
    //
    // A ceiling with a huge margin: the child lives five seconds, and the measured interruption
    // takes 0 ms.
    OUGHT(took < 2000);

    one.kill();
}

// After sealing, a new write has nowhere to go and says so plainly instead of waiting.
TRIAL("Child: a write after seal() is a refusal") {
    const Child one = Child::start(pretending({"sleeping", "200"})).get();

    one.close_input();

    bool told = false;

    try {
        (void)one.write(bytes("raz")).get();
    } catch (const std::exception&) {
        told = true;
    }

    OUGHT(told);

    (void)one.wait().get();
}

// Sealing twice has no effect and is not an error, just like closing something closed.
TRIAL("Child: seal() twice breaks nothing") {
    const Child one = Child::start(pretending({"echo"})).get();

    (void)one.write(bytes("raz")).get();

    one.close_input();
    one.close_input();

    Bytes whole;

    for (;;) {
        const Bytes got = one.read(4096).get();

        if (got.empty()) break;

        whole.insert(whole.end(), got.begin(), got.end());
    }

    SAME(text(whole), std::string{"raz"});
    SAME(one.wait().get().code, 0);
}

#ifdef _WIN32

namespace {

// A directory for the duration of a trial and PATH with it in front; both go back as they were.
class Planted {
public:
    // Creates an empty directory talcite-<name> in the temporary directory and puts it at the
    // front of PATH.
    explicit Planted(const std::string& name) {
        where_ = std::filesystem::temp_directory_path() / std::filesystem::path{"talcite-" + name};

        std::error_code ignored;

        std::filesystem::remove_all(where_, ignored);
        std::filesystem::create_directories(where_, ignored);

        std::wstring room(32767, L'\0');

        const DWORD much = GetEnvironmentVariableW(L"PATH", room.data(), static_cast<DWORD>(room.size()));

        room.resize(much < room.size() ? much : 0);

        path_ = room;

        SetEnvironmentVariableW(L"PATH", (where_.wstring() + L";" + path_).c_str());
    }

    // Restores PATH and removes the directory.
    ~Planted() {
        SetEnvironmentVariableW(L"PATH", path_.c_str());

        std::error_code ignored;

        std::filesystem::remove_all(where_, ignored);
    }

    Planted(const Planted&) = delete;
    Planted& operator=(const Planted&) = delete;

    // The planted directory.
    [[nodiscard]] const std::filesystem::path& where() const { return where_; }

private:
    std::filesystem::path where_;
    std::wstring path_;
};

}

// A batch file by its bare name (PATHEXT adds .cmd), with arguments that cmd.exe would take for
// commands: & and | arrive literally, in quotes, and the second echo does not run.
TRIAL("Child: a batch file run by name gets its arguments literally") {
    const Planted planted{"wsad"};

    {
        std::ofstream making{planted.where() / "pomoc.cmd", std::ios::binary};
        making << "@echo off\r\necho %1\r\necho %2\r\n";
    }

    Command what;

    what.program = "pomoc";
    what.arguments = {"a & echo WSTRZYK", "b|c"};

    const Output made = Child::run(what).get();

    SAME(text(made.out), std::string{"\"a & echo WSTRZYK\"\r\n\"b|c\"\r\n"});
}

// Quotes do not protect a percent sign from cmd.exe: %PATH% would expand inside the argument.
TRIAL("Child: an argument with a percent sign does not go to a batch file") {
    const Planted planted{"procent"};

    {
        std::ofstream making{planted.where() / "pomoc.bat", std::ios::binary};
        making << "@echo off\r\necho %1\r\n";
    }

    Command what;

    what.program = "pomoc";
    what.arguments = {"%PATH%"};

    THREW((void)Child::run(what).get());
}

// A program that lies only in the current directory does not start by its bare name: the current
// directory is the panel's directory, and anything may lie in it.
TRIAL("Child: a program in the current directory does not start by name") {
    const Planted planted{"obcy"};

    const std::filesystem::path aside = planted.where() / "osobno";

    std::filesystem::create_directories(aside);
    std::filesystem::copy_file(talcite::fs::from_string(tal::testing::mine().text()), aside / "obcy.exe");

    std::wstring was(32768, L'\0');

    was.resize(GetCurrentDirectoryW(static_cast<DWORD>(was.size()), was.data()));

    SetCurrentDirectoryW(aside.c_str());

    Command what;

    what.program = "obcy";
    what.arguments = {"--pretending", "said", "x"};

    bool refused = false;

    try {
        (void)Child::run(what).get();
    } catch (const std::exception&) {
        refused = true;
    }

    SetCurrentDirectoryW(was.c_str());

    OUGHT(refused);
}

#endif

// --- Detached program ----------------------------------------------------------------------------

namespace {

// The contents of the file a detached program writes when it starts; we wait for it at most ten
// seconds, and an empty string means it never came.
std::string awaited_note(const std::filesystem::path& where) {
    std::string made;

    (void)tal::testing::came([&] {
        std::ifstream input{where, std::ios::binary};

        if (!input) return false;

        made.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});

        return true;
    });

    return made;
}

}

// A detached program lives on its own: we know its id at once, and it is not our child; on POSIX
// it sits in a session other than ours and does not have us as its parent.
TRIAL("Child: detached, the program lives on its own and we know its id") {
    const std::filesystem::path where = fs::temp() / "talcite-wolny.txt";

    std::error_code why;

    std::filesystem::remove(where, why);

    const std::int64_t pid = Child::detached(pretending({"noted", where.string(), "200"})).get();

    OUGHT(pid > 0);

    const std::string told = awaited_note(where);

    OUGHT(!told.empty());

    const std::int64_t said = std::atoll(told.c_str());

    SAME(said, pid);

#ifndef _WIN32
    const std::size_t first = told.find(' ');
    const std::size_t second = told.find(' ', first + 1);

    OUGHT(first != std::string::npos && second != std::string::npos);

    if (first == std::string::npos || second == std::string::npos) return;

    const long session = std::atol(told.c_str() + first + 1);
    const long parent = std::atol(told.c_str() + second + 1);

    OUGHT(session != static_cast<long>(::getsid(0)));
    OUGHT(parent != static_cast<long>(::getpid()));
#endif

    std::filesystem::remove(where, why);
}

TRIAL("Child: detached, a missing program is a refusal, not silence") {
    Command what;

    what.program = fs::temp() / "talcite-nie-ma-takiego-programu";

    THREW(Child::detached(what).get());
    THREW(Child::detached(Command{}).get());
}

// A working directory that does not exist is a refusal too, not a program started elsewhere.
TRIAL("Child: detached, a missing working directory is a refusal") {
    Command what = pretending({"code", "0"});

    what.where = fs::temp() / "talcite-nie-ma-takiego-katalogu";

    THREW(Child::detached(what).get());
}

// --- Waiting and running to the end --------------------------------------------------------------

// Every caller of `wait` gets a view of one shared wait. A caller who gives up (here a timeout)
// lets go of its own view only; the next caller still gets the real end.
TRIAL("Child: a caller who gives up waiting does not take the end from others") {
    const Child one = Child::start(pretending({"sleeping", "300"})).get();

    THREW((void)one.wait().timeout(std::chrono::milliseconds{20}).get());

    const Exit made = one.wait().get();

    OUGHT(made.ended == Ended::normal);
    SAME(made.code, 0);
}

// A program may stop reading before it has taken its whole input. For `run` that is the end of the
// input, not a failure: the output and the code still come. Four megabytes are far more than a
// pipe holds, so the write is still going when the program ends.
TRIAL("Child: run takes a program that stops reading as the end of its input") {
    const Output made = Child::run(pretending({"said", "dosyć"}), Bytes(4 << 20, std::byte{'x'})).get();

    SAME(text(made.out), std::string{"dosyć"});
    OUGHT(made.exit.ended == Ended::normal);
    SAME(made.exit.code, 0);
}

// A run given up on the way kills its program, which would otherwise go on with nobody to read
// it. The program notes its number in a file, so the trial can see whether it lives.
TRIAL("Child: cancelling run kills the program") {
    const std::filesystem::path where = fs::temp() / "talcite-przerwany.txt";

    std::error_code why;

    std::filesystem::remove(where, why);

    Promise<Output> running = Child::run(pretending({"noted", where.string(), "30000"}));

    const std::string told = awaited_note(where);

    OUGHT(!told.empty());

    const std::int64_t which = std::atoll(told.c_str());

    OUGHT(which > 0);

    running.cancel();

    OUGHT(!alive(which, std::chrono::milliseconds{5000}));

    std::filesystem::remove(where, why);
}
