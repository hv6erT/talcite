#include "proving.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "talcite/child.hpp"
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

    made.program = fs::path{proving::mine()};
    made.arguments.emplace_back("--pretending");

    for (std::string& one : what) made.arguments.push_back(std::move(one));

    return made;
}



}

// --- Start and end -------------------------------------------------------------------------------

TRIAL("Child: an empty child knows nothing") {
    const Child one;

    ought(!one.held());
    same(one.pid(), std::int64_t{-1});
}

TRIAL("Child: a started program gives its output and code zero") {
    const Output made = Child::run(pretending({"said", "dzień dobry"})).await();

    same(text(made.out), std::string{"dzień dobry"});
    ought(made.err.empty());
    ought(made.exit.ended == Ended::normal);
    same(made.exit.code, 0);
}

TRIAL("Child: the exit code comes back as the program gave it") {
    const Output made = Child::run(pretending({"code", "7"})).await();

    ought(made.exit.ended == Ended::normal);
    same(made.exit.code, 7);
}

// The two streams are separate until someone asks for them to be merged into one.
TRIAL("Child: output and errors go separately") {
    const Output made = Child::run(pretending({"both"})).await();

    same(text(made.out), std::string{"output"});
    same(text(made.err), std::string{"error"});
}

TRIAL("Child: merge pours errors into the output") {
    Command what = pretending({"both"});
    what.merge = true;

    const Output made = Child::run(std::move(what)).await();

    ought(made.err.empty());
    same(made.out.size(), std::string{"output"}.size() + std::string{"error"}.size());
}

// The systems report this in different ways, and both are correct. On POSIX the fork succeeds and
// exec does not; the child then exits with code 127, because there is nobody to tell about the
// failure earlier. On Windows CreateProcessW refuses at once, and the refusal comes back as a
// rejected promise. The trial guards the one thing in common: neither way gives a silent zero.
TRIAL("Child: a missing program does not pretend it worked") {
    Command what;
    what.program = fs::path{"nie-ma-takiego-programu-w-talcite"};

    try {
        const Output made = Child::run(std::move(what)).await();

        ought(made.exit.code != 0);
    } catch (const std::exception&) {
    }
}

TRIAL("Child: no program is a refusal, not silence") {
    threw((void)Child::run(Command{}).await());
}

// --- Input ---------------------------------------------------------------------------------------

TRIAL("Child: the given input comes back as output") {
    const Output made = Child::run(pretending({"echo"}), bytes("tam i z powrotem")).await();

    same(text(made.out), std::string{"tam i z powrotem"});
    same(made.exit.code, 0);
}

// A second program does not inherit the first one's pipes. Without the close-on-exec flag on our
// ends of the pipes, a program started later would hold the first one's input open, and the first
// would not see the end of its input while the second lived.
TRIAL("Child: a second program does not hold the first one's input") {
    const Child first = Child::start(pretending({"echo"})).await();
    const Child second = Child::start(pretending({"sleeping", "8000"})).await();

    const auto began = std::chrono::steady_clock::now();

    first.close_input();

    while (!first.read(4096).await().empty()) {
    }

    same(first.wait().await().code, 0);

    ought(std::chrono::steady_clock::now() - began < std::chrono::seconds{4});

    second.kill();
    (void)second.wait().await();
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
        const Child one = Child::start(pretending({"echo"})).await();

        (void)one.write(bytes("tam i z powrotem")).await();
        one.close_input();

        for (Bytes part = one.read(4096).await(); !part.empty(); part = one.read(4096).await()) got += text(part);

        (void)one.wait().await();
    } catch (...) {
    }

    ::dup2(saved, STDIN_FILENO);
    ::close(saved);

    same(got, std::string{"tam i z powrotem"});
}

#endif

#if defined(__linux__)

