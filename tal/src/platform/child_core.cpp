#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "tal/process.hpp"
#include "tal/text.hpp"

#include "core.hpp"
#include "offload.hpp"

#ifdef _WIN32

#include <windows.h>

#include <shellapi.h>

#include <cwchar>
#include <cwctype>

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif

#else

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char** environ;

#endif

// The processes of the system, ported from talcite's child.cpp: three pipes or a pseudoterminal
// (a pseudoconsole on Windows), a program found as the shell finds it, its environment, its group
// (a job on Windows) for stopping and killing it with all it started, a burial that leaves no
// zombie, and a program let loose that is nobody's child. The adapter of the port is at the end.

namespace tal::platform {

#ifndef _WIN32


namespace sealed {

// Descriptors that are closed on exec of a child program from the moment they are created.
// Setting `FD_CLOEXEC` with a separate `fcntl` leaves a gap: another thread that happens to be
// starting a program (`Child` runs on the pool) inherits the descriptor from before the flag is
// set. For a child's pipe this means a second, parallel program also holds its end, and the first
// does not see the end of its input while the second lives. Linux and the BSDs take the flag in
// the call itself; macOS has neither `pipe2` nor `SOCK_CLOEXEC`, so there the separate `fcntl`
// remains.

// Creates a pipe whose both ends are close-on-exec, and non-blocking when `quiet`; false on
// failure.
inline bool piped(int both[2], bool quiet) {
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    return ::pipe2(both, O_CLOEXEC | (quiet ? O_NONBLOCK : 0)) == 0;
#else
    if (::pipe(both) != 0) return false;

    for (int at = 0; at < 2; ++at) {
        ::fcntl(both[at], F_SETFD, ::fcntl(both[at], F_GETFD, 0) | FD_CLOEXEC);

        if (quiet) ::fcntl(both[at], F_SETFL, ::fcntl(both[at], F_GETFL, 0) | O_NONBLOCK);
    }

    return true;
#endif
}

// Creates a non-blocking, close-on-exec socket; returns the descriptor, or a negative value on
// failure.
inline int socketed(int family, int type) {
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
    return ::socket(family, type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
#else
    const int made = ::socket(family, type, 0);

    if (made < 0) return made;

    ::fcntl(made, F_SETFL, ::fcntl(made, F_GETFL, 0) | O_NONBLOCK);
    ::fcntl(made, F_SETFD, ::fcntl(made, F_GETFD, 0) | FD_CLOEXEC);

    return made;
#endif
}

// Accepts a connection as a non-blocking, close-on-exec descriptor; returns it, or a negative
// value on failure.
inline int accepted(int one, sockaddr* told, socklen_t* much) {
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK) && !defined(__APPLE__)
    return ::accept4(one, told, much, SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
    const int made = ::accept(one, told, much);

    if (made < 0) return made;

    ::fcntl(made, F_SETFD, ::fcntl(made, F_GETFD, 0) | FD_CLOEXEC);
    ::fcntl(made, F_SETFL, ::fcntl(made, F_GETFL, 0) | O_NONBLOCK);

    return made;
#endif
}

}

#endif

namespace {

// The size of a terminal: columns and rows.
struct Size {
    int wide = 80;
    int high = 24;
};

// A program to start as the core takes it: the port's command with the folder and the merge of
// errors the core knows by its own names, and the environment always inherited.
struct Command {
    path program;
    std::vector<std::string> arguments;
    path where;
    std::vector<Variable> environment;
    bool inherit = true;
    bool merge = false;
    std::uint64_t memory = 0;
};

// How a program ended, once it has: any number of tasks may wait for it, and the burial tells
// them all at once.
class Ending {
public:
    // The program has ended; whoever waits is woken.
    void settled(Exit exit) {
        std::vector<Pending<Exit>> woken;

        {
            const std::lock_guard<std::mutex> kept{guard_};

            if (done_) return;

            done_ = exit;
            woken.swap(waiting_);
        }

        for (Pending<Exit>& one : woken) one.settle(exit);
    }

    // How the program ended, once it has.
    static Task<Exit> awaited(std::shared_ptr<Ending> self) {
        Pending<Exit> waiter;

        {
            const std::lock_guard<std::mutex> kept{self->guard_};

            if (self->done_) co_return *self->done_;

            self->waiting_.push_back(waiter);
        }

        co_return co_await waiter.task();
    }

private:
    std::mutex guard_;
    std::optional<Exit> done_;
    std::vector<Pending<Exit>> waiting_;
};

}

namespace {

// The read chunk. It is the same one `fs` uses on a network share: a pipe has no size one could ask
// about, so the only thing we can choose is how often to come back for the answer to "am I still
// needed".
constexpr std::size_t brim = 64 * 1024;

// Throws a trouble about the program of the kind of the error the system reported last.
[[noreturn]] void amiss(const std::string& said, const path& where) { core::amiss(said, where); }

#ifdef _WIN32

// The attribute that hands a pseudoconsole to a new process. Older SDK headers, MinGW's among them,
// may not know it; the value is fixed by the system.
constexpr DWORD_PTR given_console = 0x00020016;

// The pseudoconsole functions of the system: Windows 10 1809 and later have them, older systems do
// not, so they are looked up at run time and a program built here still starts there. The
// pseudoconsole's handle is a plain pointer, written here as such, because headers without ConPTY
// do not know its name.
struct Consoles {
    HRESULT(WINAPI* open)(COORD, HANDLE, HANDLE, DWORD, void**) = nullptr;
    HRESULT(WINAPI* resize)(void*, COORD) = nullptr;
    void(WINAPI* close)(void*) = nullptr;
};

// Looks the three functions up in kernel32 once; all three or none.
const Consoles& consoles() {
    static const Consoles found = [] {
        Consoles made;

        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");

        if (kernel == nullptr) return made;

        // Through `void*`: a direct cast between unrelated function pointer types is what MinGW
        // warns about, and the address the system gives is such a type.
        made.open = reinterpret_cast<decltype(made.open)>(reinterpret_cast<void*>(GetProcAddress(kernel, "CreatePseudoConsole")));
        made.resize = reinterpret_cast<decltype(made.resize)>(reinterpret_cast<void*>(GetProcAddress(kernel, "ResizePseudoConsole")));
        made.close = reinterpret_cast<decltype(made.close)>(reinterpret_cast<void*>(GetProcAddress(kernel, "ClosePseudoConsole")));

        if (made.open == nullptr || made.resize == nullptr || made.close == nullptr) made = Consoles{};

        return made;
    }();

    return found;
}

// The size of a pseudoconsole, within what `COORD` holds and at least one cell each way.
COORD measured(Size size) {
    return COORD{static_cast<SHORT>(std::clamp(size.wide, 1, 32767)), static_cast<SHORT>(std::clamp(size.high, 1, 32767))};
}

#else

// Opens the master side of a new pseudoterminal, closed on exec from birth where the system takes
// the flag in the call (Linux and the BSDs), otherwise right after, as `sealed::piped` does; -1 on
// failure.
int mastered() {
#if defined(__linux__) || defined(__FreeBSD__)
    return ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
#else
    const int made = ::posix_openpt(O_RDWR | O_NOCTTY);

    if (made >= 0) ::fcntl(made, F_SETFD, ::fcntl(made, F_GETFD, 0) | FD_CLOEXEC);

    return made;
#endif
}

// Opens the slave side of the pseudoterminal on `master`, without making it our controlling
// terminal; -1 on failure. `ptsname` answers in a buffer of its own that the next call overwrites,
// so the calls take turns.
int slaved(int master) {
    static std::mutex naming;

    const std::lock_guard<std::mutex> kept{naming};

    if (::grantpt(master) != 0 || ::unlockpt(master) != 0) return -1;

    const char* name = ::ptsname(master);

    if (name == nullptr) return -1;

    return ::open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
}

// The size of a pseudoterminal, within what `winsize` holds and at least one cell each way.
winsize measured(Size size) {
    winsize made{};

    made.ws_col = static_cast<unsigned short>(std::clamp(size.wide, 1, 65535));
    made.ws_row = static_cast<unsigned short>(std::clamp(size.high, 1, 65535));

    return made;
}

#endif

}

// The grave. Everything needed to bury the program and not to kill it a second time after burial,
// kept apart from the rest, because it outlives `Child`: a task waiting for the end may run to
// completion even when the last handle was let go earlier.
//
// It has to be separate for one more reason: the promise from `wait()` lives in `Held`, and its
// closure has to hold what it waits for. Holding `Held`, it would hold itself, and nothing would
// ever be freed.
struct Grave {
    std::mutex guard;
    bool gone = false;
    std::int64_t which = -1;

    // Whether the program stands in its own group. On Windows the polite ending goes by a console
    // event, and that event hits the whole group, so sent to a program that sits in ours it would
    // kill us as well.
    bool grouped = false;

    // Whether the program got our terminal. Only such a program is resumed after a stop; a program
    // on pipes stopped by hand from outside should stay stopped.
    bool foreground = false;

#ifdef _WIN32
    HANDLE process = nullptr;

    // A job object covering the program and everything it starts. `kill()` ends the whole tree: a
    // batch file runs as cmd.exe, which only then starts the actual program, and killing cmd.exe
    // alone would leave that program alive. Closing the handle kills nothing (there is no
    // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE), just as letting go of a `Child` leaves the program to
    // its fate.
    HANDLE job = nullptr;

    // The pseudoconsole of a program in a terminal (`teletype`). It lives here, not with the pipes,
    // because the burial closes it once the program has ended: only then does the output pipe reach
    // its end, and a reader learns that the terminal is over.
    void* console = nullptr;

    // Takes the pseudoconsole out under the lock and closes it; whoever comes second finds nothing.
    void unconsoled() {
        void* one = nullptr;

        {
            const std::lock_guard<std::mutex> kept{guard};

            one = console;
            console = nullptr;
        }

        if (one != nullptr && consoles().close != nullptr) consoles().close(one);
    }

    // Closes the pseudoconsole, the process and the job handles.
    ~Grave() {
        unconsoled();

        if (process != nullptr) CloseHandle(process);
        if (job != nullptr) CloseHandle(job);
    }
#endif
};

struct Living {
    std::mutex guard;

