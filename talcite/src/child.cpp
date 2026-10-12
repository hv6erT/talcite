#include "talcite/child.hpp"

#include "talcite/encoding.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "talcite/pool.hpp"
#include "talcite/term.hpp"

#include "launching.hpp"
#include "sealed.hpp"

#ifdef _WIN32

#include <windows.h>

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
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

#endif

namespace talcite {

namespace {

// The read chunk. It is the same one `fs` uses on a network share: a pipe has no size one could ask
// about, so the only thing we can choose is how often to come back for the answer to "am I still
// needed".
constexpr std::size_t brim = 64 * 1024;

// Throws `fs::Trouble` with the text and the program's path.
[[noreturn]] void amiss(const std::string& said, const fs::path& where) {
    throw fs::Trouble{said, where};
}

}

// A child is made in one place in this file, but it has to be wrapped by someone with access to the
// private constructor. Instead of opening it to everyone, we open it to one class that does not
// exist outside this file.
class Rearing {
public:
    // Wraps the shared state into a `Child`.
    [[nodiscard]] static Child made(std::shared_ptr<Child::Held> held) {
        return Child{std::move(held)};
    }

    // The shared state inside a `Child`.
    [[nodiscard]] static const std::shared_ptr<Child::Held>& inside(const Child& one) {
        return one.held_;
    }
};

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

    // Closes the process and job handles.
    ~Grave() {
        if (process != nullptr) CloseHandle(process);
        if (job != nullptr) CloseHandle(job);
    }
#endif
};

struct Child::Held {
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

    std::optional<Promise<Exit>> waiting;

    fs::path program;