// A socket we opened does not reach a child program; otherwise it would keep the port taken long
// after we close it. We check directly: the child's descriptors in /proc.
TRIAL("Child: our sockets do not reach the program") {
    const net::Listener gate{net::Address::loopback(net::Family::four, 0)};
    const net::Udp post{net::Address::loopback(net::Family::four, 0)};

    const Child one = Child::start(pretending({"sleeping", "3000"})).await();

    // We count after exec, not before. Between fork and exec the child still holds all our
    // descriptors (only exec closes them, CLOEXEC), and start may hand over the child earlier.
    // Under load the trial could hit that gap and see sockets that are gone after exec. A socket
    // that really got through stays forever, so waiting does not hide it.
    std::size_t sockets = 0;

    for (int round = 0; round < 200; ++round) {
        sockets = 0;

        std::error_code ignored;

        for (const std::filesystem::directory_entry& fd :
             std::filesystem::directory_iterator{"/proc/" + std::to_string(one.pid()) + "/fd", ignored})
            if (std::filesystem::read_symlink(fd.path(), ignored).string().rfind("socket:", 0) == 0) ++sockets;

        if (sockets == 0) break;

        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }

    one.kill();
    (void)one.wait().await();

    same(sockets, std::size_t{0});
}

#endif

// Closing the input is a command: without it echo would wait forever.
TRIAL("Child: without closing the input the program waits, with it the program ends") {
    const Child one = Child::start(pretending({"echo"})).await();

    (void)one.write(bytes("raz")).await();
    one.close_input();

    Bytes whole;

    for (;;) {
        const Bytes got = one.read(4096).await();

        if (got.empty()) break;

        whole.insert(whole.end(), got.begin(), got.end());
    }

    same(text(whole), std::string{"raz"});
    same(one.wait().await().code, 0);
}

// A pipe has a bottom. A program that writes to both streams more than they hold deadlocks a reader
// that watches only one of them, and that is exactly why taken exists.
TRIAL("Child: taken does not deadlock on plentiful output") {
    const Output made = Child::run(pretending({"much"})).await();

    same(made.out.size(), std::size_t{300 * 1000});
    same(made.err.size(), std::size_t{300 * 1000});
    same(made.exit.code, 0);
}

// --- Command -------------------------------------------------------------------------------------

TRIAL("Child: arguments go separately, also with spaces") {
    const Output made = Child::run(pretending({"args", "jeden dwa", "trzy"})).await();

    same(text(made.out), std::string{"jeden dwa\ntrzy\n"});
}

TRIAL("Child: the working directory is the one given") {
    Command what = pretending({"where"});
    what.where = fs::temp();

    const Output made = Child::run(std::move(what)).await();

    // We ask the system instead of comparing strings. The same path can be written differently
    // (Windows gives the temporary directory with a trailing backslash and the current directory
    // without one), so a character-by-character comparison would speak of the spelling, not of
    // what we ask.
    ought(fs::equivalent(fs::path{text(made.out)}, fs::temp()).await());
}

TRIAL("Child: an environment variable reaches the program") {
    Command what = pretending({"around", "TALCITE_PROBA"});
    what.environment.push_back(Variable{"TALCITE_PROBA", "wartość"});

    const Output made = Child::run(std::move(what)).await();

    same(text(made.out), std::string{"wartość"});
}

// Without inheritance the program has nothing but what it was given, and that is the only way to
// run something under known conditions.
TRIAL("Child: without inheritance only what was given remains") {
    Command what = pretending({"around", "PATH"});
    what.inherit = false;
    what.environment.push_back(Variable{"TALCITE_PROBA", "wartość"});

    const Output made = Child::run(std::move(what)).await();

    same(text(made.out), std::string{"<none>"});
}

// --- Ending --------------------------------------------------------------------------------------

TRIAL("Child: kill ends a program that is still sleeping") {
    const Child one = Child::start(pretending({"sleeping", "30000"})).await();

    Promise<Exit> waiting = one.wait();

    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    one.kill();

    const Exit made = waiting.await();

    ought(made.ended != Ended::lost);

#ifndef _WIN32

    ought(made.ended == Ended::killed);
    same(made.signal, SIGKILL);

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
    const Child one = Child::start(pretending({"parent", "30000"})).await();

    std::string line;

    for (int again = 0; again < 100 && line.find('\n') == std::string::npos; ++again)
        line += text(one.read(64).await());

    const std::int64_t grandchild = std::stoll(line);

    ought(alive(grandchild, std::chrono::milliseconds{0}));

    one.kill();

    (void)one.wait().await();

    ought(!alive(grandchild, std::chrono::milliseconds{3000}));
}