    std::shared_ptr<Grave> grave = std::make_shared<Grave>();

#ifdef _WIN32
    HANDLE into = nullptr;
    HANDLE from = nullptr;
    HANDLE wrong = nullptr;

    // The wake-up event. `close_input()` raises it, and a writer waits for it together with the
    // completion of its own write, because a synchronous `WriteFile` already stuck on a full pipe
    // cannot be woken other than by canceling the I/O itself. It is manual-reset, because it has to
    // stay raised for good: it wakes every writer, not just the first, just like the wake-up pipe
    // on POSIX.
    HANDLE rouse = nullptr;
#else
    int into = -1;
    int from = -1;
    int wrong = -1;

    // The wake-up pipe. `close_input()` drops a byte into it, and a writer waiting in `poll` sees
    // it next to the input and the bell, because closing a descriptor does not wake a `poll`
    // already waiting on it. Without it, `close_input()` on a child that does not read would end
    // the write only when the child dies.
    int rouse[2] = {-1, -1};
#endif

    // Closing the input is deferred. The system hands out a freed descriptor number again right
    // away, so closing it under a running system call risks content meant for the child landing in
    // someone else's file. So the last writer to leave closes it, not whoever asked:
    // `close_input()` sets a flag, and `poured` counts itself.
    int writers = 0;
    bool sealing = false;

    bool merge = false;

    // Whether the program sits in a pseudoterminal (`teletype`): one stream each way, and on POSIX
    // the end of the terminal reads as an error (EIO), not as an empty read.
    bool terminal = false;

    std::shared_ptr<Ending> ending = std::make_shared<Ending>();

    path program;