    ~Held();
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
Child::Held::~Held() {
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
Bytes sipped(Child::Held& held, bool errors, std::size_t most) {
    Bytes made;

    if (most == 0) return made;

#ifdef _WIN32

    HANDLE one = nullptr;

    {
        const std::lock_guard<std::mutex> kept{held.guard};

        one = errors ? held.wrong : held.from;
    }

    if (one == nullptr) return made;

    // Our end of the pipe is overlapped (`reared`), so the read waits for its completion and for the
    // worker's bell at once, the same shape as `poll` on POSIX: a cancelled read wakes at once,
    // and an idle one costs nothing while it waits.
    const HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    if (ready == nullptr) amiss("Cannot read from the program.", held.program);

    // It is closed on every way out, including an exception from `amiss`.
    const std::shared_ptr<void> closing{ready, [](void* which) { CloseHandle(which); }};

    Bell* bell = ringing();

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

            const HANDLE these[2] = {ready, bell != nullptr ? static_cast<HANDLE>(bell->waited()) : nullptr};
            const DWORD many = bell != nullptr ? 2 : 1;

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

    Bell* bell = ringing();

    for (;;) {
        pollfd watched[2];
        int many = 1;

        watched[0] = pollfd{one, POLLIN, 0};

        if (bell != nullptr) {
            watched[1] = pollfd{bell->waited(), POLLIN, 0};
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
    explicit Writing(Child::Held& held) : held_(held) {}

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
    Child::Held& held_;
};

// Writes all of the bytes into the program's input, in chunks of at most `brim`, and returns how
// many were written. Throws when the input is closed, before or during the write, or the write
// fails; throws `Cancelled` when the worker's bell rings.
std::size_t poured(Child::Held& held, const Bytes& what) {
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

    Bell* bell = ringing();

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

            if (bell != nullptr) {
                rung = many;
                these[many++] = static_cast<HANDLE>(bell->waited());
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

    Bell* bell = ringing();

    std::size_t done = 0;

    while (done < what.size()) {
        // Three things at once: the child's input, the worker's bell and the wake-up pipe. The bell
        // turns `cancel()` into an interrupted wait, and the wake-up pipe does the same for
        // `close_input()`. Without the third, a writer stuck on a full pipe would wake only when
        // the child reads something or dies.
        pollfd watched[3];
        int many = 1;

        watched[0] = pollfd{one, POLLOUT, 0};

        if (bell != nullptr) {
            watched[many] = pollfd{bell->waited(), POLLIN, 0};
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

        if (bell != nullptr && (watched[1].revents & POLLIN) != 0) throw Cancelled{};

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
Exit buried(const std::shared_ptr<Grave>& grave, const fs::path& program) {
    Exit made;

#ifdef _WIN32

    HANDLE one = nullptr;

    {
        const std::lock_guard<std::mutex> kept{grave->guard};

        one = grave->process;
    }

    if (one == nullptr) {
        made.ended = Ended::lost;

        return made;
    }

    if (WaitForSingleObject(one, INFINITE) != WAIT_OBJECT_0)
        amiss("Cannot wait for the program to end.", program);

    DWORD code = 0;

    {
        const std::lock_guard<std::mutex> kept{grave->guard};

        grave->gone = true;

        if (GetExitCodeProcess(one, &code) == 0) {
            made.ended = Ended::lost;

            return made;
        }
    }

    // A program we killed ends with the code we gave it ourselves. Windows has no signals, so there
    // is nothing to tell apart here; what remains is an ordinary exit with a code.
    made.ended = Ended::normal;
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

        made.ended = Ended::lost;

        return made;
    }

    // Buried. From this moment the number may be given to someone else, so any signal sent to it
    // would hit a stranger's program, and that is why `stop()` and `kill()` ask for the same lock.
    {
        const std::lock_guard<std::mutex> kept{grave->guard};

        grave->gone = true;
    }

    if (WIFEXITED(told)) {
        made.ended = Ended::normal;
        made.code = WEXITSTATUS(told);
    } else if (WIFSIGNALED(told)) {
        made.ended = Ended::killed;
        made.signal = WTERMSIG(told);
    } else {
        made.ended = Ended::lost;
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
std::wstring sought(const fs::path& program) {
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
        std::wstring own(32768, L'\0');

        const DWORD much = GetModuleFileNameW(nullptr, own.data(), static_cast<DWORD>(own.size()));

        if (much > 0 && much < own.size()) {
            own.resize(much);
            places.push_back(fs::path{own}.parent_path().wstring());
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
            const std::wstring tried = place.empty() ? named + ending : (fs::path{place} / (named + ending)).wstring();

            const DWORD told = GetFileAttributesW(tried.c_str());

            if (told != INVALID_FILE_ATTRIBUTES && (told & FILE_ATTRIBUTE_DIRECTORY) == 0) return tried;
        }

    // Not found: we refuse here. A bare name passed on would make `CreateProcessW` go back to the
    // current folder, exactly where we must not take it from.
    amiss("Cannot start the program.", program);
}

// Whether the program is a batch file (.bat or .cmd), which only cmd.exe can run.
bool batched(const std::wstring& program) {
    std::wstring ending = fs::path{program}.extension().wstring();

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
void batch_quoted(const std::wstring& one, std::wstring& into, const fs::path& program) {
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

        const std::wstring shell = (fs::path{system} / L"cmd.exe").wstring();

        made.line = L"cmd.exe /d /v:off /s /c \"";

        batch_quoted(made.application, made.line, what.program);

        for (const std::string& one : what.arguments) {
            made.line += L' ';

            batch_quoted(to_wstring(one), made.line, what.program);
        }

        made.line += L'"';
        made.application = shell;

        return made;
    }

    quoted(what.program.wstring(), made.line);

    for (const std::string& one : what.arguments) {
        made.line += L' ';

        quoted(to_wstring(one), made.line);
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
        const std::wstring name = to_wstring(one.name);
        const std::wstring said = name + L'=' + to_wstring(one.value);

        const auto at = std::find_if(among.begin(), among.end(), [&name](const std::wstring& had) {
            return had.size() > name.size() && had[name.size()] == L'=' &&
                   _wcsnicmp(had.c_str(), name.c_str(), name.size()) == 0;
        });

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
std::string found(const fs::path& program) {
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
        const std::string said = one.name + "=" + one.value;

        const auto at = std::find_if(among.begin(), among.end(), [&one](const std::string& had) {
            return had.size() > one.name.size() && had[one.name.size()] == '=' &&
                   had.compare(0, one.name.size(), one.name) == 0;
        });

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

// Starting. `ahead` means "this program gets our terminal": then we create no pipes at all, because
// its input is the keyboard and its output the screen, and we also put it in the foreground, so
// that Ctrl-C goes to it and not to us.
std::shared_ptr<Child::Held> reared(const Command& what, bool ahead) {
    auto held = std::make_shared<Child::Held>();

    held->program = what.program;
    held->merge = what.merge;

    if (what.program.empty()) amiss("No program given.", what.program);

#ifdef _WIN32

    HANDLE in_read = nullptr;
    HANDLE in_write = nullptr;
    HANDLE out_read = nullptr;
    HANDLE out_write = nullptr;
    HANDLE err_read = nullptr;
    HANDLE err_write = nullptr;

    SECURITY_ATTRIBUTES how{};
    how.nLength = sizeof(how);
    how.bInheritHandle = TRUE;

    const auto undone = [&] {
        closed(in_read);
        closed(in_write);
        closed(out_read);
        closed(out_write);
        closed(err_read);
        closed(err_write);
    };

    if (!ahead) {
        // All three pipes have an overlapped end on our side (`paired`): the writer waits for the
        // completion and for the wake-up event of `close_input()`, the readers for the completion
        // and for the worker's bell, so both a sealed input and a cancelled read or write wake at
        // once instead of waiting for the child.
        if (!paired(false, how, in_write, in_read)) {
            undone();
            amiss("Cannot create the input pipe.", what.program);
        }

        if (!paired(true, how, out_read, out_write)) {
            undone();
            amiss("Cannot create the output pipe.", what.program);
        }

        if (!what.merge && !paired(true, how, err_read, err_write)) {
            undone();
            amiss("Cannot create the error pipe.", what.program);
        }

        // Our ends must not be inherited: a child holding the write end of its own output would
        // never see the end of the pipe.
        SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

        // The wake-up event for `close_input()`. If it cannot be created, only the deferred close
        // remains: the writer learns about the sealing on the next chunk instead of at once. That
        // is a degradation, not a failure, so we do not reject the child for it.
        held->rouse = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        if (err_read != nullptr) SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
    }

    Launch launch;

    try {
        launch = commanded(what);
    } catch (...) {
        undone();
        throw;
    }

    std::wstring around = surrounded(what);
    const std::wstring where = what.where.empty() ? std::wstring{} : what.where.wstring();

    STARTUPINFOEXW setting{};

    // The size has to match what we really pass: the extended form only when we add the handle
    // list. Given without that flag, it would be a lie about the size of the structure.
    setting.StartupInfo.cb =
        ahead ? static_cast<DWORD>(sizeof(STARTUPINFOW)) : static_cast<DWORD>(sizeof(setting));

    std::vector<char> room;
    HANDLE passed[3] = {nullptr, nullptr, nullptr};

    if (!ahead) {
        setting.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        setting.StartupInfo.hStdInput = in_read;
        setting.StartupInfo.hStdOutput = out_write;
        setting.StartupInfo.hStdError = what.merge ? out_write : err_write;

        // Handle inheritance on Windows is all or nothing, unless a list is given. Without it the
        // child would also get our sockets and keep a port busy long after we close it.
        passed[0] = in_read;
        passed[1] = out_write;
        passed[2] = what.merge ? out_write : err_write;

        SIZE_T much = 0;

        InitializeProcThreadAttributeList(nullptr, 1, 0, &much);

        room.resize(much);

        setting.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(room.data());

        if (InitializeProcThreadAttributeList(setting.lpAttributeList, 1, 0, &much) == 0) {
            undone();
            amiss("Cannot prepare the handle list.", what.program);
        }

        if (UpdateProcThreadAttribute(setting.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      passed, sizeof(HANDLE) * (what.merge ? 2 : 3), nullptr,
                                      nullptr) == 0) {
            DeleteProcThreadAttributeList(setting.lpAttributeList);
            undone();
            amiss("Cannot pass the handle list.", what.program);
        }
    }

    PROCESS_INFORMATION told{};

    // Its own process group for a program on pipes, so that `stop()` has somewhere to send
    // Ctrl-Break. A program that gets our terminal stays in our group, because otherwise Ctrl-C
    // pressed by the user would not reach it.
    //
    // It starts suspended, so that it enters the job before it starts anything itself.
    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED |
                        (ahead ? 0u : (EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_PROCESS_GROUP));

    const BOOL done =
        CreateProcessW(launch.application.c_str(), launch.line.data(), nullptr, nullptr, ahead ? FALSE : TRUE,
                       flags, around.data(), where.empty() ? nullptr : where.c_str(), &setting.StartupInfo, &told);

    if (!ahead) DeleteProcThreadAttributeList(setting.lpAttributeList);

    if (done == 0) {
        undone();
        amiss("Cannot start the program.", what.program);
    }

    // A job that cannot be made (the process is already in someone else's job without the right to
    // leave, an old system) is no reason to refuse: `kill()` then kills only the program itself.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);

    if (job != nullptr && AssignProcessToJobObject(job, told.hProcess) == 0) {
        CloseHandle(job);
        job = nullptr;
    }

    ResumeThread(told.hThread);
    CloseHandle(told.hThread);

    closed(in_read);
    closed(out_write);
    closed(err_write);

    held->into = in_write;
    held->from = out_read;
    held->wrong = err_read;

    held->grave->which = static_cast<std::int64_t>(told.dwProcessId);
    held->grave->process = told.hProcess;
    held->grave->job = job;
    held->grave->grouped = !ahead;

#else

    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int starting[2] = {-1, -1};

    const auto undone = [&] {
        for (int* one : {&in_pipe[0], &in_pipe[1], &out_pipe[0], &out_pipe[1], &err_pipe[0],
                         &err_pipe[1], &starting[0], &starting[1]})
            closed(*one);
    };

    // All three pipes are closed on exec from birth (`sealed::piped`): another program starting at
    // the same moment from another thread will not get the ends of this one. The child moves its
    // ends to 0, 1 and 2, and `dup2` does not carry the flag over.
    if (!ahead) {
        if (!sealed::piped(in_pipe, false)) amiss("Cannot create the input pipe.", what.program);

        if (!sealed::piped(out_pipe, false)) {
            undone();
            amiss("Cannot create the output pipe.", what.program);
        }

        if (!what.merge && !sealed::piped(err_pipe, false)) {
            undone();
            amiss("Cannot create the error pipe.", what.program);
        }
    }

    // The start pipe, also closed on exec. A successful `execve` closes the child's end and we
    // read the end of the pipe; a failed `chdir` or `execve` writes its errno there first. Without
    // it a program that cannot be started would start anyway and end with 127, as from a shell,
    // while on Windows the same command is a refusal; this way both systems refuse alike, and with
    // the reason.
    if (!sealed::piped(starting, false)) {
        undone();
        amiss("Cannot create the start pipe.", what.program);
    }

    // Everything the child will need is put together here. After the fork it may call only what is
    // async-signal-safe, so there is no memory allocation or string building there anymore.
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

    // The reason of a failed start, for the parent. No memory allocation: this runs between `fork`
    // and `exec`.
    const auto say = [&starting](int why) { (void)!::write(starting[1], &why, sizeof why); };

    const pid_t which = ::fork();

    if (which < 0) {
        undone();
        amiss("Cannot start the program.", what.program);
    }

    if (which == 0) {
        if (ahead) {
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

            (void)::dup2(in_pipe[0], STDIN_FILENO);
            (void)::dup2(out_pipe[1], STDOUT_FILENO);
            (void)::dup2(what.merge ? out_pipe[1] : err_pipe[1], STDERR_FILENO);

            for (const int one : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1], err_pipe[0],
                                  err_pipe[1]})
                if (one > STDERR_FILENO) ::close(one);

            // A pipe end that already was one of 0, 1, 2 (a parent without standard input gets
            // exactly zero from `pipe2`) is left by `dup2` as it was, with the flag, and the flag
            // would close it on `exec`.
            for (const int one : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) ::fcntl(one, F_SETFD, 0);
        }

        // Signals we set to ignored stay ignored after `exec`, and a program that inherited an
        // ignored SIGPIPE behaves differently from one started from a shell.
        std::signal(SIGPIPE, SIG_DFL);

        if (!where.empty() && ::chdir(where.c_str()) != 0) {
            say(errno);

            ::_exit(127);
        }

        ::execve(program.c_str(), argv.data(), envp.data());

        say(errno);

        ::_exit(127);
    }

    // The child does the same, and both do it on purpose. Whoever is first sets it; the second call
    // changes nothing. Otherwise a gap remains in which the child already reads and the foreground
    // is not there yet, or `kill()` hits a group that does not exist yet.
    ::setpgid(which, which);

    if (ahead) {
        void (*was)(int) = std::signal(SIGTTOU, SIG_IGN);

        (void)::tcsetpgrp(STDIN_FILENO, which);

        std::signal(SIGTTOU, was == nullptr ? SIG_DFL : was);
    }

    // Waits for the child to either execute the program or say why it could not.
    closed(starting[1]);

    int why = 0;
    std::size_t heard = 0;

    while (heard < sizeof why) {
        const ssize_t much = ::read(starting[0], reinterpret_cast<char*>(&why) + heard, sizeof why - heard);

        if (much > 0) {
            heard += static_cast<std::size_t>(much);

            continue;
        }

        if (much < 0 && errno == EINTR) continue;

        break;
    }

    closed(starting[0]);

    if (heard == sizeof why) {
        // The child ends at once; it is buried here, since no wait for it exists yet. The terminal
        // it took comes back to us, because nobody will run `Term::execute` to its end.
        int status = 0;

        while (::waitpid(which, &status, 0) < 0 && errno == EINTR) {
        }

        if (ahead) {
            void (*was)(int) = std::signal(SIGTTOU, SIG_IGN);

            (void)::tcsetpgrp(STDIN_FILENO, ::getpgrp());

            std::signal(SIGTTOU, was == nullptr ? SIG_DFL : was);
        }

        undone();

        throw fs::Trouble{"Cannot start the program: " + std::string{std::strerror(why)} + ".", what.program};
    }

    closed(in_pipe[0]);
    closed(out_pipe[1]);
    closed(err_pipe[1]);

    held->into = in_pipe[1];
    held->from = out_pipe[0];
    held->wrong = err_pipe[0];

    // Our pipe ends are non-blocking. The read and write loops are written for this (they ask
    // `poll`, handle `EAGAIN` and keep the bell next to them), but without this setting `EAGAIN`
    // never comes, because a blocking write to a pipe returns only after handing over everything it
    // was asked to. The pool worker then stands inside a system call, where neither the bell nor
    // `close_input()` can reach it: `close_input()` on a child that does not read would end the
    // write only when the child dies.
    //
    // The setting applies only to our ends. A pipe has two separate open file descriptions (we hold
    // the input for writing and the two outputs for reading, the child holds the other ends), so
    // the child gets its ends blocking, as every program expects.
    for (const int one : {held->into, held->from, held->wrong}) {
        if (one < 0) continue;

        const int flags = ::fcntl(one, F_GETFL, 0);

        if (flags >= 0) (void)::fcntl(one, F_SETFL, flags | O_NONBLOCK);
    }

    // The wake-up pipe for `close_input()`. If it cannot be created, only the deferred close
    // remains: the writer learns about the sealing on the next turn of the loop instead of at once.
    // That is a degradation, not a failure, so we do not reject the child for it.
    if (!sealed::piped(held->rouse, true)) held->rouse[0] = held->rouse[1] = -1;

    held->grave->which = static_cast<std::int64_t>(which);
    held->grave->foreground = ahead;

#endif

    const std::shared_ptr<Grave> grave = held->grave;
    const fs::path program_named = what.program;

    held->waiting = Promise<Exit>{[grave, program_named](auto resolve, Reject reject) {
        try {
            resolve(buried(grave, program_named));
        } catch (...) {
            reject(std::current_exception());
        }
    }};

    // We start right away, not on the first question about the end. Promises in talcite are lazy,
    // so a child whose end nobody ever asks about would not be buried and would leave a zombie in
    // the process table. And the right to let go of the handle without asking anything is exactly
    // what `close()` promises.
    held->waiting->start();

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
        throw fs::Trouble{"Cannot start the program: " +
                              (failed > 0 ? std::string{std::strerror(static_cast<int>(failed))} : std::string{"no process"}) + ".",
                          what.program};

    return pid;

#endif
}

// Kills the program on the way out, unless told it may live (`spare`). `Child::run` leaves through
// it by every exception, a cancellation above all.
class Killing {
public:
    explicit Killing(const Child& one) : one_(one) {}

    // Kills the program, unless spared.
    ~Killing() {
        if (!spared_) one_.kill();
    }

    Killing(const Killing&) = delete;
    Killing& operator=(const Killing&) = delete;
    Killing(Killing&&) = delete;
    Killing& operator=(Killing&&) = delete;

    // Lets the program live.
    void spare() { spared_ = true; }

private:
    const Child& one_;
    bool spared_ = false;
};

// A loop reading one pipe to the end. A separate promise for each side, because both have to be
// read at once: a program that fills the one we are not looking at will stop and never end.
[[nodiscard]] Promise<void> sipping(std::shared_ptr<Child::Held> held, bool errors,
                                    std::shared_ptr<Bytes> into) {
    return Promise<void>{[held, errors, into](auto resolve, Reject reject) {
        try {
            for (;;) {
                Bytes got = sipped(*held, errors, brim);

                if (got.empty()) break;

                into->insert(into->end(), got.begin(), got.end());
            }

            resolve();
        } catch (...) {
            reject(std::current_exception());
        }
    }};
}

}

// Starts a program let loose and resolves to its process number; see `loosed`.
Promise<std::int64_t> Child::detached(Command what) {
    return Promise<std::int64_t>{[what = std::move(what)](auto resolve, Reject reject) {
        try {
            resolve(loosed(what));
        } catch (...) {
            reject(std::current_exception());
        }
    }};
}

// Starts a child program on pipes.
Promise<Child> Child::start(Command what) {
    return Promise<Child>{[what = std::move(what)](auto resolve, Reject reject) {
        try {
            resolve(Rearing::made(reared(what, false)));
        } catch (...) {
            reject(std::current_exception());
        }
    }};
}

// The child's process number, or -1 for an empty handle.
std::int64_t Child::pid() const { return held_ == nullptr ? -1 : held_->grave->which; }

// Writes the bytes to the child's input on the pool and resolves to how many were written; 0 for an
// empty handle.
Promise<std::size_t> Child::write(Bytes what) const {
    std::shared_ptr<Held> held = held_;

    return Promise<std::size_t>{
        [held, what = std::move(what)](auto resolve, Reject reject) mutable {
            try {
                if (held == nullptr) {
                    resolve(0);

                    return;
                }

                resolve(poured(*held, what));
            } catch (...) {
                reject(std::current_exception());
            }
        }};
}

// Closes the child's input. The last writer to leave closes it, not whoever asked. As long as any
// write sits in a system call on this descriptor, closing would give its number back to the system,
// which hands numbers out again at once, so content meant for the child could land in someone
// else's file. Instead we set a flag and wake the waiting writers; whoever leaves last closes it.
void Child::close_input() const {
    if (held_ == nullptr) return;

    const std::lock_guard<std::mutex> kept{held_->guard};

    if (held_->sealing) return;

    held_->sealing = true;

    if (held_->writers == 0) {
        closed(held_->into);

        return;
    }

#ifdef _WIN32

    // A manual-reset event: once raised it stays raised, so it wakes every writer, not just the
    // first, just like the byte in the wake-up pipe on POSIX.
    if (held_->rouse != nullptr) (void)SetEvent(held_->rouse);

#else

    // The byte stays in the pipe forever and nobody collects it: the pipe is level-triggered, so it
    // wakes everyone who looks at it, not just the first.
    if (held_->rouse[1] >= 0) {
        const char one = 0;

        // The result does not matter: the pipe is empty and a one-byte write cannot fail to go
        // through, and if it did after all, the deferred close and the next turn of the loop
        // remain.
        if (::write(held_->rouse[1], &one, 1) < 0) return;
    }

#endif
}

// Reads at most `most` bytes of the child's output on the pool; empty at the end of the output or
// for an empty handle.
Promise<Bytes> Child::read(std::size_t most) const {
    std::shared_ptr<Held> held = held_;

    return Promise<Bytes>{[held, most](auto resolve, Reject reject) {
        try {
            if (held == nullptr) {
                resolve(Bytes{});

                return;
            }

            resolve(sipped(*held, false, most));
        } catch (...) {
            reject(std::current_exception());
        }
    }};
}

// Reads at most `most` bytes of the child's error output on the pool; empty at the end of it or for
// an empty handle.
Promise<Bytes> Child::read_errors(std::size_t most) const {
    std::shared_ptr<Held> held = held_;

    return Promise<Bytes>{[held, most](auto resolve, Reject reject) {
        try {
            if (held == nullptr) {
                resolve(Bytes{});

                return;
            }

            resolve(sipped(*held, true, most));
        } catch (...) {
            reject(std::current_exception());
        }
    }};
}

// The child's exit. Every caller gets its own view of the one shared wait, shielded from it: a
// caller who gives up (a timeout, a cancelled chain) cancels only that view. Handing out the shared
// promise itself would let the first impatient caller cancel the burial for everyone; later callers
// would then get `Cancelled` instead of the exit code, and the dead child would stay unburied. An
// empty handle resolves to `Ended::lost` at once.
Promise<Exit> Child::wait() const {
    if (held_ == nullptr || !held_->waiting.has_value())
        return Promise<Exit>{[](auto resolve, Reject) { resolve(Exit{Ended::lost, 0, 0}); }};

    return held_->waiting->shielded();
}

// Asks the program to end: SIGTERM to its whole group on POSIX, Ctrl-Break on Windows. Does nothing
// once the program is buried.
void Child::stop() const {
    if (held_ == nullptr) return;

    const std::shared_ptr<Grave> grave = held_->grave;

    const std::lock_guard<std::mutex> kept{grave->guard};

    if (grave->gone) return;

#ifdef _WIN32

    // A console event goes to the whole group, so it may be sent only to a program for which we
    // made a group. The one that got our terminal sits in ours: a Ctrl-Break sent to its number
    // would kill us along with it. For that one only `kill()` remains.
    if (grave->grouped)
        (void)GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, static_cast<DWORD>(grave->which));

#else

    // The program's whole group: the shell and what it started.
    (void)::kill(-static_cast<pid_t>(grave->which), SIGTERM);

#endif
}

// Kills the program at once: SIGKILL to its whole group on POSIX, the whole job (or the process
// alone) on Windows. Does nothing once the program is buried.
void Child::kill() const {
    if (held_ == nullptr) return;

    const std::shared_ptr<Grave> grave = held_->grave;

    const std::lock_guard<std::mutex> kept{grave->guard};

    if (grave->gone) return;

#ifdef _WIN32

    if (grave->job != nullptr) (void)TerminateJobObject(grave->job, 1);
    else if (grave->process != nullptr) (void)TerminateProcess(grave->process, 1);

#else

    (void)::kill(-static_cast<pid_t>(grave->which), SIGKILL);

#endif
}

// Letting go of the handle does not kill the program. Killing in the destructor would be a
// surprise: whoever wants to kill has `stop()` and `kill()`, and whoever has merely lost interest
// leaves it to its fate. The worker waiting for the end will finish and bury it.
void Child::close() { held_.reset(); }

// Runs the program to the end on pipes: feeds it the input, collects both outputs and resolves to
// them with the exit.
//
// A program may stop reading before it has taken the whole input: it has what it needs, or it does
// not read at all. That is not a failure of the run but the end of the input; the outputs and the
// exit code still come, and they say how the program fared. And a run abandoned on the way (a
// cancellation, above all) kills the program, which otherwise would go on with nobody to read it.
// The program is started on this worker, not through `start`, so that no moment exists in which it
// runs and no handle to kill it is held.
Promise<Output> Child::run(Command what, Bytes input) {
    return Promise<Output>{[what = std::move(what), input = std::move(input)](auto resolve,
                                                                       Reject reject) mutable {
        try {
            const Child one = Rearing::made(reared(what, false));

            Killing guarded{one};

            const std::shared_ptr<Child::Held>& held = Rearing::inside(one);

            auto read = std::make_shared<Bytes>();
            auto errors = std::make_shared<Bytes>();

            // Reading starts before writing. The other order deadlocks on input larger than the
            // pipe: we wait for the child to take it, and the child waits for us to take what it
            // has managed to print.
            Promise<void> reading = sipping(held, false, read);
            Promise<void> moaning =
                what.merge ? Promise<void>{[](auto done, Reject) { done(); }}
                           : sipping(held, true, errors);

            Promise<void> both = Promise<void>::all(reading, moaning);

            both.start();

            if (!input.empty()) {
                try {
                    (void)one.write(std::move(input)).await();
                } catch (const fs::Trouble&) {
                    // The program stopped reading; its input ends here.
                }
            }

            one.close_input();

            both.await();

            Output made;

            made.exit = one.wait().await();
            made.out = std::move(*read);
            made.err = std::move(*errors);

            guarded.spare();

            resolve(std::move(made));
        } catch (...) {
            reject(std::current_exception());
        }
    }};
}

#ifdef _WIN32

// The whole command line with the program's full path at its head, for places that take one string
// and start it themselves later, such as the Run key (`login.cpp`). The quoting rules are the same
// as for starting: a batch file through cmd.exe from the system folder, every argument in quotation
// marks.
std::wstring launched(const Command& what) {
    const Launch made = commanded(what);

    std::wstring line;

    quoted(made.application, line);

    if (batched(sought(what.program))) {
        line += made.line.substr(std::wstring_view{L"cmd.exe"}.size());

        return line;
    }

    for (const std::string& one : what.arguments) {
        line += L' ';

        quoted(to_wstring(one), line);
    }

    return line;
}

#endif

// Hands the terminal to the program and waits for it to end, with our drawing paused.
Exit Term::execute(Command what) {
    const Pause step{*this};

    const Child one = Rearing::made(reared(what, true));

    const Exit made = one.wait().await();

#ifndef _WIN32

    // The terminal comes back to us. Without this it would stay with a group that no longer exists,
    // and our first read would end in a stop by SIGTTIN.
    void (*was)(int) = std::signal(SIGTTOU, SIG_IGN);

    (void)::tcsetpgrp(STDIN_FILENO, ::getpgrp());

    std::signal(SIGTTOU, was == nullptr ? SIG_DFL : was);

#endif

    return made;
}

}