// `stop()` asks the whole group to end (the parent and the grandchild), not the parent alone.
TRIAL("Child: stop also asks what the program started to end") {
    const Child one = Child::start(pretending({"parent", "30000"})).await();

    std::string line;

    for (int again = 0; again < 100 && line.find('\n') == std::string::npos; ++again)
        line += text(one.read(64).await());

    const std::int64_t grandchild = std::stoll(line);

    one.stop();

    (void)one.wait().await();

    ought(!alive(grandchild, std::chrono::milliseconds{3000}));
}

// Killing after reaping must be quiet and safe: the process id may then be given to someone else,
// so a signal sent blindly would hit a foreign program.
TRIAL("Child: kill after the end does nothing") {
    const Child one = Child::start(pretending({"code", "3"})).await();

    same(one.wait().await().code, 3);

    one.kill();
    one.stop();

    same(one.wait().await().code, 3);
}

TRIAL("Child: the end can be asked for many times and always says the same") {
    const Child one = Child::start(pretending({"code", "5"})).await();

    same(one.wait().await().code, 5);
    same(one.wait().await().code, 5);
    same(one.wait().await().code, 5);
}

// Letting go of the handle is not an order to die; the program should finish its work.
TRIAL("Child: close does not kill the program") {
    Child one = Child::start(pretending({"code", "0"})).await();

    Promise<Exit> waiting = one.wait();

    one.close();

    ought(!one.held());
    same(waiting.await().code, 0);
}

#ifndef _WIN32

// Whoever let go of the handle and never asked for the end still leaves no zombie behind: reaping
// starts at launch, not at the first question. Checkable only on POSIX, because only there an
// uncollected end stays in the process table.
TRIAL("Child: an abandoned child is reaped without being asked") {
    std::int64_t which = -1;

    {
        Child one = Child::start(pretending({"code", "0"})).await();

        which = one.pid();

        one.close();
    }

    // We ask with signal zero, not with waitpid: waitpid would clean up the zombie itself and the
    // trial would stop checking anything. A zombie still answers this question, a reaped one does
    // not.
    for (int again = 0; again < 200; ++again) {
        if (::kill(static_cast<pid_t>(which), 0) != 0 && errno == ESRCH) return;

        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }

    ought(!"a zombie was left after the child");
}

#endif

TRIAL("Child: the process id is known at once") {
    const Child one = Child::start(pretending({"code", "0"})).await();

    ought(one.held());
    ought(one.pid() > 0);

    (void)one.wait().await();
}

// `close_input()` interrupts a write that is stuck on a full pipe. Without the wake-up pipe,
// closing the descriptor does not wake a poll already waiting on it: the write would end only with
// the child's death, and a pool worker would sit there all that time.
TRIAL("Child: seal() interrupts a write stuck on a full pipe") {
    const Child one = Child::start(pretending({"sleeping", "5000"})).await();

    // More than the pipe holds, to a child that does not read at all: the write is sure to stall.
    Promise<std::size_t> wrote = one.write(bytes(std::string(1024 * 1024, 'x')));

    wrote.start();

    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    const auto began = std::chrono::steady_clock::now();

    one.close_input();

    bool told = false;

    try {
        (void)wrote.await();
    } catch (const std::exception&) {
        told = true;
    }

    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - began)
                          .count();

    // The write ends on both systems, but for different reasons, so only that is checked
    // everywhere.
    ought(told);

    // Immediacy holds on both systems. On POSIX the wake-up pipe in the poll set, next to the
    // input and the bell, wakes it; under Windows an event that the writer waits for together with
    // the completion of its own overlapped portion. The shape is the same: wait for two things at
    // once, because a write stuck on a full pipe cannot be woken any other way.
    //
    // A ceiling with a huge margin: the child lives five seconds, and the measured interruption
    // takes 0 ms.
    ought(took < 2000);

    one.kill();
}