    ~Living();
};

namespace {

#ifdef _WIN32

// Closes the handle once and marks it closed.
void closed(HANDLE& one) {
    if (one == nullptr || one == INVALID_HANDLE_VALUE) return;

    CloseHandle(one);
    one = nullptr;
}

#else

// Closes the descriptor once and marks it closed.
void closed(int& one) {
    if (one < 0) return;

    ::close(one);
    one = -1;
}

#endif

}

// Closes whatever pipes are still open.
Living::~Living() {
    closed(into);
    closed(from);
    closed(wrong);

#ifdef _WIN32

    if (rouse != nullptr) CloseHandle(rouse);

#else

    closed(rouse[0]);
    closed(rouse[1]);

#endif
}

namespace {

// Reads one chunk from one pipe. It is called from a pool worker, so that worker's bell is what
// turns `cancel()` on the promise into an interrupted wait rather than an interrupted system call.
// An empty result means the end of the pipe, not an error: the program has ended and nobody will
// write anything more.
Bytes sipped(Living& held, bool errors, std::size_t most) {
    Bytes made;

    if (most == 0) return made;

#ifdef _WIN32

    HANDLE one = nullptr;

    {
        const std::lock_guard<std::mutex> kept{held.guard};

        one = errors ? held.wrong : held.from;
    }

    if (one == nullptr) return made;

    // Our end of the pipe is overlapped (`reared`), so the read waits for its completion and for
    // the worker's bell at once, the same shape as `poll` on POSIX: a cancelled read wakes at once,
    // and an idle one costs nothing while it waits.
    const HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    if (ready == nullptr) amiss("Cannot read from the program.", held.program);

    // It is closed on every way out, including an exception from `amiss`.
    const std::shared_ptr<void> closing{ready, [](void* which) { CloseHandle(which); }};

    Bell* const stop = core::ringing();

    const DWORD asked = most < static_cast<std::size_t>(MAXDWORD) ? static_cast<DWORD>(most) : MAXDWORD;

    made.resize(asked);

    // The end of the pipe is a broken pipe, not an empty read. A program that writes zero bytes
    // completes a waiting read with nothing, and that must not be taken for the end, so such a read
    // is simply made again.
    for (;;) {
        OVERLAPPED over{};

        ResetEvent(ready);

        over.hEvent = ready;

        DWORD given = 0;

        if (ReadFile(one, made.data(), asked, &given, &over) == 0) {
            const DWORD why = GetLastError();

            if (why == ERROR_BROKEN_PIPE || why == ERROR_HANDLE_EOF) {
                made.clear();

                return made;
            }

            if (why != ERROR_IO_PENDING) amiss("Cannot read from the program.", held.program);

            const HANDLE these[2] = {ready, stop != nullptr ? static_cast<HANDLE>(stop->waited()) : nullptr};
            const DWORD many = stop != nullptr ? 2 : 1;

            const DWORD which = WaitForMultipleObjects(many, these, FALSE, INFINITE);

            if (which != WAIT_OBJECT_0) {
                // A read taken back has to be finished before its `OVERLAPPED` and buffer
                // disappear: `GetOverlappedResult` with waiting gives the certainty that the kernel
                // no longer touches that memory.
                (void)CancelIoEx(one, &over);
                (void)GetOverlappedResult(one, &over, &given, TRUE);

                if (which == WAIT_OBJECT_0 + 1) throw Cancelled{};

                amiss("Cannot read from the program.", held.program);
            }
        }

        if (GetOverlappedResult(one, &over, &given, TRUE) == 0) {
            const DWORD why = GetLastError();

            made.clear();

            if (why == ERROR_BROKEN_PIPE || why == ERROR_HANDLE_EOF) return made;

            amiss("Cannot read from the program.", held.program);
        }

        if (given == 0) continue;

        made.resize(given);

        return made;
    }

#else

    int one = -1;

    {
        const std::lock_guard<std::mutex> kept{held.guard};

        one = errors ? held.wrong : held.from;
    }

    if (one < 0) return made;

    Bell* const stop = core::ringing();

    for (;;) {
        pollfd watched[2];
        int many = 1;

        watched[0] = pollfd{one, POLLIN, 0};

        if (stop != nullptr) {
            watched[1] = pollfd{stop->waited(), POLLIN, 0};
            many = 2;
        }

        const int ready = ::poll(watched, static_cast<nfds_t>(many), -1);

        if (ready < 0) {
            if (errno == EINTR) continue;

            amiss("Cannot read from the program.", held.program);
        }

        if (many == 2 && (watched[1].revents & POLLIN) != 0) throw Cancelled{};

        // `POLLNVAL` has to be in the mask, even though nobody asks for it. `poll` reports it when
        // the descriptor is already closed and returns with it at once; without this condition the
        // loop would spin forever, burning a pool worker and never settling the promise. This is
        // not a hypothetical state: the pipes are closed by `close_input()` and by the destructor,
        // both from a thread other than this one.
        if ((watched[0].revents & POLLNVAL) != 0)
            amiss("The program output is already closed.", held.program);

        if ((watched[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0) continue;

        made.resize(most);

        const ssize_t given = ::read(one, made.data(), most);

        if (given < 0) {
            made.clear();

            if (errno == EINTR || errno == EAGAIN) continue;

            // The master side of a pseudoterminal whose last slave has closed answers with EIO:
            // that is its end, as an empty read is a pipe's.
            if (errno == EIO && held.terminal) return made;

            amiss("Cannot read from the program.", held.program);
        }

        made.resize(static_cast<std::size_t>(given));

        return made;
    }

#endif
}

#ifndef _WIN32

// Writing to a pipe whose other end is already dead sends SIGPIPE, and the default reaction to
// SIGPIPE is the death of our own process. We block it for the time of writing and only on this
// thread: setting it to ignored once and for all would be a silent change to the conditions of the
// whole application that nobody asked us for. A signal that has managed to gather we take off the
// queue ourselves.
class Muffled {
public:
    // Blocks SIGPIPE on this thread, remembering the mask it had.
    Muffled() {
        sigemptyset(&mine_);
        sigaddset(&mine_, SIGPIPE);

        held_ = pthread_sigmask(SIG_BLOCK, &mine_, &was_) == 0;
    }

    // Consumes a SIGPIPE that arrived meanwhile, then restores the thread's mask.
    ~Muffled() {
        if (!held_) return;

        sigset_t waiting;

        if (sigpending(&waiting) == 0 && sigismember(&waiting, SIGPIPE) == 1) {
            int which = 0;

            (void)sigwait(&mine_, &which);
        }

        (void)pthread_sigmask(SIG_SETMASK, &was_, nullptr);
    }

    Muffled(const Muffled&) = delete;
    Muffled& operator=(const Muffled&) = delete;
    Muffled(Muffled&&) = delete;
    Muffled& operator=(Muffled&&) = delete;

private:
    sigset_t mine_{};
    sigset_t was_{};
    bool held_ = false;
};

#endif

// The last writer to leave closes the input. A guard, because `poured` also leaves by exception:
// through `amiss`, through `Cancelled` from the bell and through a `throw` from the close itself.
// If an ordinary line at the end decremented the counter, each of these exits would leave the input
// open forever.
class Writing {
public:
    explicit Writing(Living& held) : held_(held) {}

    // Leaves the count of writers and closes the input if it was the last one and closing was
    // requested.
    ~Writing() {
        const std::lock_guard<std::mutex> kept{held_.guard};

        if (--held_.writers == 0 && held_.sealing) closed(held_.into);
    }

    Writing(const Writing&) = delete;
    Writing& operator=(const Writing&) = delete;
    Writing(Writing&&) = delete;
    Writing& operator=(Writing&&) = delete;

private:
    Living& held_;
};

// Writes all of the bytes into the program's input, in chunks of at most `brim`, and returns how
// many were written. Throws when the input is closed, before or during the write, or the write
// fails; throws `Cancelled` when the worker's bell rings.
std::size_t poured(Living& held, const Bytes& what) {
    if (what.empty()) return 0;

#ifdef _WIN32

    HANDLE one = nullptr;
    HANDLE wake = nullptr;

    {
        const std::lock_guard<std::mutex> kept{held.guard};

        if (held.sealing || held.into == nullptr)
            amiss("The program input is already closed.", held.program);

        one = held.into;
        wake = held.rouse;

        ++held.writers;
    }

    const Writing counted{held};

    Bell* const stop = core::ringing();

    // The event for the completion of a write. Our own, not shared: each chunk waits for its own.
    const HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    if (ready == nullptr) amiss("Cannot write to the program.", held.program);

    // It is closed on every way out, including an exception from `amiss`.
    const std::shared_ptr<void> closing{ready, [](void* which) { CloseHandle(which); }};

    std::size_t done = 0;

    while (done < what.size()) {
        const std::size_t left = what.size() - done;
        const DWORD asked = left > brim ? static_cast<DWORD>(brim) : static_cast<DWORD>(left);

        OVERLAPPED over{};

        ResetEvent(ready);

        over.hEvent = ready;

        DWORD given = 0;

        // The handle is overlapped, so `WriteFile` nearly always returns "pending", and that is an
        // ordinary situation, not a failure. True means "it went at once"; even then we ask for the
        // result one call below, so as not to have two ways out.
        if (WriteFile(one, what.data() + done, asked, &given, &over) == 0) {
            if (GetLastError() != ERROR_IO_PENDING)
                amiss("Cannot write to the program.", held.program);

            // We wait for three things at once: the chunk going through, the sealing and the
            // worker's bell. This is the same shape as `poll` on POSIX, and for the same reason:
            // closing the input or cancelling the write has to interrupt a write stuck on a full
            // pipe, not wait for the child to die.
            constexpr DWORD none = 3;

            HANDLE these[3] = {ready, nullptr, nullptr};
            DWORD many = 1;
            DWORD sealed = none;
            DWORD rung = none;

            if (wake != nullptr) {
                sealed = many;
                these[many++] = wake;
            }

            if (stop != nullptr) {
                rung = many;
                these[many++] = static_cast<HANDLE>(stop->waited());
            }

            const DWORD which = WaitForMultipleObjects(many, these, FALSE, INFINITE);

            if (which != WAIT_OBJECT_0) {
                // An interrupted chunk has to be taken back from the kernel before its `OVERLAPPED`
                // and buffer disappear. `CancelIoEx` asks for the interruption, and
                // `GetOverlappedResult` with waiting gives the certainty that the kernel no longer
                // touches that memory. The same holds when the wait itself failed.
                (void)CancelIoEx(one, &over);
                (void)GetOverlappedResult(one, &over, &given, TRUE);

                if (rung != none && which == WAIT_OBJECT_0 + rung) throw Cancelled{};

                if (sealed != none && which == WAIT_OBJECT_0 + sealed)
                    amiss("The program input is already closed.", held.program);

                amiss("Cannot write to the program.", held.program);
            }
        }

        if (GetOverlappedResult(one, &over, &given, TRUE) == 0 || given == 0)
            amiss("Cannot write to the program.", held.program);

        done += given;
    }

    return done;

#else

    int one = -1;
    int wake = -1;

    {
        const std::lock_guard<std::mutex> kept{held.guard};

        if (held.sealing || held.into < 0)
            amiss("The program input is already closed.", held.program);

        one = held.into;
        wake = held.rouse[0];

        ++held.writers;
    }

    const Writing counted{held};

    const Muffled quiet;

    Bell* const stop = core::ringing();

    std::size_t done = 0;

    while (done < what.size()) {
        // Three things at once: the child's input, the worker's bell and the wake-up pipe. The bell
        // turns `cancel()` into an interrupted wait, and the wake-up pipe does the same for
        // `close_input()`. Without the third, a writer stuck on a full pipe would wake only when
        // the child reads something or dies.
        pollfd watched[3];
        int many = 1;

        watched[0] = pollfd{one, POLLOUT, 0};

        if (stop != nullptr) {
            watched[many] = pollfd{stop->waited(), POLLIN, 0};
            ++many;
        }

        const int roused = wake < 0 ? -1 : many;

        if (roused >= 0) {
            watched[many] = pollfd{wake, POLLIN, 0};
            ++many;
        }

        const int ready = ::poll(watched, static_cast<nfds_t>(many), -1);

        if (ready < 0) {
            if (errno == EINTR) continue;

            amiss("Cannot write to the program.", held.program);
        }

        if (stop != nullptr && (watched[1].revents & POLLIN) != 0) throw Cancelled{};

        // The wake-up pipe is level-triggered and never drained: the byte dropped by
        // `close_input()` stays in it forever, so it wakes every writer, not just the first.
        if (roused >= 0 && (watched[roused].revents & POLLIN) != 0)
            amiss("The program input is already closed.", held.program);

        // A close between turns of the loop: then `poll` returns with this at once. `poll` does not
        // notice a close during its own wait; that is what the wake-up pipe is for.
        if ((watched[0].revents & POLLNVAL) != 0)
            amiss("The program input is already closed.", held.program);

        if ((watched[0].revents & (POLLOUT | POLLERR | POLLHUP)) == 0) continue;

        const ssize_t given = ::write(one, what.data() + done, what.size() - done);

        if (given < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;

            amiss("Cannot write to the program.", held.program);
        }

        done += static_cast<std::size_t>(given);
    }

    return done;

#endif
}

// Waiting for the end. It occupies its own pool worker for the whole life of the program: the pool
// grows without limit for blocking work, so this starves nobody, and polling every so often would
// cost staying awake while idle.
Exit buried(const std::shared_ptr<Grave>& grave, const path& program) {
    Exit made;

#ifdef _WIN32

    HANDLE one = nullptr;

    {
        const std::lock_guard<std::mutex> kept{grave->guard};

        one = grave->process;
    }

    if (one == nullptr) {
        made.ended = Exit::Ended::lost;

        return made;
    }

    if (WaitForSingleObject(one, INFINITE) != WAIT_OBJECT_0)
        amiss("Cannot wait for the program to end.", program);

    DWORD code = 0;

    {
        const std::lock_guard<std::mutex> kept{grave->guard};

        grave->gone = true;

        if (GetExitCodeProcess(one, &code) == 0) {
            made.ended = Exit::Ended::lost;

            return made;
        }
    }

    // A program we killed ends with the code we gave it ourselves. Windows has no signals, so there
    // is nothing to tell apart here; what remains is an ordinary exit with a code.
    made.ended = Exit::Ended::normal;
    made.code = static_cast<int>(code);

    return made;

#else

    const std::int64_t which = grave->which;

    int told = 0;

    for (;;) {
        // A stop is not the end. A program with our terminal stopped by Ctrl+Z (SIGTSTP) would
        // otherwise stand forever: `waitpid` without `WUNTRACED` would not return, the terminal
        // would belong to the stopped group, and twa would wait. We do not do job control, so a
        // stopped program is resumed: Ctrl+Z in a program started from twa does nothing, as in MC.
        const pid_t done = ::waitpid(static_cast<pid_t>(which), &told, grave->foreground ? WUNTRACED : 0);

        if (done > 0 && WIFSTOPPED(told)) {
            (void)::kill(-static_cast<pid_t>(which), SIGCONT);

            continue;
        }

        if (done > 0) break;

        if (errno == EINTR) continue;

        {
            const std::lock_guard<std::mutex> kept{grave->guard};

            grave->gone = true;
        }

        made.ended = Exit::Ended::lost;

        return made;
    }

    // Buried. From this moment the number may be given to someone else, so any signal sent to it
    // would hit a stranger's program, and that is why `stop()` and `kill()` ask for the same lock.
    {
        const std::lock_guard<std::mutex> kept{grave->guard};

        grave->gone = true;
    }

    if (WIFEXITED(told)) {
        made.ended = Exit::Ended::normal;
        made.code = WEXITSTATUS(told);
    } else if (WIFSIGNALED(told)) {
        made.ended = Exit::Ended::killed;
        made.signal = WTERMSIG(told);
    } else {
        made.ended = Exit::Ended::lost;
    }

    return made;

#endif
}

#ifdef _WIN32

// Quoting by the rules with which the MSVC runtime takes a command line apart: a backslash is an
// escape character only before a quotation mark and before another backslash standing before a
// quotation mark. We do it ourselves, because `CreateProcessW` takes one string, while the caller
// gave us the arguments separately and has no reason to know this rule.
void quoted(const std::wstring& one, std::wstring& into) {
    if (!one.empty() && one.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        into += one;

        return;
    }

    into += L'"';

    for (std::size_t at = 0; at < one.size(); ++at) {
        std::size_t slashes = 0;

        while (at < one.size() && one[at] == L'\\') {
            ++at;
            ++slashes;
        }

        if (at == one.size()) {
            into.append(slashes * 2, L'\\');

            break;
        }

        if (one[at] == L'"') {
            into.append(slashes * 2 + 1, L'\\');
        } else {
            into.append(slashes, L'\\');
        }

        into += one[at];
    }

    into += L'"';
}

// We search for a program by name ourselves: in our own program's folder, then in PATH, and in
// every place with the extensions from PATHEXT. Given a bare name, `CreateProcessW` would look into
// the current folder on the way (which can be a panel's folder with someone else's git.exe inside)
// and would add only .exe, so npm (npm.cmd) would not start. A path with a slash or a drive is
// taken as it is. Throws when nothing is found.
std::wstring sought(const path& program) {
    const std::wstring named = program.wstring();

    std::vector<std::wstring> endings{L""};

    if (program.extension().empty()) {
        endings.clear();

        std::wstring listed(32767, L'\0');

        const DWORD much = GetEnvironmentVariableW(L"PATHEXT", listed.data(), static_cast<DWORD>(listed.size()));

        listed.resize(much > 0 && much < listed.size() ? much : 0);

        if (listed.empty()) listed = L".COM;.EXE;.BAT;.CMD";

        for (std::size_t at = 0; at <= listed.size();) {
            const std::size_t next = std::min(listed.find(L';', at), listed.size());

            if (next > at) endings.push_back(listed.substr(at, next - at));

            at = next + 1;
        }
    }

    std::vector<std::wstring> places;

    if (named.find_first_of(L"\\/:") != std::wstring::npos) {
        places.emplace_back();
    } else {
        try {
            places.push_back(core::own_program().parent_path().wstring());
        } catch (const Trouble&) {
        }

        std::wstring listed(32767, L'\0');

        const DWORD told = GetEnvironmentVariableW(L"PATH", listed.data(), static_cast<DWORD>(listed.size()));

        listed.resize(told > 0 && told < listed.size() ? told : 0);

        for (std::size_t at = 0; at <= listed.size();) {
            const std::size_t next = std::min(listed.find(L';', at), listed.size());

            std::wstring one = listed.substr(at, next - at);

            one.erase(std::remove(one.begin(), one.end(), L'"'), one.end());

            if (!one.empty()) places.push_back(std::move(one));

            at = next + 1;
        }
    }

    for (const std::wstring& place : places)
        for (const std::wstring& ending : endings) {
            const std::wstring tried = place.empty() ? named + ending : (path{place} / (named + ending)).wstring();

            const DWORD told = GetFileAttributesW(tried.c_str());

            if (told != INVALID_FILE_ATTRIBUTES && (told & FILE_ATTRIBUTE_DIRECTORY) == 0) return tried;
        }

    // Not found: we refuse here. A bare name passed on would make `CreateProcessW` go back to the
    // current folder, exactly where we must not take it from.
    amiss("Cannot start the program.", program);
}

// Whether the program is a batch file (.bat or .cmd), which only cmd.exe can run.
bool batched(const std::wstring& program) {
    std::wstring ending = path{program}.extension().wstring();

    for (wchar_t& one : ending) one = static_cast<wchar_t>(std::towlower(one));

    return ending == L".bat" || ending == L".cmd";
}

// A batch file runs in cmd.exe, which takes the line apart by its own rules: `&`, `|` and
// `%VARIABLE%` in an argument would run someone else's command (BatBadBut, 2024). So we call
// cmd.exe explicitly (from the system folder, without AutoRun via `/d`, without delayed expansion
// via `/v:off`), and every argument goes in quotation marks, inside which `&|<>^()` mean nothing.
// What remains are the characters quotes do not protect: the quotation mark, the percent sign and
// the line end. Such an argument is not passed at all; refusing is better than guessing what cmd
// would make of it.
void batch_quoted(const std::wstring& one, std::wstring& into, const path& program) {
    if (one.find_first_of(L"\"%\r\n") != std::wstring::npos || one.find(L'\0') != std::wstring::npos)
        amiss("This argument cannot be passed to a batch file.", program);

    into += L'"';
    into += one;
    into += L'"';
}

struct Launch {
    std::wstring application;
    std::wstring line;
};

// The application path and the command line for `CreateProcessW`: the program found by `sought`
// with its arguments quoted, or cmd.exe from the system folder around a batch file.
Launch commanded(const Command& what) {
    Launch made;

    made.application = sought(what.program);

    if (batched(made.application)) {
        std::wstring system(MAX_PATH, L'\0');

        const UINT much = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));

        system.resize(much > 0 && much < system.size() ? much : 0);

        const std::wstring shell = (path{system} / L"cmd.exe").wstring();

        made.line = L"cmd.exe /d /v:off /s /c \"";

        batch_quoted(made.application, made.line, what.program);

        for (const std::string& one : what.arguments) {
            made.line += L' ';

            batch_quoted(::tal::text::wide(one), made.line, what.program);
        }

        made.line += L'"';
        made.application = shell;

        return made;
    }

    quoted(what.program.wstring(), made.line);

    for (const std::string& one : what.arguments) {
        made.line += L' ';

        quoted(::tal::text::wide(one), made.line);
    }

    return made;
}

// The child's environment: ours when it inherits, with the command's variables set over it (names
// compared without regard to case, as Windows does). The environment block is a sequence of
// `name=value` pairs, each ending with a zero, and the whole with a second zero.
std::wstring surrounded(const Command& what) {
    std::vector<std::wstring> among;

    if (what.inherit) {
        wchar_t* whole = GetEnvironmentStringsW();

        if (whole != nullptr) {
            for (wchar_t* at = whole; *at != L'\0';) {
                const std::wstring one{at};

                // Windows keeps entries starting with `=` at the beginning of the block; they are
                // not variables but the current folders of the drives, and we copy them untouched.
                among.push_back(one);

                at += one.size() + 1;
            }

            FreeEnvironmentStringsW(whole);
        }
    }

    for (const Variable& one : what.environment) {
        const std::wstring name = ::tal::text::wide(one.name);

        const auto at = std::find_if(among.begin(), among.end(), [&name](const std::wstring& had) {
            return had.size() > name.size() && had[name.size()] == L'=' &&
                   _wcsnicmp(had.c_str(), name.c_str(), name.size()) == 0;
        });

        // A variable without a value is taken away.
        if (!one.value) {
            if (at != among.end()) among.erase(at);

            continue;
        }

        const std::wstring said = name + L'=' + ::tal::text::wide(*one.value);

        if (at == among.end()) among.push_back(said);
        else *at = said;
    }

    std::wstring made;

    for (const std::wstring& one : among) {
        made += one;
        made += L'\0';
    }

    made += L'\0';

    return made;
}

#else

// The PATH search happens before forking, so that the child calls `execve` with a full path and not
// execvp: between `fork` and `exec` only what is async-signal-safe may be called, and execvp
// allocates memory along the way. Here we allocate in the parent, at leisure. A name with a slash
// is taken as it is, and a name not found is returned unchanged for `execve` to fail on.
std::string found(const path& program) {
    const std::string named = program.string();

    if (named.find('/') != std::string::npos) return named;

    const char* whole = std::getenv("PATH");

    if (whole == nullptr) return named;

    const std::string said{whole};

    std::size_t at = 0;

    while (at <= said.size()) {
        const std::size_t next = said.find(':', at);
        const std::string one = said.substr(at, next == std::string::npos ? next : next - at);

        const std::string tried = (one.empty() ? std::string{"."} : one) + "/" + named;

        if (::access(tried.c_str(), X_OK) == 0) return tried;

        if (next == std::string::npos) break;

        at = next + 1;
    }

    return named;
}

// The child's environment: ours when it inherits, with the command's variables set over it, as
// `name=value` strings.
std::vector<std::string> surrounded(const Command& what) {
    std::vector<std::string> among;

    if (what.inherit && environ != nullptr)
        for (char** at = environ; *at != nullptr; ++at) among.emplace_back(*at);

    for (const Variable& one : what.environment) {
        const auto at = std::find_if(among.begin(), among.end(), [&one](const std::string& had) {
            return had.size() > one.name.size() && had[one.name.size()] == '=' &&
                   had.compare(0, one.name.size(), one.name) == 0;
        });

        // A variable without a value is taken away.
        if (!one.value) {
            if (at != among.end()) among.erase(at);

            continue;
        }

        const std::string said = one.name + "=" + *one.value;

        if (at == among.end()) among.push_back(said);
        else *at = said;
    }

    return among;
}

#endif

#ifdef _WIN32

// A pipe whose end on our side is overlapped and whose end for the child is ordinary, the way every
// program expects its standard handles. CreatePipe gives only synchronous handles, and a
// synchronous call stuck on a pipe can be woken only by CancelSynchronousIo with another thread's
// handle, which leaves a window that cannot be closed: between checking the flag and entering the
// call, the interruption hits nothing. An overlapped end lets our reader and writer wait for the
// completion and for whatever should interrupt them at the same time, as `poll` does on POSIX.
// `inbound` says that we read from the pipe and the child writes. Both ends come out inheritable;
// the caller takes the right away from ours. The name has to be unique in the whole system, so it
// takes the process number and a counter; the first instance only, and local clients only, so that
// nobody else can take the name over or connect from another machine.
bool paired(bool inbound, SECURITY_ATTRIBUTES& how, HANDLE& ours, HANDLE& theirs) {
    static std::atomic<unsigned> counted{0};

    wchar_t named[64];

    // `swprintf`, not _snwprintf_s: the latter exists only in MSVC, and the file should also build
    // under MinGW.
    (void)swprintf(named, sizeof(named) / sizeof(named[0]), L"\\\\.\\pipe\\talcite-%lu-%u",
                   static_cast<unsigned long>(GetCurrentProcessId()), counted.fetch_add(1));

    const DWORD side = (inbound ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) | FILE_FLAG_OVERLAPPED |
                       FILE_FLAG_FIRST_PIPE_INSTANCE;

    ours = CreateNamedPipeW(named, side, PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536,
                            65536, 0, &how);

    if (ours == INVALID_HANDLE_VALUE) {
        ours = nullptr;

        return false;
    }

    theirs = CreateFileW(named, inbound ? GENERIC_WRITE : GENERIC_READ, 0, &how, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);

    if (theirs == INVALID_HANDLE_VALUE) {
        theirs = nullptr;

        return false;
    }

    return true;
}

#endif

#ifdef _WIN32

// The handles a start puts together: the three pipes, each with our end and the program's, and the
// pseudoconsole between two of them (`console`).
struct Plumbing {
    HANDLE in_read = nullptr;
    HANDLE in_write = nullptr;
    HANDLE out_read = nullptr;
    HANDLE out_write = nullptr;
    HANDLE err_read = nullptr;
    HANDLE err_write = nullptr;
    void* console = nullptr;

    // Closes every pipe end still open, after a failed start.
    void undone() {
        closed(in_read);
        closed(in_write);
        closed(out_read);
        closed(out_write);
        closed(err_read);
        closed(err_write);
    }

    // Closes the pseudoconsole, when there is one, after a failed start.
    void unconsoled() {
        if (console != nullptr) consoles().close(console);

        console = nullptr;
    }
};

// The pipes of a start, and for a program in a pseudoconsole (`terminal`) the pseudoconsole between
// them. A program that gets our terminal (`ahead`) needs neither. The child's wake-up event for
// `close_input()` comes along (`Living::rouse`). A failure closes what was made and refuses.
void plumbed(Plumbing& ends, const Command& what, bool ahead, const std::optional<Size>& terminal, Living& held) {
    SECURITY_ATTRIBUTES how{};
    how.nLength = sizeof(how);
    how.bInheritHandle = TRUE;

    if (terminal) {
        // Two pipes, overlapped on our side as below, and the pseudoconsole between them and the
        // program: it reads what we write, writes what the program shows, and the program sees a
        // console. Nothing is inherited: the program reaches the pseudoconsole through the
        // attribute below, not through handles.
        if (consoles().open == nullptr) amiss("This system has no pseudoconsole (Windows 10 1809 or later).", what.program);

        how.bInheritHandle = FALSE;

        if (!paired(false, how, ends.in_write, ends.in_read)) {
            ends.undone();
            amiss("Cannot create the input pipe.", what.program);
        }

        if (!paired(true, how, ends.out_read, ends.out_write)) {
            ends.undone();
            amiss("Cannot create the output pipe.", what.program);
        }

        if (FAILED(consoles().open(measured(*terminal), ends.in_read, ends.out_write, 0, &ends.console))) {
            ends.undone();
            amiss("Cannot create the pseudoconsole.", what.program);
        }

        // The pseudoconsole holds its own copies of its two ends.
        closed(ends.in_read);
        closed(ends.out_write);

        held.rouse = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    } else if (!ahead) {
        // All three pipes have an overlapped end on our side (`paired`): the writer waits for the
        // completion and for the wake-up event of `close_input()`, the readers for the completion
        // and for the worker's bell, so both a sealed input and a cancelled read or write wake at
        // once instead of waiting for the child.
        if (!paired(false, how, ends.in_write, ends.in_read)) {
            ends.undone();
            amiss("Cannot create the input pipe.", what.program);
        }

        if (!paired(true, how, ends.out_read, ends.out_write)) {
            ends.undone();
            amiss("Cannot create the output pipe.", what.program);
        }

        if (!what.merge && !paired(true, how, ends.err_read, ends.err_write)) {
            ends.undone();
            amiss("Cannot create the error pipe.", what.program);
        }

        // Our ends must not be inherited: a child holding the write end of its own output would
        // never see the end of the pipe.
        SetHandleInformation(ends.in_write, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(ends.out_read, HANDLE_FLAG_INHERIT, 0);

        // The wake-up event for `close_input()`. If it cannot be created, only the deferred close
        // remains: the writer learns about the sealing on the next chunk instead of at once. That
        // is a degradation, not a failure, so we do not reject the child for it.
        held.rouse = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        if (ends.err_read != nullptr) SetHandleInformation(ends.err_read, HANDLE_FLAG_INHERIT, 0);
    }
}

// The start information for `CreateProcessW`: for a program in a pseudoconsole (`terminal`) one
// attribute, the pseudoconsole; for one on pipes the standard handles and the list of the handles
// it inherits (`passed`, which must outlive the start, since the list points at it). The attribute
// list lives in `room`. A program that gets our terminal (`ahead`) gets the plain form. A failure
// closes what the start holds and refuses.
void attributed(STARTUPINFOEXW& setting, std::vector<char>& room, HANDLE (&passed)[3], Plumbing& ends,
                const Command& what, bool ahead, bool terminal) {
    // The size has to match what we really pass: the extended form only when we add the handle
    // list. Given without that flag, it would be a lie about the size of the structure.
    setting.StartupInfo.cb =
        ahead ? static_cast<DWORD>(sizeof(STARTUPINFOW)) : static_cast<DWORD>(sizeof(setting));

    if (terminal) {
        // One attribute: the pseudoconsole, given as the handle itself, not as a pointer to it.
        SIZE_T much = 0;

        InitializeProcThreadAttributeList(nullptr, 1, 0, &much);

        room.resize(much);

        setting.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(room.data());

        if (InitializeProcThreadAttributeList(setting.lpAttributeList, 1, 0, &much) == 0) {
            ends.undone();
            ends.unconsoled();
            amiss("Cannot prepare the pseudoconsole.", what.program);
        }

        if (UpdateProcThreadAttribute(setting.lpAttributeList, 0, given_console, ends.console, sizeof(ends.console),
                                      nullptr, nullptr) == 0) {
            DeleteProcThreadAttributeList(setting.lpAttributeList);
            ends.undone();
            ends.unconsoled();
            amiss("Cannot pass the pseudoconsole.", what.program);
        }

        // The program's standard handles are said to be none, so that it takes the pseudoconsole's.
        // Without this a program started from a process whose own output goes to a file or a pipe
        // (a log, a script that runs the tests) gets that file instead: it writes past the
        // pseudoconsole and never sees a terminal. Windows Terminal does the same.
        setting.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        setting.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
        setting.StartupInfo.hStdOutput = INVALID_HANDLE_VALUE;
        setting.StartupInfo.hStdError = INVALID_HANDLE_VALUE;
    } else if (!ahead) {
        setting.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        setting.StartupInfo.hStdInput = ends.in_read;
        setting.StartupInfo.hStdOutput = ends.out_write;
        setting.StartupInfo.hStdError = what.merge ? ends.out_write : ends.err_write;

        // Handle inheritance on Windows is all or nothing, unless a list is given. Without it the
        // child would also get our sockets and keep a port busy long after we close it.
        passed[0] = ends.in_read;
        passed[1] = ends.out_write;
        passed[2] = what.merge ? ends.out_write : ends.err_write;

        SIZE_T much = 0;

        InitializeProcThreadAttributeList(nullptr, 1, 0, &much);

        room.resize(much);

        setting.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(room.data());

        if (InitializeProcThreadAttributeList(setting.lpAttributeList, 1, 0, &much) == 0) {
            ends.undone();
            amiss("Cannot prepare the handle list.", what.program);
        }

        if (UpdateProcThreadAttribute(setting.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      passed, sizeof(HANDLE) * (what.merge ? 2 : 3), nullptr,
                                      nullptr) == 0) {
            DeleteProcThreadAttributeList(setting.lpAttributeList);
            ends.undone();
            amiss("Cannot pass the handle list.", what.program);
        }
    }
}

// The job of a started process, so that `kill()` ends the whole tree it starts. A job that cannot
// be made (the process is already in someone else's job without the right to leave, an old system)
// is no reason to refuse: null, and `kill()` then kills only the program itself.
HANDLE jobbed(HANDLE process) {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);

    if (job != nullptr && AssignProcessToJobObject(job, process) == 0) {
        CloseHandle(job);
        job = nullptr;
    }

    return job;
}

// Holds every process of `job` to `memory` bytes of committed memory each. False when there is no
// job or the system refuses the limit.
bool limited(HANDLE job, std::uint64_t memory) {
    if (job == nullptr) return false;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};

    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limit.ProcessMemoryLimit = static_cast<SIZE_T>(memory);