// After sealing, a new write has nowhere to go and says so plainly instead of waiting.
TRIAL("Child: a write after seal() is a refusal") {
    const Child one = Child::start(pretending({"sleeping", "200"})).await();

    one.close_input();

    bool told = false;

    try {
        (void)one.write(bytes("raz")).await();
    } catch (const std::exception&) {
        told = true;
    }

    ought(told);

    (void)one.wait().await();
}

// Sealing twice has no effect and is not an error, just like closing something closed.
TRIAL("Child: seal() twice breaks nothing") {
    const Child one = Child::start(pretending({"echo"})).await();

    (void)one.write(bytes("raz")).await();

    one.close_input();
    one.close_input();

    Bytes whole;

    for (;;) {
        const Bytes got = one.read(4096).await();

        if (got.empty()) break;

        whole.insert(whole.end(), got.begin(), got.end());
    }

    same(text(whole), std::string{"raz"});
    same(one.wait().await().code, 0);
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

    const Output made = Child::run(what).await();

    same(text(made.out), std::string{"\"a & echo WSTRZYK\"\r\n\"b|c\"\r\n"});
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

    threw((void)Child::run(what).await());
}

// A program that lies only in the current directory does not start by its bare name: the current
// directory is the panel's directory, and anything may lie in it.
TRIAL("Child: a program in the current directory does not start by name") {
    const Planted planted{"obcy"};

    const std::filesystem::path aside = planted.where() / "osobno";

    std::filesystem::create_directories(aside);
    std::filesystem::copy_file(proving::mine(), aside / "obcy.exe");

    std::wstring was(32768, L'\0');

    was.resize(GetCurrentDirectoryW(static_cast<DWORD>(was.size()), was.data()));

    SetCurrentDirectoryW(aside.c_str());

    Command what;

    what.program = "obcy";
    what.arguments = {"--pretending", "said", "x"};

    bool refused = false;

    try {
        (void)Child::run(what).await();
    } catch (const std::exception&) {
        refused = true;
    }

    SetCurrentDirectoryW(was.c_str());

    ought(refused);
}

#endif

// --- Detached program ----------------------------------------------------------------------------

namespace {

// The contents of the file a detached program writes when it starts; we wait for it at most a
// few seconds, and an empty string means it never came.
std::string awaited_note(const std::filesystem::path& where) {
    for (int turn = 0; turn < 250; ++turn) {
        std::ifstream input{where, std::ios::binary};

        if (input) return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};

        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }

    return {};
}

}

// A detached program lives on its own: we know its id at once, and it is not our child; on POSIX
// it sits in a session other than ours and does not have us as its parent.
TRIAL("Child: detached, the program lives on its own and we know its id") {
    const std::filesystem::path where = fs::temp() / "talcite-wolny.txt";

    std::error_code why;

    std::filesystem::remove(where, why);

    const std::int64_t pid = Child::detached(pretending({"noted", where.string(), "200"})).await();

    ought(pid > 0);

    const std::string told = awaited_note(where);

    ought(!told.empty());

    const std::int64_t said = std::atoll(told.c_str());

    same(said, pid);

#ifndef _WIN32
    const std::size_t first = told.find(' ');
    const std::size_t second = told.find(' ', first + 1);

    ought(first != std::string::npos && second != std::string::npos);

    if (first == std::string::npos || second == std::string::npos) return;

    const long session = std::atol(told.c_str() + first + 1);
    const long parent = std::atol(told.c_str() + second + 1);

    ought(session != static_cast<long>(::getsid(0)));
    ought(parent != static_cast<long>(::getpid()));
#endif

    std::filesystem::remove(where, why);
}

TRIAL("Child: detached, a missing program is a refusal, not silence") {
    Command what;

    what.program = fs::temp() / "talcite-nie-ma-takiego-programu";

    threw(Child::detached(what).await());
    threw(Child::detached(Command{}).await());
}

// A working directory that does not exist is a refusal too, not a program started elsewhere.
TRIAL("Child: detached, a missing working directory is a refusal") {
    Command what = pretending({"code", "0"});

    what.where = fs::temp() / "talcite-nie-ma-takiego-katalogu";

    threw(Child::detached(what).await());
}