    return SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof limit) != 0;
}

#else

// The descriptors a start puts together: the three pipes, the start pipe (`starting`) and the two
// sides of a pseudoterminal.
struct Plumbing {
    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int starting[2] = {-1, -1};
    int master = -1;
    int slave = -1;

    // Closes every descriptor still open, after a failed start.
    void undone() {
        for (int* one : {&in_pipe[0], &in_pipe[1], &out_pipe[0], &out_pipe[1], &err_pipe[0],
                         &err_pipe[1], &starting[0], &starting[1], &master, &slave})
            closed(*one);
    }
};

// The descriptors of a start: a pseudoterminal for a program that gets one (`terminal`), the three
// pipes for a program that gets neither it nor our terminal (`ahead`), and the start pipe for every
// program. A failure closes what was made and refuses.
void plumbed(Plumbing& ends, const Command& what, bool ahead, const std::optional<Size>& terminal) {
    // A pseudoterminal instead of pipes: both sides closed on exec, the slave opened here, before
    // the fork, so that the child needs no `ptsname`, which is not safe between `fork` and `exec`.
    // The size is set before the program starts, so its first look at the terminal is right.
    if (terminal) {
        ends.master = mastered();

        if (ends.master < 0) amiss("Cannot open a pseudoterminal.", what.program);

        ends.slave = slaved(ends.master);

        if (ends.slave < 0) {
            ends.undone();
            amiss("Cannot open a pseudoterminal.", what.program);
        }

        const winsize size = measured(*terminal);

        (void)::ioctl(ends.master, TIOCSWINSZ, &size);
    }

    // All three pipes are closed on exec from birth (`sealed::piped`): another program starting at
    // the same moment from another thread will not get the ends of this one. The child moves its
    // ends to 0, 1 and 2, and `dup2` does not carry the flag over.
    if (!ahead && !terminal) {
        if (!sealed::piped(ends.in_pipe, false)) amiss("Cannot create the input pipe.", what.program);

        if (!sealed::piped(ends.out_pipe, false)) {
            ends.undone();
            amiss("Cannot create the output pipe.", what.program);
        }

        if (!what.merge && !sealed::piped(ends.err_pipe, false)) {
            ends.undone();
            amiss("Cannot create the error pipe.", what.program);
        }
    }

    // The start pipe, also closed on exec. A successful `execve` closes the child's end and we
    // read the end of the pipe; a failed `chdir` or `execve` writes its errno there first. Without
    // it a program that cannot be started would start anyway and end with 127, as from a shell,
    // while on Windows the same command is a refusal; this way both systems refuse alike, and with
    // the reason.
    if (!sealed::piped(ends.starting, false)) {
        ends.undone();
        amiss("Cannot create the start pipe.", what.program);
    }
}

// Everything the child will need, put together before the fork: after it the child may call only
// what is async-signal-safe, so there is no memory allocation or string building there anymore. The
// arguments and the environment (`argv`, `envp`) point into the strings (`words`, `among`), so a
// start is never copied.
struct Startup {
    std::string program;
    std::string where;
    std::uint64_t memory = 0;
    std::vector<std::string> among;
    std::vector<std::string> words;
    std::vector<char*> argv;
    std::vector<char*> envp;

    // The program found on the PATH, the folder, the limit of memory, the environment and the
    // words of the command.
    explicit Startup(const Command& what)
        : program(found(what.program)), where(what.where.empty() ? std::string{} : what.where.string()), memory(what.memory),
          among(surrounded(what)) {
        words.push_back(what.program.string());

        for (const std::string& one : what.arguments) words.push_back(one);

        for (std::string& one : words) argv.push_back(one.data());

        argv.push_back(nullptr);

        for (std::string& one : among) envp.push_back(one.data());

        envp.push_back(nullptr);
    }

    Startup(const Startup&) = delete;
    Startup& operator=(const Startup&) = delete;
};

// The child's side of the fork, up to `execve`, for a program in a pseudoterminal (`terminal`), one
// that gets our terminal (`ahead`) or one on pipes. It runs between `fork` and `exec`, so it calls
// only what is async-signal-safe. A failed `chdir`, limit of memory or `execve` tells the parent
// its errno through the start pipe and ends with 127.
[[noreturn]] void begun(const Plumbing& ends, const Startup& start, bool merge, bool ahead, bool terminal) {
    // The reason of a failed start, for the parent. No memory allocation.
    const auto say = [&ends](int why) { (void)!::write(ends.starting[1], &why, sizeof why); };

    if (terminal) {
        // A session of its own, led by the program, with the slave as its controlling terminal:
        // the line discipline then sends Ctrl-C and SIGWINCH to whatever the program puts in
        // the foreground, as in any terminal window. Linux takes the controlling terminal on
        // the first open after `setsid`, but the slave is open already, so it is asked for.
        (void)::setsid();
        (void)::ioctl(ends.slave, TIOCSCTTY, 0);

        (void)::dup2(ends.slave, STDIN_FILENO);
        (void)::dup2(ends.slave, STDOUT_FILENO);
        (void)::dup2(ends.slave, STDERR_FILENO);

        if (ends.slave > STDERR_FILENO) ::close(ends.slave);
        if (ends.master > STDERR_FILENO) ::close(ends.master);

        for (const int one : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) ::fcntl(one, F_SETFD, 0);

        // A terminal's program starts with the signals as a terminal window gives them: none
        // blocked and none ignored. Both survive `exec`, so a program started under nohup
        // would otherwise give its shell an ignored SIGHUP, and hanging up would not end it.
        sigset_t none;

        sigemptyset(&none);
        (void)::sigprocmask(SIG_SETMASK, &none, nullptr);

        for (const int one : {SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGTSTP, SIGTTIN, SIGTTOU, SIGCHLD, SIGWINCH})
            std::signal(one, SIG_DFL);
    } else if (ahead) {
        // Its own process group and the foreground: without this Ctrl-C would go to us, and an
        // attempt to read from the terminal would stop the child with SIGTTIN. `tcsetpgrp`
        // itself also sends a signal, SIGTTOU, so we silence it first.
        ::setpgid(0, 0);

        std::signal(SIGTTOU, SIG_IGN);

        (void)::tcsetpgrp(STDIN_FILENO, ::getpgrp());
    } else {
        // Its own group too, but without the foreground: `stop()` and `kill()` then reach the
        // whole group, so the programs a shell started die with it.
        ::setpgid(0, 0);

        (void)::dup2(ends.in_pipe[0], STDIN_FILENO);
        (void)::dup2(ends.out_pipe[1], STDOUT_FILENO);
        (void)::dup2(merge ? ends.out_pipe[1] : ends.err_pipe[1], STDERR_FILENO);

        for (const int one : {ends.in_pipe[0], ends.in_pipe[1], ends.out_pipe[0], ends.out_pipe[1], ends.err_pipe[0],
                              ends.err_pipe[1]})
            if (one > STDERR_FILENO) ::close(one);

        // A pipe end that already was one of 0, 1, 2 (a parent without standard input gets
        // exactly zero from `pipe2`) is left by `dup2` as it was, with the flag, and the flag
        // would close it on `exec`.
        for (const int one : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) ::fcntl(one, F_SETFD, 0);
    }

    // Signals we set to ignored stay ignored after `exec`, and a program that inherited an
    // ignored SIGPIPE behaves differently from one started from a shell.
    std::signal(SIGPIPE, SIG_DFL);

    if (!start.where.empty() && ::chdir(start.where.c_str()) != 0) {
        say(errno);

        ::_exit(127);
    }

    // A limit of memory, on the data the program may take (not on its addresses, which engines
    // like WebKit reserve by the gigabyte without using them). It holds from the program's first
    // instruction; `setrlimit` is a plain system call, safe here.
    if (start.memory != 0) {
        const auto most = static_cast<rlim_t>(start.memory);
        const rlimit limit{most, most};

        if (::setrlimit(RLIMIT_DATA, &limit) != 0) {
            say(errno);

            ::_exit(127);
        }
    }

    ::execve(start.program.c_str(), start.argv.data(), start.envp.data());

    say(errno);

    ::_exit(127);
}

// Gives the terminal's foreground to the process group. `tcsetpgrp` from outside the foreground
// sends us SIGTTOU, so the signal is ignored for that moment.
void foregrounded(pid_t group) {
    void (*was)(int) = std::signal(SIGTTOU, SIG_IGN);

    (void)::tcsetpgrp(STDIN_FILENO, group);

    std::signal(SIGTTOU, was == nullptr ? SIG_DFL : was);
}

// What the child said through the start pipe (`from`): the errno of a failed start, or nothing when
// the pipe ended first, as a successful `execve` ends it.
std::optional<int> heard_from(int from) {
    int why = 0;
    std::size_t heard = 0;

    while (heard < sizeof why) {
        const ssize_t much = ::read(from, reinterpret_cast<char*>(&why) + heard, sizeof why - heard);

        if (much > 0) {
            heard += static_cast<std::size_t>(much);

            continue;
        }

        if (much < 0 && errno == EINTR) continue;

        break;
    }

    if (heard == sizeof why) return why;

    return std::nullopt;
}

// Our pipe ends are non-blocking. The read and write loops are written for this (they ask `poll`,
// handle `EAGAIN` and keep the bell next to them), but without this setting `EAGAIN` never comes,
// because a blocking write to a pipe returns only after handing over everything it was asked to.
// The pool worker then stands inside a system call, where neither the bell nor `close_input()` can
// reach it: `close_input()` on a child that does not read would end the write only when the child
// dies. The setting applies only to our ends. A pipe has two separate open file descriptions (we
// hold the input for writing and the two outputs for reading, the child holds the other ends), so
// the child gets its ends blocking, as every program expects.
void unblocked(std::initializer_list<int> ours) {
    for (const int one : ours) {
        if (one < 0) continue;

        const int flags = ::fcntl(one, F_GETFL, 0);

        if (flags >= 0) (void)::fcntl(one, F_SETFL, flags | O_NONBLOCK);
    }
}

#endif

// Starting. `ahead` means "this program gets our terminal": then we create no pipes at all, because
// its input is the keyboard and its output the screen, and we also put it in the foreground, so
// that Ctrl-C goes to it and not to us. `terminal` means "this program gets a pseudoterminal of its
// own, of that size": one stream each way instead of three pipes, the program leads a session of
// its own with that terminal as its controlling one (on Windows, a pseudoconsole), and line
// editing, echo and Ctrl-C are the terminal's business, as in any terminal window.
std::shared_ptr<Living> reared(const Command& what, bool ahead, std::optional<Size> terminal = std::nullopt) {
    auto held = std::make_shared<Living>();

    held->program = what.program;
    held->merge = what.merge;
    held->terminal = terminal.has_value();

    if (what.program.empty()) amiss("No program given.", what.program);

#ifdef _WIN32

    Plumbing ends;

    plumbed(ends, what, ahead, terminal, *held);

    Launch launch;

    try {
        launch = commanded(what);
    } catch (...) {
        ends.undone();
        ends.unconsoled();
        throw;
    }

    std::wstring around = surrounded(what);
    const std::wstring where = what.where.empty() ? std::wstring{} : what.where.wstring();

    STARTUPINFOEXW setting{};
    std::vector<char> room;
    HANDLE passed[3] = {nullptr, nullptr, nullptr};

    attributed(setting, room, passed, ends, what, ahead, terminal.has_value());

    PROCESS_INFORMATION told{};

    // Its own process group for a program on pipes, so that `stop()` has somewhere to send
    // Ctrl-Break. A program that gets our terminal stays in our group, because otherwise Ctrl-C
    // pressed by the user would not reach it.
    //
    // It starts suspended, so that it enters the job before it starts anything itself.
    //
    // A program in a pseudoconsole needs no group of its own: Ctrl-C reaches it through the
    // pseudoconsole, and it inherits no handles.
    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED |
                        (terminal ? EXTENDED_STARTUPINFO_PRESENT
                                  : ahead ? 0u : (EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_PROCESS_GROUP));

    const BOOL done = CreateProcessW(launch.application.c_str(), launch.line.data(), nullptr, nullptr,
                                     ahead || terminal ? FALSE : TRUE, flags, around.data(),
                                     where.empty() ? nullptr : where.c_str(), &setting.StartupInfo, &told);

    if (!ahead) DeleteProcThreadAttributeList(setting.lpAttributeList);

    if (done == 0) {
        ends.undone();
        ends.unconsoled();
        amiss("Cannot start the program.", what.program);
    }

    const HANDLE job = jobbed(told.hProcess);

    // A limit of memory holds from the program's first instruction, set while it is still
    // suspended; a program that cannot be held to it is not let run.
    if (what.memory != 0 && !limited(job, what.memory)) {
        TerminateProcess(told.hProcess, 1);
        CloseHandle(told.hThread);
        CloseHandle(told.hProcess);

        if (job != nullptr) CloseHandle(job);

        ends.undone();
        ends.unconsoled();
        amiss("Cannot limit the memory of the program.", what.program);
    }

    ResumeThread(told.hThread);
    CloseHandle(told.hThread);

    closed(ends.in_read);
    closed(ends.out_write);
    closed(ends.err_write);

    held->into = ends.in_write;
    held->from = ends.out_read;
    held->wrong = ends.err_read;

    held->grave->which = static_cast<std::int64_t>(told.dwProcessId);
    held->grave->process = told.hProcess;
    held->grave->job = job;
    held->grave->grouped = !ahead && !terminal;
    held->grave->console = ends.console;

#else

    // Put together before any descriptor, so that a failure here leaves none open.
    const Startup start{what};

    Plumbing ends;

    plumbed(ends, what, ahead, terminal);

    const pid_t which = ::fork();

    if (which < 0) {
        ends.undone();
        amiss("Cannot start the program.", what.program);
    }

    if (which == 0) begun(ends, start, what.merge, ahead, terminal.has_value());

    // The child does the same, and both do it on purpose. Whoever is first sets it; the second call
    // changes nothing. Otherwise a gap remains in which the child already reads and the foreground
    // is not there yet, or `kill()` hits a group that does not exist yet. Not for a program in a
    // pseudoterminal: it makes its own session, and `setsid` refuses a process that already leads
    // a group, so a parent who came first would leave it without a terminal.
    if (!terminal) ::setpgid(which, which);

    if (ahead) foregrounded(which);

    // Waits for the child to either execute the program or say why it could not.
    closed(ends.starting[1]);

    const std::optional<int> why = heard_from(ends.starting[0]);

    closed(ends.starting[0]);

    if (why) {
        // The child ends at once; it is buried here, since no wait for it exists yet. The terminal
        // it took comes back to us, because nobody will run `Term::execute` to its end.
        int status = 0;

        while (::waitpid(which, &status, 0) < 0 && errno == EINTR) {
        }

        if (ahead) foregrounded(::getpgrp());

        ends.undone();

        throw ::tal::Trouble{core::kind_of(std::error_code{*why, std::generic_category()}),
                             "Cannot start the program: " + std::string{std::strerror(*why)} + ".", core::told(what.program).text(), *why};
    }

    closed(ends.in_pipe[0]);
    closed(ends.out_pipe[1]);
    closed(ends.err_pipe[1]);
    closed(ends.slave);

    held->into = ends.in_pipe[1];
    held->from = ends.out_pipe[0];
    held->wrong = ends.err_pipe[0];

    // The master side is both the input and the output. The output gets a copy of its own, so that
    // closing one side never closes the other's number under it.
    if (terminal) {
        held->into = ends.master;
        held->from = ::fcntl(ends.master, F_DUPFD_CLOEXEC, 0);

        ends.master = -1;
    }

    unblocked({held->into, held->from, held->wrong});

    // The wake-up pipe for `close_input()`. If it cannot be created, only the deferred close
    // remains: the writer learns about the sealing on the next turn of the loop instead of at once.
    // That is a degradation, not a failure, so we do not reject the child for it.
    if (!sealed::piped(held->rouse, true)) held->rouse[0] = held->rouse[1] = -1;

    held->grave->which = static_cast<std::int64_t>(which);
    held->grave->foreground = ahead;

#endif

    const std::shared_ptr<Grave> grave = held->grave;
    const path program_named = what.program;
    const std::shared_ptr<Ending> ending = held->ending;

    // The burial runs on a thread of its own, let go, and only reports to the ending that the
    // waiters look at. A child whose end nobody ever asks about is still buried and leaves no
    // zombie in the process table; a thread of the io pool would be held for the program's life.
    //
    // On Windows a pseudoconsole outlives its program: the output pipe ends only when it is closed.
    // So the burial closes it, after the end is reported, because closing may wait until what is
    // left of the output has been read, and the end must not wait for a reader.
    std::thread{[grave, program_named, ending] {
        try {
            ending->settled(buried(grave, program_named));
        } catch (...) {
            ending->settled(Exit{Exit::Ended::lost, 0, 0});
        }

#ifdef _WIN32
        grave->unconsoled();
#endif
    }}.detach();

    return held;
}

// A program let loose (the sharing daemon in twa). It is not our child: it has no pipes, inherits
// nothing but the environment, does not die with us and nobody buries it; it stays when the program
// that started it ends, even when the terminal is closed. We return only the process number.
//
// POSIX: two forks with `setsid` in between. The grandchild has no controlling terminal and is not
// a session leader, so it will never get one, and its parent becomes the system, not us. Input and
// outputs go to /dev/null. The middle process reports the grandchild's number, and the grandchild
// reports the `exec` error when it fails; the pipe is closed on exec, so a successful `exec` closes
// it without a word. Each message is a character and eight bytes in one write, shorter than
// PIPE_BUF and therefore atomic, in whatever order the two arrive.
//
// Windows: a process without a console, in its own group, taken out of our job when the job allows
// it (otherwise it would die together with the job), without inheriting handles.
std::int64_t loosed(const Command& what) {
    if (what.program.empty()) amiss("No program given.", what.program);

#ifdef _WIN32

    Launch launch = commanded(what);

    std::wstring around = surrounded(what);
    const std::wstring where = what.where.empty() ? std::wstring{} : what.where.wstring();

    STARTUPINFOW setting{};

    setting.cb = sizeof(setting);

    PROCESS_INFORMATION told{};

    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP;

    BOOL done = CreateProcessW(launch.application.c_str(), launch.line.data(), nullptr, nullptr, FALSE,
                               flags | CREATE_BREAKAWAY_FROM_JOB, around.data(),
                               where.empty() ? nullptr : where.c_str(), &setting, &told);

    // A job that does not allow leaving refuses the whole start; then the program stays in it,
    // which is better than not starting at all.
    if (done == 0 && GetLastError() == ERROR_ACCESS_DENIED)
        done = CreateProcessW(launch.application.c_str(), launch.line.data(), nullptr, nullptr, FALSE, flags,
                              around.data(), where.empty() ? nullptr : where.c_str(), &setting, &told);

    if (done == 0) amiss("Cannot start the program.", what.program);

    CloseHandle(told.hThread);
    CloseHandle(told.hProcess);

    return static_cast<std::int64_t>(told.dwProcessId);

#else

    const std::string program = found(what.program);
    const std::string where = what.where.empty() ? std::string{} : what.where.string();

    std::vector<std::string> among = surrounded(what);
    std::vector<std::string> words;

    words.push_back(what.program.string());

    for (const std::string& one : what.arguments) words.push_back(one);

    std::vector<char*> argv;
    std::vector<char*> envp;

    for (std::string& one : words) argv.push_back(one.data());

    argv.push_back(nullptr);

    for (std::string& one : among) envp.push_back(one.data());

    envp.push_back(nullptr);

    int told[2] = {-1, -1};

    if (!sealed::piped(told, false)) amiss("Cannot create the start pipe.", what.program);

    // A character and eight bytes. No memory allocation: this runs between `fork` and `exec`.
    const auto say = [&told](char kind, std::int64_t value) {
        char one[9];

        one[0] = kind;

        std::memcpy(one + 1, &value, sizeof value);

        (void)!::write(told[1], one, sizeof one);
    };

    const pid_t middle = ::fork();

    if (middle < 0) {
        ::close(told[0]);
        ::close(told[1]);

        amiss("Cannot start the program.", what.program);
    }

    if (middle == 0) {
        ::close(told[0]);

        (void)::setsid();

        const pid_t last = ::fork();

        if (last < 0) {
            say('e', errno);

            ::_exit(127);
        }

        if (last > 0) {
            say('p', last);

            ::_exit(0);
        }

        const int nothing = ::open("/dev/null", O_RDWR);

        if (nothing >= 0) {
            for (const int one : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) (void)::dup2(nothing, one);

            if (nothing > STDERR_FILENO) ::close(nothing);
        }

        std::signal(SIGPIPE, SIG_DFL);

        if (!where.empty() && ::chdir(where.c_str()) != 0) {
            say('e', errno);

            ::_exit(127);
        }

        ::execve(program.c_str(), argv.data(), envp.data());

        say('e', errno);

        ::_exit(127);
    }

    ::close(told[1]);

    std::string heard;
    char room[64];

    for (;;) {
        const ssize_t much = ::read(told[0], room, sizeof room);

        if (much > 0) {
            heard.append(room, static_cast<std::size_t>(much));

            continue;
        }

        if (much < 0 && errno == EINTR) continue;

        break;
    }

    ::close(told[0]);

    int status = 0;

    while (::waitpid(middle, &status, 0) < 0 && errno == EINTR) {
    }

    std::int64_t pid = -1;
    std::int64_t failed = 0;

    for (std::size_t at = 0; at + 9 <= heard.size(); at += 9) {
        std::int64_t value = 0;

        std::memcpy(&value, heard.data() + at + 1, sizeof value);

        if (heard[at] == 'p') pid = value;
        else failed = value == 0 ? -1 : value;
    }

    if (failed != 0 || pid < 0)
        throw ::tal::Trouble{failed > 0 ? core::kind_of(std::error_code{static_cast<int>(failed), std::generic_category()}) : ::tal::Kind::other,
                             "Cannot start the program: " +
                                 (failed > 0 ? std::string{std::strerror(static_cast<int>(failed))} : std::string{"no process"}) + ".",
                             core::told(what.program).text(), static_cast<int>(failed)};

    return pid;

#endif
}

}

// Whether this system has pseudoterminals: every POSIX one does, Windows from 10 1809 on.
bool teletyped() {
#ifdef _WIN32
    return consoles().open != nullptr;
#else
    return true;
#endif
}

// Starts a program in a pseudoterminal of the given size, on the calling thread; see `reared`.
std::shared_ptr<Living> teletype(const Command& what, Size size) { return reared(what, false, size); }

// Tells the pseudoterminal of a program its new size. On POSIX the line discipline then sends
// SIGWINCH to the program in its foreground; on Windows the pseudoconsole tells the program itself.
// Does nothing for a program without one or once it is gone.
void resized(const std::shared_ptr<Living>& held, Size size) {
    if (held == nullptr || !held->terminal) return;

#ifdef _WIN32

    const std::lock_guard<std::mutex> kept{held->grave->guard};

    if (held->grave->console != nullptr) (void)consoles().resize(held->grave->console, measured(size));

#else

    const std::lock_guard<std::mutex> kept{held->guard};

    if (held->into < 0) return;

    const winsize told = measured(size);

    (void)::ioctl(held->into, TIOCSWINSZ, &told);

#endif
}


namespace {

// How long a program asked to end may take before it is killed.
constexpr std::chrono::seconds patience{3};

// Asks the program to end: SIGTERM to its whole group on POSIX, Ctrl-Break on Windows. Does
// nothing once the program is buried.
void stopped(const std::shared_ptr<Grave>& grave) {
    const std::lock_guard<std::mutex> kept{grave->guard};

    if (grave->gone) return;

#ifdef _WIN32
    // A console event goes to the whole group, so it may be sent only to a program for which we
    // made a group. The one that got our terminal sits in ours: a Ctrl-Break sent to its number
    // would kill us along with it. For that one only killing remains.
    if (grave->grouped) (void)GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, static_cast<DWORD>(grave->which));
#else
    // The program's whole group: the shell and what it started.
    (void)::kill(-static_cast<pid_t>(grave->which), SIGTERM);
#endif
}

// Hangs up the terminal of the program, as closing a terminal window does: SIGHUP to its whole
// group on POSIX, and SIGCONT after it, so that a shell stopped meanwhile hears it too, hands it to
// its jobs and ends; on Windows the pseudoconsole closes, which tells its programs the console is
// gone. Does nothing once the program is buried.
void hung(const std::shared_ptr<Grave>& grave) {
#ifdef _WIN32
    grave->unconsoled();
#else
    const std::lock_guard<std::mutex> kept{grave->guard};

    if (grave->gone) return;

    (void)::kill(-static_cast<pid_t>(grave->which), SIGHUP);
    (void)::kill(-static_cast<pid_t>(grave->which), SIGCONT);
#endif
}

// Kills the program at once: SIGKILL to its whole group on POSIX, the whole job (or the process
// alone) on Windows. Does nothing once the program is buried.
void killed(const std::shared_ptr<Grave>& grave) {
    const std::lock_guard<std::mutex> kept{grave->guard};

    if (grave->gone) return;

#ifdef _WIN32
    if (grave->job != nullptr) (void)TerminateJobObject(grave->job, 1);
    else if (grave->process != nullptr) (void)TerminateProcess(grave->process, 1);
#else
    (void)::kill(-static_cast<pid_t>(grave->which), SIGKILL);
#endif
}

// Closes the program's input. The last writer to leave closes it, not whoever asked: as long as a
// write sits in a system call on the descriptor, closing would give its number back to the system,
// which hands numbers out again at once. So a flag is set and the waiting writers are woken.
void seal_input(Living& held) {
    const std::lock_guard<std::mutex> kept{held.guard};

    if (held.sealing) return;

    held.sealing = true;

    if (held.writers == 0) {
        closed(held.into);

        return;
    }

#ifdef _WIN32
    if (held.rouse != nullptr) (void)SetEvent(held.rouse);
#else
    if (held.rouse[1] >= 0) {
        const char one = 0;

        if (::write(held.rouse[1], &one, 1) < 0) return;
    }
#endif
}

// What the program writes, on its output or its errors, read on the io pool; the end of the
// program's writing reads as nothing.
class Hearing : public Channel {
public:
    // The output, or the errors, of the program.
    Hearing(std::shared_ptr<Living> held, bool errors) : held_(std::move(held)), errors_(errors) {}

    // At most `most` bytes, as soon as the program has written some.
    Task<Bytes> read(std::size_t most) override {
        return offloaded([held = held_, errors = errors_, most] { return sipped(*held, errors, most); });
    }

    // The program's output is not written to.
    Task<void> write(Bytes) override {
        return []() -> Task<void> {
            throw Trouble{::tal::Kind::unsupported, "The output of a program only reads."};

            co_return;
        }();
    }

    // Nothing to end: the program's writing ends with the program.
    void close() override {}

private:
    std::shared_ptr<Living> held_;
    bool errors_ = false;
};

// The program's input, written on the io pool; a full pipe holds the writer until the program
// reads.
class Telling : public Channel {
public:
    // The input of the program.
    explicit Telling(std::shared_ptr<Living> held) : held_(std::move(held)) {}

    // The input of a program is not read from.
    Task<Bytes> read(std::size_t) override {
        return []() -> Task<Bytes> {
            throw Trouble{::tal::Kind::unsupported, "The input of a program only writes."};

            co_return Bytes{};
        }();
    }

    // Writes all the bytes; a program that has stopped reading breaks the stream.
    Task<void> write(Bytes what) override {
        return offloaded([held = held_, what = std::move(what)] { (void)poured(*held, what); });
    }

    // Ends the input: the program reads to its end.
    void close() override { seal_input(*held_); }

private:
    std::shared_ptr<Living> held_;
};

// A program of the system while it runs, and how it ended once it has.
class Alive : public Spawned {
public:
    // The program the core started, with its pipes or its terminal.
    explicit Alive(std::shared_ptr<Living> held) : held_(std::move(held)), grave_(held_->grave) {}

    // The system's number of the process.
    [[nodiscard]] std::int64_t id() const override { return grave_->which; }

    // How it ended, for any number of waiters.
    Task<Exit> ended() override { return Ending::awaited(held_->ending); }

    // Asks it to end, and kills it when it has not ended `patience` later.
    void stop() override {
        stopped(grave_);

        std::thread{[grave = std::weak_ptr<Grave>{grave_}] {
            std::this_thread::sleep_for(patience);

            if (const auto held = grave.lock()) killed(held);
        }}.detach();
    }

    // Kills it at once, with all it started.
    void kill() override { killed(grave_); }

    // Hangs up its terminal; a program on pipes has none.
    void hang_up() override {
        if (held_->terminal) hung(grave_);
    }

    // Tells a program in a terminal its new size.
    void resize(int columns, int rows) override { resized(held_, Size{columns, rows}); }

private:
    std::shared_ptr<Living> held_;
    std::shared_ptr<Grave> grave_;
};

// A program that is nobody's child: let loose, or a file or an address opened by the default
// program. It is lost to us at once.
class Loose : public Spawned {
public:
    // The number of the process let loose, or 0 when there is none.
    explicit Loose(std::int64_t which) : which_(which) {}

    // The number of the process.
    [[nodiscard]] std::int64_t id() const override { return which_; }

    // Lost at once: nobody buries it here.
    Task<Exit> ended() override {
        return []() -> Task<Exit> { co_return Exit{Exit::Ended::lost, 0, 0}; }();
    }

    // A program let loose is not ours to stop.
    void stop() override {}

    // Nor to kill.
    void kill() override {}

    // Nor to hang up.
    void hang_up() override {}

    // Nor has it a terminal.
    void resize(int, int) override {}

private:
    std::int64_t which_ = 0;
};

// The reading end of a stream that has already ended.
Stream ended_stream() { return detail::piped().first; }

// The writing end of a stream nobody reads.
Stream deaf_stream() {
    auto [reading, writing] = detail::piped();

    reading.close();

    return writing;
}

// The port's command in the core's terms.
Command ordered(const ::tal::Command& what) {
    Command made;

    made.program = core::native(what.program);
    made.arguments = what.arguments;
    made.where = what.folder.empty() ? path{} : core::native(what.folder);
    made.environment = what.environment;
    made.merge = what.merged;
    made.memory = what.memory;

    return made;
}

// Opens a file or an address with the program the system has for it: the shell on Windows, open on
// macOS, xdg-open elsewhere, let loose either way.
std::int64_t opened(const ::tal::Command& what) {
    const std::string address = what.arguments.empty() ? std::string{what.program.stored()} : what.arguments.front();

#ifdef _WIN32
    const std::wstring wide = ::tal::text::wide(address);

    SHELLEXECUTEINFOW how{};

    how.cbSize = sizeof how;
    how.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    how.lpVerb = L"open";
    how.lpFile = wide.c_str();
    how.nShow = SW_SHOWNORMAL;

    if (ShellExecuteExW(&how) == 0) core::amiss("Cannot open it.", path{wide});

    return 0;
#else
    Command loose;

#ifdef __APPLE__
    loose.program = "open";
#else
    loose.program = "xdg-open";
#endif
    loose.arguments = {address};

    return loosed(loose);
#endif
}

// The processes of the system.
class System : public Processes {
public:
    // The adapter's name.
    [[nodiscard]] std::string_view name() const override { return "system"; }

    // Starts the program: on pipes, in a terminal of its own, let loose, or a file opened.
    Task<Process> start(::tal::Command what) override {
        return offloaded([what = std::move(what)]() -> Process {
            if (what.mode == ::tal::Mode::opened) return Process{std::make_shared<Loose>(opened(what)), deaf_stream(), ended_stream(), ended_stream()};

            if (what.mode == ::tal::Mode::detached) return Process{std::make_shared<Loose>(loosed(ordered(what))), deaf_stream(), ended_stream(), ended_stream()};

            if (what.mode == ::tal::Mode::terminal) {
                if (!teletyped()) throw Trouble{::tal::Kind::unsupported, "This system has no pseudoconsole.", what.program.text()};

                const std::shared_ptr<Living> held = teletype(ordered(what), Size{what.columns, what.rows});

                return Process{std::make_shared<Alive>(held), Stream{std::make_shared<Telling>(held)},
                               Stream{std::make_shared<Hearing>(held, false)}, ended_stream()};
            }

            const std::shared_ptr<Living> held = reared(ordered(what), false);

            return Process{std::make_shared<Alive>(held), Stream{std::make_shared<Telling>(held)}, Stream{std::make_shared<Hearing>(held, false)},
                           what.merged ? ended_stream() : Stream{std::make_shared<Hearing>(held, true)}};
        });
    }
};

}

namespace core {

// The file of our own program, as the system knows it. Throws `Trouble` unsupported where the
// system cannot tell.
path own_program() {
#ifdef _WIN32
    std::wstring own(32768, L'\0');

    const DWORD much = GetModuleFileNameW(nullptr, own.data(), static_cast<DWORD>(own.size()));

    if (much == 0 || much >= own.size()) throw Trouble{::tal::Kind::unsupported, "The file of this program is not known."};

    own.resize(much);

    return path{own};
#elif defined(__APPLE__)
    std::uint32_t much = 0;

    (void)_NSGetExecutablePath(nullptr, &much);

    std::string own(much, '\0');

    if (much == 0 || _NSGetExecutablePath(own.data(), &much) != 0) throw Trouble{::tal::Kind::unsupported, "The file of this program is not known."};

    own.resize(std::strlen(own.c_str()));

    std::error_code why;
    const path made = stood::canonical(path{own}, why);

    if (why) throw Trouble{::tal::Kind::unsupported, "The file of this program is not known."};

    return made;
#elif defined(__linux__)
    std::error_code why;
    const path made = stood::read_symlink("/proc/self/exe", why);

    if (why) throw Trouble{::tal::Kind::unsupported, "The file of this program is not known."};

    return made;
#else
    throw Trouble{::tal::Kind::unsupported, "The file of this program is not known on this system."};
#endif
}

}

}

namespace tal {

// The built-in adapter of the port: the processes of the system, one for the whole program.
std::shared_ptr<Processes> Processes::builtin() {
    static const std::shared_ptr<Processes> made = std::make_shared<platform::System>();

    return made;
}

}
